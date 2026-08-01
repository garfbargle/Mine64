#include <nusys.h>
#include "main.h"
#include "menu.h"
#include "camera.h"
#include "audio.h"
#include "player.h"
#include "geometry.h"
#include "graphics.h"
#include "items.h"
#include "mobs.h"
#include "storage.h"
#include "day_cycle.h"
#include "details.h"
#include "edits.h"

/*
 * Replacing the world rewrites every column display list in the mesh arena the
 * RSP renders terrain from.  That may only happen when NuSystem reports no
 * graphics task in flight, and it must finish before the next task is
 * submitted -- otherwise the RSP walks commands being overwritten underneath
 * it.
 *
 * Generating a world is also roughly two seconds of noise sampling and
 * compiling its mesh is about another, both far longer than a retrace.  Since
 * nothing else on this thread submits frames, doing either in one pass stops
 * the picture and the controller dead.  So the job runs in slices: the
 * outgoing world keeps orbiting out of the arena that is still on screen while
 * its replacement is generated and compiled into the other one.
 */
#define WORLD_JOB_IDLE 0
#define WORLD_JOB_LOAD 1
#define WORLD_JOB_GENERATE 2
#define WORLD_JOB_MESH 3

#define WORLD_JOB_PREVIEW 0
#define WORLD_JOB_GAME 1

/*
 * Budgets are per graphics callback.  A terrain column costs several octave
 * samples plus a cave test for most of its height, and a mesh column costs a
 * full greedy pass over four chunks, so these are deliberately small: the
 * point is a world that arrives smoothly, not one that arrives fastest.
 */
/* Counted in chunk columns now, not block columns: one step builds a whole
   8x32x8 column, which is roughly what 48 block columns used to cost. */
#define WORLD_GEN_COLUMNS_PER_STEP 1
#define WORLD_MESH_COLUMNS_PER_STEP 3
/* One x-slab of a save per callback: MAX_Y * MAX_Z / 2 bytes, which is three
   and a half 512-byte cart reads.  MAX_X slabs at 60 Hz puts a whole save on
   screen in under two seconds without any single frame paying for it. */
#define WORLD_LOAD_SLABS_PER_STEP 1

/*
 * How the progress bar is divided between a job's two halves.
 *
 * Every stage costs one callback per step and a loading-screen callback is
 * dominated by the frame it draws, so callbacks are the honest unit of
 * "how long is left" -- and the three ways in differ enormously.  Generating
 * is three passes over every chunk column; reading a save is one pass over
 * MAX_X slabs; both then compile the same mesh, which is far shorter than
 * either.  A fixed three-quarters split spent 90% of a generate on the first
 * 75% of the bar and then covered the last quarter in a second, which reads
 * as a stall followed by a jump.  Deriving the split from the budgets above
 * keeps it true when one of them is retuned.
 */
#define WORLD_GEN_STEPS (3 * CHUNKS_X * CHUNKS_Z / WORLD_GEN_COLUMNS_PER_STEP)
#define WORLD_LOAD_STEPS (MAX_X / WORLD_LOAD_SLABS_PER_STEP)
#define WORLD_MESH_STEPS (CHUNKS_X * CHUNKS_Z / WORLD_MESH_COLUMNS_PER_STEP)
#define BAR_SHARE(steps) \
  ((u8) ((steps) * 100 / ((steps) + WORLD_MESH_STEPS)))

/*
 * Streaming budgets, also per graphics callback.  Terrain is the expensive
 * half -- a whole 8x32x8 column of noise -- so it gets one column a frame, the
 * same rate world creation runs at.  Decoration is cheap by comparison, and
 * some of its attempts are spent on columns still waiting for a neighbour, so
 * it gets more.
 */
#define STREAM_TERRAIN_PER_STEP 3
#define STREAM_DECORATE_PER_STEP 4

/*
 * How far (in blocks, per axis) the player may wander from the render origin
 * before it is re-centred on them.  Far enough that rebasing is rare, near
 * enough that origin-relative translations -- terrain out to the mesh ring,
 * entities near the players -- stay well inside the s15.16 Mtx integer
 * range: 256 + a ~64-block ring is ~20500 units of a possible 32767.
 *
 * The far-walk freeze reproduced with rebasing switched off (O=0, B=0 on the
 * frozen screen), so rebasing is not the fault and the bisection threshold
 * of 64 is restored to 256 -- which also means any freeze within ~250 blocks
 * of spawn is automatically rebase-free.
 */
#define REBASE_DISTANCE 256
/* Blocks.  Any legitimate walk stays far inside this; past it the position is
   corrupt and must not be trusted with the render origin. */
#define REBASE_SANITY_LIMIT 100000

static u8 world_job_stage = WORLD_JOB_IDLE;
static u8 world_job_kind;
/* Which slot the running preview is for.  The cursor can move again while a
   world is still building, and the finished preview must not be mistaken for
   the slot now highlighted. */
static u8 world_job_world;
/* Slicing makes a world arrive smoothly but not faster -- it is still seconds
   of noise sampling.  Scrolling past a slot should not pay for it, so wait for
   the cursor to settle before committing to a build. */
#define WORLD_PREVIEW_SETTLE_FRAMES 12
static u8 world_preview_settle;
static u8 world_preview_last_world = 0xFF;
/* The bar only ever moves forwards within one job; see worldJobProgress. */
static u8 world_job_progress_floor;
/* Where this job's mesh stage takes over the bar, from BAR_SHARE. */
static u8 world_job_mesh_start;

static u8 worldGameBuildRequested() {
  return (current_screen == MENU || current_screen == WORLD_NAMING) &&
    menuGameRequested();
}

static u8 worldPreviewBuildRequested() {
  return current_screen == MENU && menuPreviewRequested();
}

u8 worldJobActive() {
  return world_job_stage != WORLD_JOB_IDLE;
}

u8 worldJobProgress() {
  u8 share = world_job_mesh_start;
  u8 progress;

  if (world_job_stage == WORLD_JOB_LOAD) {
    progress = (u8) ((loadGameProgress() * share) / 100);
  } else if (world_job_stage == WORLD_JOB_GENERATE) {
    progress = (u8) ((worldGenerationProgress() * share) / 100);
  } else if (world_job_stage == WORLD_JOB_MESH) {
    progress = (u8) (share +
      (worldMeshBuildProgress() * (100 - share)) / 100);
  } else {
    progress = 100;
  }
  /* A slot that will not load and has to be generated instead is the one job
     that walks two of these in turn, and its second half starts over at zero.
     A bar that runs backwards reads as a fault, and the player has no way to
     know that it is not one. */
  if (progress < world_job_progress_floor) {
    progress = world_job_progress_floor;
  }
  world_job_progress_floor = progress;
  return progress;
}

/* TRUE once the highlighted slot has held still long enough to be worth
   generating.  Entering a world is never delayed by this. */
static u8 previewSelectionSettled() {
  u8 selected = menuSelectedWorld();

  if (selected != world_preview_last_world) {
    world_preview_last_world = selected;
    world_preview_settle = 0;
    return FALSE;
  }
  if (world_preview_settle < WORLD_PREVIEW_SETTLE_FRAMES) {
    world_preview_settle++;
    return FALSE;
  }
  return TRUE;
}

/*
 * Terrain is in memory; give the world its entities and start compiling the
 * display lists.  Shared by the two ways of getting there, because a loaded
 * world and a generated one are the same thing from here on -- except that a
 * save also restored its players, and a fresh world has to be given some.
 */
static void beginWorldMeshStage(u8 place_players) {
  if (place_players) {
    /* Spawn placement reads the finished terrain, so it cannot run until
       generation has completed. */
    initPlayers();
  }
  initDroppedItems();
  initMobs();
  initGeometry();
  world_job_stage = WORLD_JOB_MESH;
  beginWorldMeshBuild(TRUE);
}

/*
 * Route a load status onto the job's next stage.  A save that will not open,
 * will not verify, and has no usable backup leaves the slot empty rather than
 * the screen frozen: the job falls through to generating a fresh world, on
 * the same slices, instead of running initWorld() to completion inside this
 * one callback.
 */
static void applyLoadStatus(u8 status) {
  if (status == LOAD_BUSY) {
    world_job_stage = WORLD_JOB_LOAD;
    return;
  }
  if (status == LOAD_DONE) {
    beginWorldMeshStage(FALSE);
    return;
  }
  beginWorldGeneration();
  world_job_mesh_start = BAR_SHARE(WORLD_GEN_STEPS);
  world_job_stage = WORLD_JOB_GENERATE;
}

static void beginWorldJob() {
  world_job_progress_floor = 0;
  if (worldGameBuildRequested()) {
    /* The picked world is already generated and loaded -- only its display
       lists are still the reduced scenic mesh, so this recompiles them at full
       detail without touching the terrain the player chose.  The mesh is the
       whole job here, so it gets the whole bar. */
    world_job_kind = WORLD_JOB_GAME;
    world_job_mesh_start = 0;
    world_job_stage = WORLD_JOB_MESH;
    beginWorldMeshBuild(FALSE);
    return;
  }

  /* The title menu is also a world picker.  Preparing the highlighted slot
     gives the renderer a real world to orbit rather than a background image. */
  world_job_kind = WORLD_JOB_PREVIEW;
  world_job_world = menuSelectedWorld();
  game_file_num = world_job_world + 1;
  initDetails();
  initWorldEdits();
  if (files_present[world_job_world]) {
    /* The header page only; the 200 KB of packed blocks behind it arrive a
       slab at a time from stepWorldJob. */
    diagPaintPhase(DIAG_PHASE_LOAD);
    world_job_mesh_start = BAR_SHARE(WORLD_LOAD_STEPS);
    applyLoadStatus(beginLoadGame());
  } else {
    beginWorldGeneration();
    world_job_mesh_start = BAR_SHARE(WORLD_GEN_STEPS);
    world_job_stage = WORLD_JOB_GENERATE;
  }
}

static void stepWorldJob() {
  if (world_job_stage == WORLD_JOB_LOAD) {
    diagPaintPhase(DIAG_PHASE_LOAD);
    applyLoadStatus(stepLoadGame(WORLD_LOAD_SLABS_PER_STEP));
    return;
  }

  if (world_job_stage == WORLD_JOB_GENERATE) {
    diagPaintPhase(DIAG_PHASE_GENERATE);
    if (!stepWorldGeneration(WORLD_GEN_COLUMNS_PER_STEP)) {
      return;
    }
    beginWorldMeshStage(TRUE);
    return;
  }

  if (world_job_stage == WORLD_JOB_MESH) {
    diagPaintPhase(DIAG_PHASE_MESH);
    if (!stepWorldMeshBuild(WORLD_MESH_COLUMNS_PER_STEP)) {
      return;
    }
    world_incomplete_message = !worldMeshBuildComplete();
    world_job_stage = WORLD_JOB_IDLE;
    if (world_job_kind == WORLD_JOB_GAME) {
      menuGameStarted();
    } else if (world_job_world == menuSelectedWorld()) {
      beginLoadingPreview();
      menuPreviewLoaded();
    }
    /* Otherwise the cursor moved while this world was building.  Leave the
       request standing so the next callback starts the slot now highlighted;
       the world just finished still renders until it is replaced. */
  }
}

/*
 * One slice of the streaming pipeline around player one: claim and generate
 * terrain, carve the approaching underground, stamp structures, grow trees,
 * and queue finished columns for meshing.
 *
 * This is pure CPU work against window_blocks and the derived record pools.
 * Nothing in it writes the mesh arena, the chunk matrices, or anything else
 * an in-flight RSP task reads -- eviction only redirects the per-slot start
 * pointers (whose old values are already baked into the submitted display
 * list) and returns arena blocks to a free list that only the gated
 * compile/defrag path ever writes into.  That is what makes it legal to run
 * from a callback that found a task still in flight, which used to return
 * having done nothing: at 20 fps two of every three callbacks were pure
 * idle time while the RSP ground through the frame.  Streaming there means
 * walking no longer pays for generation and rendering in series.
 *
 * Mesh compilation, arena defrag and the origin rebase are NOT here; they
 * stay behind the pendingGfx == 0 gate in callbackGfx.
 */
static void stepGameplayStreaming(OSTime work_start, u32 budget_usec,
    u32 urgent_budget_usec) {
  int player_block_x = floor(players[0].position.x / BLOCK_SIZE);
  int player_block_z = floor(players[0].position.z / BLOCK_SIZE);
  int pcx = player_block_x >> CHUNK_SHIFT;
  int pcz = player_block_z >> CHUNK_SHIFT;

  /* Spread streaming over frames instead of letting one callback spend
     100+ ms: the run-5 quicksand was B tracking W at ~143 ms, all of it CPU
     work in this block.
     The exception is a gap the player is practically standing on: the
     stage pipeline (terrain, waystones, trees, mesh) at one guaranteed
     step each per frame takes seconds to finish a fresh row, and that
     is the "walk to the edge and wait" complaint.  When anything within
     two chunks of the player is unbuilt or unmeshed, trade a brief
     hitch for closing the hole at more than double the rate. */
  {
    int dx, dz;
    u8 urgent = FALSE;

    for (dx = -2; dx <= 2 && !urgent; dx++) {
      for (dz = -2; dz <= 2; dz++) {
        /* Only genuinely missing terrain or mesh is an emergency.  A
           pending LOD upgrade is routine -- treating it as urgent made
           every chunk crossing hitch. */
        if (worldColumnState(pcx + dx, pcz + dz) < COLUMN_DECORATED ||
            graphicsColumnMissingMesh(pcx + dx, pcz + dz)) {
          urgent = TRUE;
          break;
        }
      }
    }
    stream_work_deadline = work_start +
      OS_USEC_TO_CYCLES(urgent ? urgent_budget_usec : budget_usec);
  }

  /*
   * Prefetch bias: rank streaming work toward where the player is
   * heading, from a smoothed per-frame block displacement.  Without it,
   * nearest-first builds the ring edge directly ahead -- the exact
   * ground about to be stepped on -- dead last.
   */
  {
    static float heading_x, heading_z;
    static int prev_block_x, prev_block_z;
    static u8 heading_valid;

    if (heading_valid) {
      heading_x = heading_x * .95f +
        (float) (player_block_x - prev_block_x) * .05f;
      heading_z = heading_z * .95f +
        (float) (player_block_z - prev_block_z) * .05f;
    }
    prev_block_x = player_block_x;
    prev_block_z = player_block_z;
    heading_valid = TRUE;
    /* A steady walk settles the average near 0.12 blocks a frame and a
       sprint near 0.18; 0.04 catches a new direction within a second.  The
       lead scales with speed, because the complaint being solved is a
       sprinting player reaching the frontier: the faster they move, the
       further ahead of them the ring builds first. */
    worldSetStreamBias(
      heading_x > .14f ? 5 : (heading_x > .08f ? 3 :
        (heading_x > .04f ? 2 : (heading_x < -.14f ? -5 :
        (heading_x < -.08f ? -3 : (heading_x < -.04f ? -2 : 0))))),
      heading_z > .14f ? 5 : (heading_z > .08f ? 3 :
        (heading_z > .04f ? 2 : (heading_z < -.14f ? -5 :
        (heading_z < -.08f ? -3 : (heading_z < -.04f ? -2 : 0))))));
  }
  stepWorldStreaming(pcx, pcz, STREAM_TERRAIN_PER_STEP,
    STREAM_DECORATE_PER_STEP);
}

/* The ungated slice's budgets.  A retrace period is 16.7 ms and the same
   callback still has to run player physics, so these stay inside one field
   -- overrunning would delay the message that tells the next callback the
   frame finished.  They are deliberately larger than the gated budgets
   below: work done here overlaps the running RSP task for free, while
   every gated millisecond lands directly on the displayed frame time. */
#define STREAM_OVERLAP_USEC 12000
#define STREAM_OVERLAP_URGENT_USEC 14000
/* The gated slice's budgets.  At 30 fps the whole frame is 33 ms and the
   task takes most of it, so the gated callback only tops up what the
   overlap slices could not finish; the urgent case (a hole within two
   chunks) still gets a real bite without the 25 ms stall it used to be. */
#define STREAM_GATED_USEC 8000
#define STREAM_GATED_URGENT_USEC 15000

void callbackGfx(int pendingGfx) {
  static OSTime last_callback_time;
  OSTime callback_time = osGetTime();
  OSTime gated_start;

  /* Freeze forensics: the heartbeat tells the watchdog thread this callback
     is still arriving and completing; the tick latches red if a graphics
     task has been in flight for ~2 seconds (RSP/RDP hang). */
  diag_heartbeat++;
  diagWatchdogTick(pendingGfx);
  if (last_callback_time != 0) {
    diagNoteFrameInterval(
      (u32) OS_CYCLES_TO_USEC(callback_time - last_callback_time));
  }
  last_callback_time = callback_time;

  /*
   * One cinematic task can outlive a video retrace.  Queuing a second in that
   * case lets NuSystem rotate framebuffers while the RDP is still writing the
   * older one, which presents as torn terrain followed by corrupt UI.  Submit
   * only after the previous task has completely drained, letting the display
   * pace itself to the actual RSP/RDP cost.
   */
  if (pendingGfx == 0) {
    gated_start = osGetTime();
    /* No ceiling outside gameplay: the loading screen has no physics to
       starve and wants the world built as fast as slices allow. */
    stream_work_deadline = 0;
    if (world_job_stage != WORLD_JOB_IDLE) {
      stepWorldJob();
    } else if (worldGameBuildRequested()) {
      beginWorldJob();
    } else if (worldPreviewBuildRequested() && previewSelectionSettled()) {
      beginWorldJob();
    } else if (current_screen == GAME || current_screen == INVENTORY) {
      int player_block_x = floor(players[0].position.x / BLOCK_SIZE);
      int player_block_z = floor(players[0].position.z / BLOCK_SIZE);

      /*
       * Re-centre the render origin before it drifts out of the s15.16 Mtx
       * range (~+-512 blocks).  This rewrites every resident column's chunk
       * matrices, so like everything below it may only run with no task in
       * flight and before this callback's draw().  The camera and entity
       * matrices need no special handling: draw() rebuilds all of them every
       * frame, so the frame drawn below is consistently in the new origin.
       */
      /*
       * A corrupt player position must not become the render origin: run 5
       * died with a CPU fault inside this rebase, and converting an insane
       * float through guTranslate's s15.16 path raises exactly that
       * exception.  updatePlayer now snaps bad positions back, so a value
       * this far out can only be corruption -- skip and count it rather
       * than crash on it.
       */
      if (player_block_x > REBASE_SANITY_LIMIT ||
          player_block_x < -REBASE_SANITY_LIMIT ||
          player_block_z > REBASE_SANITY_LIMIT ||
          player_block_z < -REBASE_SANITY_LIMIT) {
        diag_position_glitches++;
      } else if (stream_rebase_enabled &&
          (player_block_x - render_origin_x > REBASE_DISTANCE ||
           render_origin_x - player_block_x > REBASE_DISTANCE ||
           player_block_z - render_origin_z > REBASE_DISTANCE ||
           render_origin_z - player_block_z > REBASE_DISTANCE)) {
        diagPaintPhase(DIAG_PHASE_REBASE);
        graphicsSetRenderOrigin(player_block_x & ~CHUNK_MASK,
          player_block_z & ~CHUNK_MASK);
        render_rebase_count++;
      }

      /*
       * Keep the world loaded around the player.  This rebinds window slots
       * and rewrites the display list pointers the terrain draw walks, so it
       * belongs here, inside the no-task-in-flight branch and ahead of the
       * draw below -- never beside it.
       *
       * The menu's orbiting preview deliberately does not stream: it shows one
       * fixed world from outside, and pulling columns around under it would
       * only fight the camera.
       */
      diagPaintPhase(DIAG_PHASE_STREAMING);
      stepGameplayStreaming(gated_start, STREAM_GATED_USEC,
        STREAM_GATED_URGENT_USEC);
    }
    /* A mesh arena can only be recycled when no submitted task can still
       reference its display lists. */
    diagPaintPhase(DIAG_PHASE_DRAW);
    draw(TRUE);
    diagNoteGatedWork((u32) OS_CYCLES_TO_USEC(osGetTime() - gated_start));
  } else if ((current_screen == GAME || current_screen == INVENTORY) &&
      world_job_stage == WORLD_JOB_IDLE) {
    /*
     * A task is in flight: the RSP and RDP are busy and this callback used
     * to be nothing but the heartbeat and player physics.  Spend the idle
     * field streaming instead -- see stepGameplayStreaming for why every
     * step in it is safe beside a running task.  At 20 fps this recovers
     * two otherwise-empty callbacks per displayed frame, which is what
     * stops walking from stacking generation time on top of render time.
     */
    OSTime overlap_start = osGetTime();

    diagPaintPhase(DIAG_PHASE_STREAMING);
    stepGameplayStreaming(overlap_start, STREAM_OVERLAP_USEC,
      STREAM_OVERLAP_URGENT_USEC);
    diagNoteGatedWork((u32) OS_CYCLES_TO_USEC(osGetTime() - overlap_start));
  }

  if (current_screen == LOADING_PREVIEW && loadingPreviewFinished()) {
    current_screen = GAME;
  }

  /* The pack is a true pause on a single-stick console.  Letting the clock
     advance while AI and the player are frozen could turn a daylight menu
     visit into an unavoidable night ambush on close. */
  if (current_screen == GAME) {
    updateDayCycle();
  } else {
    pauseDayCycle();
  }
  diagPaintPhase(DIAG_PHASE_PLAYERS);
  updatePlayers();
#ifdef ENABLE_AUDIO
  updateAudio(current_screen);
#endif
  diagPaintPhase(DIAG_PHASE_DONE);
}

void callbackPreNMI() {
    nuGfxDisplayOff();
    osViSetYScale(1);
    osAfterPreNMI();
}

void initVideo() {
  osCreateViManager(OS_PRIORITY_VIMGR);

  if (osTvType == OS_TV_NTSC) {
    osViSetMode(&osViModeNtscLan1);
  } else if (osTvType == OS_TV_PAL) {
    osViSetMode(&osViModeFpalLan1);
    osViSetYScale(0.833);
  } else if (osTvType == OS_TV_MPAL) {
    osViSetMode(&osViModeMpalLan1);
  }

  osViSetSpecialFeatures(OS_VI_GAMMA_OFF);
  nuPreNMIFuncSet((NUScPreNMIFunc) callbackPreNMI);
}

/*
 * The freeze-forensics watchdog.  Runs above every game and NuSystem
 * application thread, woken by a hardware timer, so it keeps executing when
 * the graphics thread is stuck in a loop (the N64 scheduler never preempts
 * by time slice, but timer interrupts still fire) and when that thread has
 * been stopped by a CPU exception.  Once the callback heartbeat goes stale
 * for two consecutive seconds it repaints the last recorded phase colour
 * into both framebuffers, every second, forever -- the colour on the frozen
 * screen names the subsystem that died.
 */
#define DIAG_WATCHDOG_THREAD_ID 200
#define DIAG_WATCHDOG_PRIORITY 126

static OSThread diag_watchdog_thread;
static u64 diag_watchdog_stack[0x800 / sizeof (u64)];
static OSMesgQueue diag_watchdog_queue;
static OSMesg diag_watchdog_messages[1];
static OSTimer diag_watchdog_timer;
/* A CPU exception posts OS_EVENT_FAULT.  The watchdog polls it so the frozen
   square's bottom band can say whether the dead thread crashed (white) or is
   spinning in a loop (black) -- entirely different hunts. */
static OSMesgQueue diag_fault_queue;
static OSMesg diag_fault_messages[1];

/* From libultra's fault handler (os_internal_error.h); present in the
   release library.  NULL when no thread has faulted. */
extern OSThread *__osGetCurrFaultedThread(void);

static char *diagReportHex(char *out, const char *label, u32 value) {
  int shift;

  while (*label) {
    *out++ = *label++;
  }
  *out++ = ' ';
  for (shift = 28; shift >= 0; shift -= 4) {
    *out++ = "0123456789ABCDEF"[(value >> shift) & 15];
  }
  *out++ = '\n';
  return out;
}

/*
 * The post-mortem the frozen screen cannot show: with the .out symbol map,
 * PC/RA turn a fault into a source line, and the raw position bits either
 * confirm or kill the corrupt-position theory.  Written once, after the
 * heartbeat has been stale for two seconds, from the watchdog thread -- the
 * console is already dead, so a failed write loses nothing.
 */
static void diagWriteFreezeReport(void) {
  static char report[640];
  char *out = report;
  OSThread *faulted = __osGetCurrFaultedThread();

  out = diagReportHex(out, "PHASE", diag_current_phase);
  out = diagReportHex(out, "PSTEP", diag_player_step);
  out = diagReportHex(out, "HEART", diag_heartbeat);
  if (faulted != NULL) {
    out = diagReportHex(out, "THREAD", (u32) faulted->id);
    out = diagReportHex(out, "PC", faulted->context.pc);
    out = diagReportHex(out, "CAUSE", faulted->context.cause);
    out = diagReportHex(out, "BADV", faulted->context.badvaddr);
    out = diagReportHex(out, "SR", faulted->context.sr);
    out = diagReportHex(out, "RA", (u32) faulted->context.ra);
    out = diagReportHex(out, "SP", (u32) faulted->context.sp);
  }
  {
    /* Raw float bits without the strict-aliasing trap of pointer punning. */
    union { float f; u32 raw; } pun;

    pun.f = players[0].position.x;
    out = diagReportHex(out, "POSX", pun.raw);
    pun.f = players[0].position.y;
    out = diagReportHex(out, "POSY", pun.raw);
    pun.f = players[0].position.z;
    out = diagReportHex(out, "POSZ", pun.raw);
  }
  out = diagReportHex(out, "ORGX", (u32) render_origin_x);
  out = diagReportHex(out, "ORGZ", (u32) render_origin_z);
  out = diagReportHex(out, "REBASE", render_rebase_count);
  out = diagReportHex(out, "CLAMPS", diag_loop_clamps);
  out = diagReportHex(out, "GLITCH", diag_position_glitches);
  out = diagReportHex(out, "KFAULT", window_key_faults);
  out = diagReportHex(out, "KVAL", window_key_fault_value);
  out = diagReportHex(out, "KSLOT", window_key_fault_slot);
  storageWriteFreezeReport(report, (u32) (out - report));
}

static void diagWatchdogThread(void *arg) {
  u32 last_heartbeat = 0;
  u32 stale_seconds = 0;
  u8 report_written = FALSE;

  (void) arg;
  osCreateMesgQueue(&diag_watchdog_queue, diag_watchdog_messages, 1);
  osCreateMesgQueue(&diag_fault_queue, diag_fault_messages, 1);
  osSetEventMesg(OS_EVENT_FAULT, &diag_fault_queue, NULL);
  osSetTimer(&diag_watchdog_timer, 0, OS_USEC_TO_CYCLES(1000000),
    &diag_watchdog_queue, NULL);
  while (1) {
    (void) osRecvMesg(&diag_watchdog_queue, NULL, OS_MESG_BLOCK);
    if (!diag_cpu_faulted &&
        osRecvMesg(&diag_fault_queue, NULL, OS_MESG_NOBLOCK) == 0) {
      diag_cpu_faulted = TRUE;
    }
    if (diag_heartbeat != last_heartbeat) {
      last_heartbeat = diag_heartbeat;
      stale_seconds = 0;
      continue;
    }
    if (++stale_seconds >= 2) {
      diagPaintStalePhase();
      if (!report_written) {
        report_written = TRUE;
        diagWriteFreezeReport();
      }
    }
  }
}

void mainproc(void) {
  initVideo();
  initCamera();
  initDayCycle();
  initGraphics();
  initStorage();
  nuContInit();
  osCreateThread(&diag_watchdog_thread, DIAG_WATCHDOG_THREAD_ID,
    diagWatchdogThread, NULL,
    diag_watchdog_stack + sizeof (diag_watchdog_stack) / sizeof (u64),
    DIAG_WATCHDOG_PRIORITY);
  osStartThread(&diag_watchdog_thread);
#ifdef ENABLE_AUDIO
  /*
   * Storage owns the flashcart/PI during startup. Start audio only after cart
   * probing and filesystem recovery have completely finished.
   */
  initAudio();
#endif
  nuGfxFuncSet((NUGfxFunc) callbackGfx);

  while(1)
    ;
}
