#include <nusys.h>
#include <assert.h>
#include "graphics.h"
#include "mon64.h"
#include "geometry.h"
#include "items.h"
#include "mobs.h"
#include "trees.h"
#include "camera.h"
#include "player.h"
#include "menu.h"
#include "quads.h"
#include "cube.h"
#include "textures.h"
#include "day_cycle.h"
#include "details.h"
#include "edits.h"

#define CROSSHAIR_SIZE 10
#define HOTBAR_SLOT_COUNT INVENTORY_COLUMNS
#define HOTBAR_SLOT_SIZE 22
#define HOTBAR_ICON_SIZE 16
#define HOTBAR_MARGIN 7
#define INVENTORY_SLOT_SIZE 18
#define INVENTORY_ICON_SIZE 14
#define INVENTORY_GRID_X 18
#define INVENTORY_GRID_Y 45
#define INVENTORY_HOTBAR_Y 105
#define RECIPE_LIST_X 197
#define RECIPE_LIST_Y 39
#define RECIPE_ROW_HEIGHT 20
#define RECIPE_VISIBLE_ROWS 6
/*
 * The Sun and Moon ride a fixed radius around the camera, comfortably inside
 * the tighter of the two projections (co-op's far plane is 8000).  The pass
 * no longer z-buffers, so this radius has nothing to do with depth precision
 * any more -- it only has to survive clipping, and the sizes below set the
 * apparent size against it.  The previous 11000, against a 14000 far plane,
 * encoded to within a thousandth of the cleared maximum depth: the sprites
 * were passing the z-test by a hair.
 *
 * Sizes are half-widths against that radius, so they read as angles: the Sun
 * spans about 11 degrees and the Moon about 10, which at the solo lens (60
 * degrees over 240 lines) puts them at roughly 46 and 42 pixels tall.  Both
 * are deliberately generous -- a physically-sized 0.5-degree disc is two
 * pixels on a CRT, and the sky is scenery here, not astronomy.
 */
#define CELESTIAL_DISTANCE 6000.f
#define SUN_SIZE 600.f
#define MOON_SIZE 540.f
/* Keep drawing a body slightly past the horizon so it sets behind the
   landscape rather than blinking out of an empty sky. */
#define CELESTIAL_SET_ALTITUDE (-.06f)
#define DROPPED_ITEM_RENDER_DISTANCE (BLOCK_SIZE * 36.f)
#define PLAYER_RENDER_DISTANCE (BLOCK_SIZE * 64.f)
#define MOB_RENDER_DISTANCE (BLOCK_SIZE * 30.f)
#define MAX_VISIBLE_MOBS 4

Gfx *dlp;
u32 dl_no = 0;
Gfx frame_display_lists[NUM_DISPLAY_LISTS][FRAME_DISPLAY_LIST_SIZE];

/*
 * One mesh arena, managed as first-fit blocks with incremental relocation
 * defrag -- the double-buffered pair it replaces cost a second megabyte of
 * RDRAM purely so a multi-frame compaction could rebuild into memory the
 * RSP was not reading.  The reasoning that makes one arena safe: every
 * mutation here runs from the graphics callback with pendingGfx == 0, when
 * no task is in flight, and finishes before draw() submits the next one --
 * so memory can move, provided every pointer that names it is fixed in the
 * same callback.
 *
 * A column's mesh is one contiguous block: the baked vertex data first,
 * then the command segments for each texture bank.  Keeping data and
 * commands segregated is what makes relocation possible at all -- the
 * patcher walks only the command region, so vertex bytes are never
 * misread as opcodes.
 *
 * Defrag slides one block per callback into the lowest free gap.  There is
 * no compaction pass, no second arena, and no stop-the-world: a fully
 * fragmented arena heals over a few dozen frames while the game runs.
 */
/* 1.25 MiB: the radius-10 mesh ring is ~390 shell columns plus the
   full-detail near disc, all in baked-vertex form now that the shells'
   compact matrix-pair format is gone -- its two per-quad matrix operations
   were the measured bulk of the RSP's standing frame.  The quarter
   megabyte of growth is paid for by retiring the per-block matrix table
   the old format needed (128 KiB) plus link headroom, and the A row still
   reports the margin on hardware. */
#define MESH_ARENA_SIZE 163840
static Gfx mesh_arena[MESH_ARENA_SIZE];

typedef struct {
  u32 start;      /* Gfx offset into mesh_arena */
  u32 length;     /* total Gfx, vertex region included */
  u32 verts;      /* leading Gfx of vertex data the patcher must skip */
  u16 slot;
  u8 generation;  /* which world build owns it; see the staging notes */
} MeshBlock;

/* Two generations can coexist mid world-build: every live column plus every
   staged replacement. */
#define MAX_MESH_BLOCKS (WINDOW_SLOTS * 2)
static MeshBlock mesh_blocks[MAX_MESH_BLOCKS];  /* sorted by start */
static u16 mesh_block_count;
static u32 mesh_arena_used;
/* The generation whose blocks the live pointers reference, and the one a
   world build is currently emitting.  Equal outside a build. */
static u8 mesh_generation;
static u8 mesh_building_generation;

/*
 * One column a frame was sized for the occasional block edit in a world that
 * never moved.  Streaming queues a whole row of columns every time the player
 * crosses a chunk boundary, and at one a frame that is most of a second of
 * terrain trailing behind them.
 */
#define MESH_REBUILD_BUDGET 3
/* Frames to skip rebuild attempts after an allocation failure.  Retrying
   every frame would pay the full greedy mesh before failing again. */
#define MESH_ALLOC_COOLDOWN_FRAMES 60
/* Headroom the terrain passes leave for the HUD, menu text and trailing sync. */
#define FRAME_COMMAND_TAIL_RESERVE 2048
/*
 * Per-column tables are indexed by window slot, not by a position in a fixed
 * world.  A slot is the only name a column still has once the world stops
 * having edges, and it is what stays valid when the window scrolls and rebinds
 * the slot to a different column.
 *
 * The staged set exists for world builds: the outgoing world keeps rendering
 * from the live pointers while the incoming one is emitted, and a completed
 * build publishes by copying staged over live and freeing the old
 * generation's blocks.
 */
static Gfx *column_starts[NUM_TEXTURES][WINDOW_SLOTS];
static Gfx *staged_starts[NUM_TEXTURES][WINDOW_SLOTS];
/* Hard ceiling for terrain branches within one frame list.  Release builds
   compile out assert(), so an overflow here would silently run past this
   frame's buffer into the next one and hand the RSP a corrupt list -- which
   presents as a console that renders correctly and then hangs. */
static Gfx *frame_dlp_limit;
/* Non-zero means a frame had to shed terrain or was dropped outright.  Shown
   on the picker so this failure mode can never be silent again. */
u32 frame_overflows;
/*
 * Set while compiling a whole world at once, as opposed to rebuilding single
 * columns during play.
 *
 * The only thing it changes is the fixed extent's border: a whole-world build
 * is the one moment nothing has been streamed beyond that border yet, so
 * those columns' outward faces are a cut through the map rather than a wall
 * between neighbours, and drawing them is the "chunks below the map"
 * silhouette.  During play the same columns have neighbours and keep their
 * faces.
 *
 * It deliberately does NOT change LOD any more.  It used to force every
 * column to the shell so the title orbit ran faster than the game it was
 * previewing -- which flattered the frame rate and hid exactly the geometry
 * worth judging before pressing START.  Detail now comes from meshLodFor on
 * both paths, so the preview is the gameplay mesh and entering the world
 * needs no second compile at all.
 */
static u8 building_whole_world;
/* Surface height of each block column in the footprint being compiled, so the
   scenic pass resolves it once per column instead of rescanning the full
   height for every candidate quad. */
static u8 surface_heights[CHUNK_SIZE][CHUNK_SIZE];
/*
 * Rebuild classes for dirty_columns.  A player edit's changed faces are
 * already visible as missing or stale geometry the frame it happens, so edit
 * marks outrank stream marks in the queue, and edits are the only marks that
 * may rebuild past the frame's work deadline outside the mesh stage's turn
 * in the streaming rotation (see STREAM_STAGE_* in world.h).  Marks only
 * ever upgrade: a stream mark landing on an edit-marked column must not
 * demote the rebuild the player is watching for.
 */
#define DIRTY_NONE 0
#define DIRTY_STREAM 1
#define DIRTY_EDIT 2
static u8 dirty_columns[WINDOW_SLOTS];
/* Frames left before rebuilds may be attempted after the arena refused an
   allocation; the dirty marks and the ring scan keep the need alive. */
static u8 mesh_alloc_cooldown;
/* Whether a slot has compiled geometry behind the live pointers, and the
   staged equivalent a world build publishes. */
static u8 column_meshed[WINDOW_SLOTS];
static u8 staged_meshed[WINDOW_SLOTS];

static Gfx empty_column_display_list[] = {
  gsSPEndDisplayList()
};

/* See graphics.h.  Zero until the player first wanders far from spawn; a
   fixed world never rebases at all. */
int render_origin_x;
int render_origin_z;
float render_origin_units_x;
float render_origin_units_z;

/*
 * Freeze-bisection instrumentation (see the hardware checklist in the
 * streaming plan).  The console froze after long walks with every streaming
 * counter healthy, so the diagnostics carry a rebase count and an alive
 * counter.
 */
u8 stream_rebase_enabled = TRUE;
u32 render_rebase_count = 0;

/*
 * Freeze forensics.  A small square painted straight into the displayed
 * framebuffer by the CPU -- no RSP, no RDP, no display list -- so it keeps
 * reporting after the graphics pipeline is dead.  While the game runs the
 * RDP repaints over it every frame and it reads as a flicker; the moment
 * everything stops, whatever was painted last persists on screen:
 *
 *   RED     a graphics task has been in flight for ~2s -- the RSP or RDP
 *           hung executing it (watchdog latches; nothing else paints after)
 *   GREEN   CPU died inside stepWorldStreaming (generation/decoration)
 *   YELLOW  CPU died inside the origin rebase
 *   CYAN    CPU died inside draw() -- mesh compile, culling, or submission
 *   MAGENTA CPU died inside updatePlayers -- physics or input
 *   BLUE    callbackGfx completed normally (painted last on the way out)
 *
 * A frozen screen with a BLUE square and no RED means callbacks stopped
 * arriving at all, which would point at the scheduler rather than this code.
 */
#define DIAG_SQUARE_X 12
#define DIAG_SQUARE_Y 200
#define DIAG_SQUARE_SIZE 16

static u8 diag_task_hung = FALSE;
static u32 diag_pending_streak = 0;
/* Most Gfx commands one frame has ever used, HUD and diagnostics included. */
static u32 frame_command_peak = 0;

/*
 * Run 2 taught us the square must outlive the graphics thread: the fatal
 * phase was painted into the displayed buffer, then the already-submitted
 * RSP/RDP task finished, repainted the other buffer completely and swapped
 * to it -- the evidence vanished with the swap.  Anything painted only by
 * the dying thread can be erased the same way.
 *
 * So the phase is now *recorded* (diag_current_phase) and painted lightly
 * for the alive-flicker, while a separate high-priority watchdog thread
 * (main.c) repaints the recorded color into BOTH framebuffers once the
 * heartbeat goes stale.  The N64 scheduler is strictly priority-based and
 * timer interrupts keep firing while a lower-priority thread spins, so the
 * watchdog reports through an infinite loop or a dead graphics thread.
 */
volatile u16 diag_current_phase;
volatile u32 diag_heartbeat;
/*
 * Run 4 (magenta at x 120 z 31) narrowed the death to the player phase but
 * no further, so the frozen square now carries three bands:
 *
 *   top     the phase colour (or red for an RSP/RDP hang), as before
 *   middle  which player sub-step ran last -- see the palette below
 *   bottom  white if the CPU took a fault (the OS_EVENT_FAULT fired, so the
 *           thread crashed), black if not (so it is spinning in a loop)
 *
 * Middle-band palette, meaningful when the top band is magenta:
 *   black none | green objectives | yellow input/steer | orange vault |
 *   red collision resolve | cyan targeting | magenta actions | blue post
 */
volatile u8 diag_player_step;
volatile u8 diag_cpu_faulted;
/*
 * Whether the diagnostics stack and phase-square flicker are drawn.  Off by
 * default now that streaming is stable; Z + D-pad Up toggles it, and it
 * switches itself on the moment any integrity counter ticks -- corruption
 * caught, a position snapped, a task hung, a CPU fault -- so an anomaly is
 * never silently absorbed.  The freeze watchdog and the SD post-mortem are
 * NOT gated on this: a dead console always reports.
 */
u8 diagnostics_visible = FALSE;

/*
 * Distance fog, blended toward the sky color so the streaming edge sits
 * behind haze instead of arriving in visible rows of bare columns.
 *
 * Fog is a function of *screen* depth -- gbi.h documents the start/end pair
 * as 0 = near plane, 1000 = far plane -- and perspective compresses that
 * savagely.  For the gameplay projection (near 10, far 14000) the value at a
 * distance d in blocks is
 *
 *     v(d) = 1000 * far * (64d - near) / (64d * (far - near))
 *
 * which puts the entire useful range in the last five units of a thousand:
 *
 *     993 = 20 blocks   996 = 33 blocks   998 =  58 blocks
 *     994 = 23 blocks   997 = 42 blocks   999 =  91 blocks
 *     995 = 27 blocks
 *
 * The band cannot be narrowed below four units: gSPFogPosition packs
 * 128000/(end-start) into a signed 16-bit field, so a band of three
 * overflows it and inverts the fog.  Four is therefore the tightest -- and
 * for hiding an edge the best -- band available.
 *
 * That fixes the choice.  The mesh ring is ~80 blocks, where v is 998.8, so
 * a band of 995..999 hazes from 27 blocks and is 94% opaque at the ring
 * edge, with full opacity at 91 blocks that nothing ever reaches.  Starting
 * one unit lower (994..998) is completely opaque at the edge but reaches
 * full fog at 58 blocks, which is what the previous 993..998 setting did:
 * it hid the frontier by throwing away the outer quarter of the view the
 * 32x32 window was widened to buy.  If the frontier is visible on a CRT,
 * one D-pad-left step trades that view distance back.
 *
 * Split-screen has a nearer far plane (8000), so the same values fog harder
 * there -- which suits a view already capped at ~40 blocks.
 *
 * Hardware remains the arbiter: with the diagnostics overlay up, Z + D-pad
 * Left/Right moves the start and Z + D-pad Down toggles fog outright for an
 * A/B against the bare edge.  The P row shows the current start.
 *
 * Fog also forces its pass into two-cycle mode -- roughly half the RDP's
 * pixel rate -- which is why it is confined to the *far* half of the
 * gameplay terrain: culling classifies each visible column against the
 * distance where the band begins (see COLUMN_VISIBLE_NEAR in camera.h), and
 * columns that cannot reach the band draw first in single-cycle with fog
 * off and identical output.  It is switched back off before entities and
 * HUD.  Entities are therefore unfogged at any distance; they are small and
 * near, and the alternative is a second two-cycle pass.
 */
u8 fog_enabled = TRUE;
/*
 * Fog hugs the streaming frontier instead of standing at a fixed distance.
 *
 * At rest it parks at 999 (~91 blocks, past the mesh ring): every column
 * classifies NEAR, the two-cycle far pass is skipped outright, and fog
 * costs exactly nothing -- its old fixed placement measured 5 fps on
 * hardware.  When culling reports a frustum cell with no geometry behind
 * it (the player outrunning the mesher), updateAutoFog slides the band in
 * so full opacity lands at that hole and terrain pops in behind haze
 * instead of out of the void.  It tightens fast and relaxes slowly as the
 * mesher catches up, and the near/far split confines the two-cycle cost
 * to whatever the band actually covers while it is in.
 *
 * Z + D-pad Left/Right bias where the band rests relative to the hole
 * (fog_auto_bias, in screen-depth steps); Z + D-pad Down toggles fog.
 */
u16 fog_start = 999;
s8 fog_auto_bias;
#define FOG_BAND 4

/* Times a runaway loop guard fired in player collision code.  A rising L row
   with no freeze means a guard is eating what used to be the hang. */
u32 diag_loop_clamps;
u32 diag_ray_clamps;
u32 diag_resolve_clamps;
u32 diag_ray_guard_speed;
u32 diag_ray_guard_time;
/* Times a player position failed the sanity check (non-finite or absurdly far
   out) and was snapped back, or a rebase was refused for the same reason.
   Any non-zero G proves the position-corruption theory of the run-5 fault. */
u32 diag_position_glitches;

static const u16 diag_step_colors[8] = {
  GPACK_RGBA5551(0, 0, 0, 1),       /* 0 none */
  GPACK_RGBA5551(0, 255, 0, 1),     /* 1 objectives */
  GPACK_RGBA5551(255, 255, 0, 1),   /* 2 input/steer */
  GPACK_RGBA5551(255, 140, 0, 1),   /* 3 vault */
  GPACK_RGBA5551(255, 0, 0, 1),     /* 4 collision resolve */
  GPACK_RGBA5551(0, 255, 255, 1),   /* 5 targeting */
  GPACK_RGBA5551(255, 0, 255, 1),   /* 6 actions */
  GPACK_RGBA5551(0, 0, 255, 1)      /* 7 post */
};

static void diagPaintRows(u16 *frame_buffer, int y0, int rows, u16 color) {
  int x, y;

  if (frame_buffer == NULL) {
    return;
  }
  for (y = y0; y < y0 + rows; y++) {
    u16 *row = frame_buffer + (DIAG_SQUARE_Y + y) * SCREEN_WD + DIAG_SQUARE_X;
    for (x = 0; x < DIAG_SQUARE_SIZE; x++) {
      row[x] = color;
    }
    /* The VI scans RDRAM; flush each painted row out of the data cache. */
    osWritebackDCache(row, DIAG_SQUARE_SIZE * sizeof (u16));
  }
}

static void diagPaintBuffer(u16 *frame_buffer, u16 color) {
  diagPaintRows(frame_buffer, 0, DIAG_SQUARE_SIZE, color);
}

void diagPaintPhase(u16 color) {
  /* Always record -- the watchdog reports the phase whether or not the
     overlay is showing.  Only the alive-flicker paint is cosmetic. */
  diag_current_phase = color;
  if (diag_task_hung || !diagnostics_visible) {
    return;
  }
  diagPaintBuffer((u16 *) osViGetCurrentFramebuffer(), color);
}

/* Called by the watchdog thread after the heartbeat stops: keep the fatal
   evidence on screen no matter which buffer the VI ends up scanning. */
void diagPaintStalePhase(void) {
  u16 phase = diag_task_hung ? GPACK_RGBA5551(255, 0, 0, 1)
    : diag_current_phase;
  u16 step = diag_step_colors[diag_player_step & 7];
  u16 fault = diag_cpu_faulted ? GPACK_RGBA5551(255, 255, 255, 1)
    : GPACK_RGBA5551(0, 0, 0, 1);
  u16 *buffers[2];
  int i;

  buffers[0] = (u16 *) osViGetCurrentFramebuffer();
  buffers[1] = (u16 *) osViGetNextFramebuffer();
  for (i = 0; i < 2; i++) {
    diagPaintRows(buffers[i], 0, 6, phase);
    diagPaintRows(buffers[i], 6, 5, step);
    diagPaintRows(buffers[i], 11, 5, fault);
  }
}

void diagWatchdogTick(int pendingGfx) {
  if (pendingGfx == 0) {
    diag_pending_streak = 0;
    return;
  }
  if (++diag_pending_streak > 120 && !diag_task_hung) {
    diag_task_hung = TRUE;
    diagPaintStalePhase();
  }
}

/*
 * Worst-of-window frame pacing, for the quicksand symptom.  W is the wall
 * clock between consecutive graphics callbacks -- the player's actual frame
 * time, whatever is causing it.  B is the CPU cost of the gated block
 * (streaming, rebasing, mesh work, frame build).  B near W says the CPU work
 * in the callback is the bottleneck; W high with B low says the RSP/RDP is.
 * The window resets every ~2s so a single historical spike does not pin the
 * numbers forever.
 */
static u32 diag_worst_frame_usec;
static u32 diag_worst_gated_usec;
static u16 diag_perf_window;

void diagNoteFrameInterval(u32 usec) {
  if (usec > diag_worst_frame_usec) {
    diag_worst_frame_usec = usec;
  }
  if (++diag_perf_window >= 120) {
    diag_perf_window = 0;
    diag_worst_frame_usec = usec;
    diag_worst_gated_usec = 0;
  }
}

void diagNoteGatedWork(u32 usec) {
  if (usec > diag_worst_gated_usec) {
    diag_worst_gated_usec = usec;
  }
}

/*
 * Frames the player actually saw in the last whole second.
 *
 * This counts submitted graphics tasks, not graphics callbacks, and the
 * difference is the whole point.  callbackGfx runs once per retrace message
 * but only builds a frame when no task is in flight, so when the RSP/RDP is
 * the bottleneck the callback keeps arriving at the field rate and returns
 * without drawing -- three 60 Hz callbacks for one picture.  Counting
 * callbacks would report 60 while the screen updates at 20.  Every
 * submission below carries NU_SC_SWAPBUFFER and so becomes exactly one buffer
 * swap, which makes this count the displayed rate by construction.
 *
 * That also makes it independent evidence from W: W only stretches once the
 * CPU work in the callback is what is late, because a callback that finds a
 * task still pending returns immediately and keeps its interval short.
 *
 * Counted unconditionally, so the number is already a full second old and
 * correct the moment the overlay is switched on.
 */
static u32 diag_fps;
static u32 diag_fps_frames;
static OSTime diag_fps_window_start;

void diagNoteFrameSubmitted(void) {
  OSTime now = osGetTime();
  OSTime second = OS_USEC_TO_CYCLES(1000000);

  diag_fps_frames++;
  if (diag_fps_window_start == 0) {
    diag_fps_window_start = now;
    return;
  }
  if (now - diag_fps_window_start < second) {
    return;
  }
  diag_fps = diag_fps_frames;
  diag_fps_frames = 0;
  /* Advance by a whole second rather than restarting from now, so the window
     does not drift by however late this frame noticed the boundary. */
  diag_fps_window_start += second;
  /* World generation can stall the pipeline for many seconds at a stretch.
     Resynchronise instead of walking the backlog one second per frame, which
     would report a stale count for as long as the stall lasted. */
  if (now - diag_fps_window_start >= second) {
    diag_fps_window_start = now;
  }
}

#define BLOCKS_PER_CHUNK (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)

/*
 * One translation per chunk, indexed by the window slot its column occupies.
 * These used to be prebaked once in initGraphics for a world whose chunks each
 * had a permanent address; a slot outlives the column in it, so a slot's
 * matrices are rewritten whenever the column bound to it is compiled.
 */
/*
 * One translation per column, indexed by window slot.  Every column bakes
 * its quads into column-local vertices now, so this is the only terrain
 * matrix left: the per-block table the shell format used to multiply in
 * (2048 matrices, 128 KiB) went to the mesh arena instead, where the same
 * bytes hold the baked vertices that make the table unnecessary.
 */
static Mtx c_models[WINDOW_SLOTS];

/* The targeting wireframe was the block table's other user; one
   origin-relative translation per player per frame replaces it, frame
   buffered like every other matrix the RSP may still be reading. */
static Mtx wireframe_target_model[NUM_DISPLAY_LISTS][MAX_PLAYERS];

/* Water keeps the normal block footprint but leaves a small lip below an
 * adjacent shore.  Only top faces need a special mesh, so this costs 4 KiB
 * rather than duplicating every terrain quad. */
#define WATER_SURFACE_HEIGHT (BLOCK_SIZE - BLOCK_SIZE / 8)
#define WATER_TOP_QUAD_ADDR(width, height) \
  (water_top_verts + (((width) * CHUNK_SIZE + (height)) * 4))
static Vtx water_top_verts[CHUNK_SIZE * CHUNK_SIZE * 4]
  __attribute__((aligned(8)));

/* Lighting and sky geometry are both frame-buffered just like the camera:
   the RSP can still be drawing the previous frame when the CPU prepares the
   next one. */
static Lights1 world_lights[NUM_DISPLAY_LISTS][MAX_PLAYERS];
static Lights0 preview_lights[NUM_DISPLAY_LISTS];
static Vtx celestial_verts[NUM_DISPLAY_LISTS][MAX_PLAYERS][8];
/*
 * The sky's own modelview.  It carries the camera position, so the quads
 * themselves stay in a local space bounded by the orbit radius -- which is
 * what keeps them inside the s16 vertex format no matter how far the player
 * has walked.  Building them at absolute world coordinates instead is what
 * broke the sky when the world stopped having edges: the camera matrix is
 * origin-relative, so the sprites were displaced by the whole render origin
 * and then wrapped their s16 coordinates a few hundred blocks later.
 */
static Mtx celestial_model[NUM_DISPLAY_LISTS][MAX_PLAYERS];

static Mtx dropped_item_translate[NUM_DISPLAY_LISTS][MAX_DROPPED_ITEMS];
static Mtx dropped_item_rotate[NUM_DISPLAY_LISTS][MAX_DROPPED_ITEMS];

#define FALLING_TREE_RENDER_SLOTS 4
#define FALLING_TREE_BOXES 6

/* A falling tree uses a few chunky volumes, not dozens of independently
   transformed cubes.  Individual cube drops appear after it lands. */
static Mtx falling_tree_translate[NUM_DISPLAY_LISTS][FALLING_TREE_RENDER_SLOTS];
static Mtx falling_tree_rotate[NUM_DISPLAY_LISTS][FALLING_TREE_RENDER_SLOTS];
static Mtx falling_tree_box_translate[NUM_DISPLAY_LISTS][FALLING_TREE_RENDER_SLOTS][FALLING_TREE_BOXES];
static Mtx falling_tree_box_scale[NUM_DISPLAY_LISTS][FALLING_TREE_RENDER_SLOTS][FALLING_TREE_BOXES];

#define ITEM_VERTEX(x, y, z, s, t) {x, y, z, 0, s, t, 255, 255, 255, 255}

static Vtx dropped_item_verts[] = {
  ITEM_VERTEX(-14, 14, 14, 0, 0), ITEM_VERTEX(14, 14, 14, 16 << 5, 0),
  ITEM_VERTEX(14, -14, 14, 16 << 5, 16 << 5), ITEM_VERTEX(-14, -14, 14, 0, 16 << 5),
  ITEM_VERTEX(14, 14, -14, 0, 0), ITEM_VERTEX(-14, 14, -14, 16 << 5, 0),
  ITEM_VERTEX(-14, -14, -14, 16 << 5, 16 << 5), ITEM_VERTEX(14, -14, -14, 0, 16 << 5)
};

static Gfx dropped_item_display_list[] = {
  gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0),
  gsSP2Triangles(4, 5, 6, 0, 4, 6, 7, 0),
  gsSP2Triangles(1, 4, 7, 0, 1, 7, 2, 0),
  gsSP2Triangles(5, 0, 3, 0, 5, 3, 6, 0),
  gsSP2Triangles(5, 4, 1, 0, 5, 1, 0, 0),
  gsSP2Triangles(3, 2, 7, 0, 3, 7, 6, 0),
  gsSPEndDisplayList()
};

/* Steve is deliberately built from a handful of shaded boxes instead of a
   texture atlas.  It is cheap enough to submit once in each co-op viewport,
   yet still reads as a character at the game's normal view distance. */
#define STEVE_HEAD 0
#define STEVE_BODY 1
#define STEVE_LEFT_ARM 2
#define STEVE_RIGHT_ARM 3
#define STEVE_LEFT_LEG 4
#define STEVE_RIGHT_LEG 5
#define STEVE_SWORD 6
#define STEVE_PART_COUNT 7

static Mtx steve_translate[NUM_DISPLAY_LISTS][MAX_PLAYERS][STEVE_PART_COUNT];
static Mtx steve_rotate[NUM_DISPLAY_LISTS][MAX_PLAYERS][STEVE_PART_COUNT];
static Mtx first_person_sword_translate[NUM_DISPLAY_LISTS][MAX_PLAYERS];
static Mtx first_person_sword_rotate[NUM_DISPLAY_LISTS][MAX_PLAYERS];

#define MOB_BODY 0
#define MOB_HEAD 1
#define MOB_FRONT_LEFT_LEG 2
#define MOB_FRONT_RIGHT_LEG 3
#define MOB_BACK_LEFT_LEG 4
#define MOB_BACK_RIGHT_LEG 5
#define MOB_SNOUT 6
#define MOB_PART_COUNT 7

static Mtx mob_translate[NUM_DISPLAY_LISTS][MAX_MOBS][MOB_PART_COUNT];
static Mtx mob_rotate[NUM_DISPLAY_LISTS][MAX_MOBS][MOB_PART_COUNT];

/* Detail records may be numerous in a base, but only a small nearest set is
   submitted per viewport.  The matrices are double-buffered like every other
   RSP reference and indexed by render slot rather than persistent record. */
#define MAX_VISIBLE_DETAILS 24
#define DETAIL_RENDER_DISTANCE (BLOCK_SIZE * 28.f)
static Mtx detail_translate[NUM_DISPLAY_LISTS][MAX_VISIBLE_DETAILS];
static Mtx detail_rotate[NUM_DISPLAY_LISTS][MAX_VISIBLE_DETAILS];

#define STEVE_VERTEX(x, y, z, r, g, b) {x, y, z, 0, 0, 0, r, g, b, 255}

#define DETAIL_BOX(name, x0, y0, z0, x1, y1, z1, r1, g1, b1, r2, g2, b2) \
static Vtx name[] = { \
  STEVE_VERTEX(x0, y1, z1, r1, g1, b1), STEVE_VERTEX(x1, y1, z1, r1, g1, b1), \
  STEVE_VERTEX(x1, y0, z1, r1, g1, b1), STEVE_VERTEX(x0, y0, z1, r1, g1, b1), \
  STEVE_VERTEX(x1, y1, z0, r2, g2, b2), STEVE_VERTEX(x0, y1, z0, r2, g2, b2), \
  STEVE_VERTEX(x0, y0, z0, r2, g2, b2), STEVE_VERTEX(x1, y0, z0, r2, g2, b2) \
}

DETAIL_BOX(wood_stair_lower_verts, -32, 0, -32, 32, 31, 32,
  174, 117, 61, 112, 70, 34);
DETAIL_BOX(wood_stair_upper_verts, -32, 31, 0, 32, 63, 32,
  187, 130, 69, 122, 78, 39);
DETAIL_BOX(stone_stair_lower_verts, -32, 0, -32, 32, 31, 32,
  155, 158, 153, 92, 97, 94);
DETAIL_BOX(stone_stair_upper_verts, -32, 31, 0, 32, 63, 32,
  177, 181, 176, 108, 113, 110);
DETAIL_BOX(door_verts, -28, 0, -4, 28, 126, 4,
  151, 96, 45, 91, 54, 27);
DETAIL_BOX(window_left_verts, -31, 0, -4, -22, 64, 4,
  195, 203, 190, 104, 119, 113);
DETAIL_BOX(window_right_verts, 22, 0, -4, 31, 64, 4,
  195, 203, 190, 104, 119, 113);
DETAIL_BOX(window_top_verts, -22, 54, -4, 22, 64, 4,
  180, 201, 196, 92, 120, 121);
DETAIL_BOX(window_bottom_verts, -22, 0, -4, 22, 10, 4,
  180, 201, 196, 92, 120, 121);
DETAIL_BOX(window_cross_verts, -3, 10, -3, 3, 54, 3,
  124, 179, 181, 64, 110, 121);
DETAIL_BOX(torch_stick_verts, -4, 0, -4, 4, 39, 4,
  146, 86, 36, 88, 48, 20);
/* A taper costs no more than the old flame box and makes both placed and
   dropped torches read as fire rather than a glowing cube. */
static Vtx torch_flame_verts[] = {
  STEVE_VERTEX(-4, 55, 4, 255, 229, 104), STEVE_VERTEX(4, 55, 4, 255, 229, 104),
  STEVE_VERTEX(8, 38, 8, 255, 192, 55), STEVE_VERTEX(-8, 38, 8, 255, 192, 55),
  STEVE_VERTEX(4, 55, -4, 238, 110, 28), STEVE_VERTEX(-4, 55, -4, 238, 110, 28),
  STEVE_VERTEX(-8, 38, -8, 218, 64, 19), STEVE_VERTEX(8, 38, -8, 218, 64, 19)
};

/* Head, torso, arms, and legs.  Limbs start at y = 0 so their rotation has a
   convincing shoulder/hip pivot instead of spinning around their middles. */
static Vtx steve_head_verts[] = {
  STEVE_VERTEX(-16, 16, 16, 198, 137, 90), STEVE_VERTEX(16, 16, 16, 198, 137, 90),
  STEVE_VERTEX(16, -16, 16, 198, 137, 90), STEVE_VERTEX(-16, -16, 16, 198, 137, 90),
  STEVE_VERTEX(16, 16, -16, 198, 137, 90), STEVE_VERTEX(-16, 16, -16, 198, 137, 90),
  STEVE_VERTEX(-16, -16, -16, 198, 137, 90), STEVE_VERTEX(16, -16, -16, 198, 137, 90)
};

static Vtx steve_body_verts[] = {
  STEVE_VERTEX(-18, 22, 9, 54, 140, 190), STEVE_VERTEX(18, 22, 9, 54, 140, 190),
  STEVE_VERTEX(18, -22, 9, 54, 140, 190), STEVE_VERTEX(-18, -22, 9, 54, 140, 190),
  STEVE_VERTEX(18, 22, -9, 54, 140, 190), STEVE_VERTEX(-18, 22, -9, 54, 140, 190),
  STEVE_VERTEX(-18, -22, -9, 54, 140, 190), STEVE_VERTEX(18, -22, -9, 54, 140, 190)
};

static Vtx steve_arm_verts[] = {
  STEVE_VERTEX(-7, 0, 7, 198, 137, 90), STEVE_VERTEX(7, 0, 7, 198, 137, 90),
  STEVE_VERTEX(7, -44, 7, 198, 137, 90), STEVE_VERTEX(-7, -44, 7, 198, 137, 90),
  STEVE_VERTEX(7, 0, -7, 198, 137, 90), STEVE_VERTEX(-7, 0, -7, 198, 137, 90),
  STEVE_VERTEX(-7, -44, -7, 198, 137, 90), STEVE_VERTEX(7, -44, -7, 198, 137, 90)
};

static Vtx steve_leg_verts[] = {
  STEVE_VERTEX(-8, 0, 8, 55, 70, 150), STEVE_VERTEX(8, 0, 8, 55, 70, 150),
  STEVE_VERTEX(8, -44, 8, 55, 70, 150), STEVE_VERTEX(-8, -44, 8, 55, 70, 150),
  STEVE_VERTEX(8, 0, -8, 55, 70, 150), STEVE_VERTEX(-8, 0, -8, 55, 70, 150),
  STEVE_VERTEX(-8, -44, -8, 55, 70, 150), STEVE_VERTEX(8, -44, -8, 55, 70, 150)
};

/* The sheep is all chunky shaded geometry, matching Steve's inexpensive
   renderer.  Its fleece therefore needs no new character atlas or UV work. */
static Vtx sheep_body_verts[] = {
  STEVE_VERTEX(-27, 17, 18, 236, 234, 218), STEVE_VERTEX(27, 17, 18, 236, 234, 218),
  STEVE_VERTEX(27, -17, 18, 236, 234, 218), STEVE_VERTEX(-27, -17, 18, 236, 234, 218),
  STEVE_VERTEX(27, 17, -18, 204, 204, 194), STEVE_VERTEX(-27, 17, -18, 204, 204, 194),
  STEVE_VERTEX(-27, -17, -18, 204, 204, 194), STEVE_VERTEX(27, -17, -18, 204, 204, 194)
};

static Vtx sheep_head_verts[] = {
  STEVE_VERTEX(-15, 14, 14, 190, 188, 176), STEVE_VERTEX(15, 14, 14, 190, 188, 176),
  STEVE_VERTEX(15, -14, 14, 190, 188, 176), STEVE_VERTEX(-15, -14, 14, 190, 188, 176),
  STEVE_VERTEX(15, 14, -14, 150, 148, 140), STEVE_VERTEX(-15, 14, -14, 150, 148, 140),
  STEVE_VERTEX(-15, -14, -14, 150, 148, 140), STEVE_VERTEX(15, -14, -14, 150, 148, 140)
};

static Vtx sheep_leg_verts[] = {
  STEVE_VERTEX(-6, 18, 6, 104, 96, 82), STEVE_VERTEX(6, 18, 6, 104, 96, 82),
  STEVE_VERTEX(6, -18, 6, 104, 96, 82), STEVE_VERTEX(-6, -18, 6, 104, 96, 82),
  STEVE_VERTEX(6, 18, -6, 76, 70, 62), STEVE_VERTEX(-6, 18, -6, 76, 70, 62),
  STEVE_VERTEX(-6, -18, -6, 76, 70, 62), STEVE_VERTEX(6, -18, -6, 76, 70, 62)
};

static Vtx pig_body_verts[] = {
  STEVE_VERTEX(-27, 16, 18, 224, 145, 151), STEVE_VERTEX(27, 16, 18, 224, 145, 151),
  STEVE_VERTEX(27, -16, 18, 224, 145, 151), STEVE_VERTEX(-27, -16, 18, 224, 145, 151),
  STEVE_VERTEX(27, 16, -18, 177, 104, 113), STEVE_VERTEX(-27, 16, -18, 177, 104, 113),
  STEVE_VERTEX(-27, -16, -18, 177, 104, 113), STEVE_VERTEX(27, -16, -18, 177, 104, 113)
};

static Vtx pig_head_verts[] = {
  STEVE_VERTEX(-16, 15, 15, 232, 155, 160), STEVE_VERTEX(16, 15, 15, 232, 155, 160),
  STEVE_VERTEX(16, -15, 15, 232, 155, 160), STEVE_VERTEX(-16, -15, 15, 232, 155, 160),
  STEVE_VERTEX(16, 15, -15, 184, 111, 120), STEVE_VERTEX(-16, 15, -15, 184, 111, 120),
  STEVE_VERTEX(-16, -15, -15, 184, 111, 120), STEVE_VERTEX(16, -15, -15, 184, 111, 120)
};

static Vtx pig_leg_verts[] = {
  STEVE_VERTEX(-6, 17, 6, 197, 123, 132), STEVE_VERTEX(6, 17, 6, 197, 123, 132),
  STEVE_VERTEX(6, -17, 6, 197, 123, 132), STEVE_VERTEX(-6, -17, 6, 197, 123, 132),
  STEVE_VERTEX(6, 17, -6, 144, 80, 91), STEVE_VERTEX(-6, 17, -6, 144, 80, 91),
  STEVE_VERTEX(-6, -17, -6, 144, 80, 91), STEVE_VERTEX(6, -17, -6, 144, 80, 91)
};

static Vtx pig_snout_verts[] = {
  STEVE_VERTEX(-10, 7, 5, 238, 171, 170), STEVE_VERTEX(10, 7, 5, 238, 171, 170),
  STEVE_VERTEX(10, -7, 5, 238, 171, 170), STEVE_VERTEX(-10, -7, 5, 238, 171, 170),
  STEVE_VERTEX(10, 7, -5, 162, 91, 101), STEVE_VERTEX(-10, 7, -5, 162, 91, 101),
  STEVE_VERTEX(-10, -7, -5, 162, 91, 101), STEVE_VERTEX(10, -7, -5, 162, 91, 101)
};

static Vtx slime_body_verts[] = {
  STEVE_VERTEX(-22, 21, 20, 91, 194, 83), STEVE_VERTEX(22, 21, 20, 91, 194, 83),
  STEVE_VERTEX(22, -21, 20, 91, 194, 83), STEVE_VERTEX(-22, -21, 20, 91, 194, 83),
  STEVE_VERTEX(22, 21, -20, 47, 125, 58), STEVE_VERTEX(-22, 21, -20, 47, 125, 58),
  STEVE_VERTEX(-22, -21, -20, 47, 125, 58), STEVE_VERTEX(22, -21, -20, 47, 125, 58)
};

static Vtx slime_eye_verts[] = {
  STEVE_VERTEX(-4, 5, 3, 16, 25, 20), STEVE_VERTEX(4, 5, 3, 16, 25, 20),
  STEVE_VERTEX(4, -5, 3, 16, 25, 20), STEVE_VERTEX(-4, -5, 3, 16, 25, 20),
  STEVE_VERTEX(4, 5, -3, 7, 12, 9), STEVE_VERTEX(-4, 5, -3, 7, 12, 9),
  STEVE_VERTEX(-4, -5, -3, 7, 12, 9), STEVE_VERTEX(4, -5, -3, 7, 12, 9)
};

static Vtx slime_gel_verts[] = {
  STEVE_VERTEX(-7, 8, 8, 128, 224, 112), STEVE_VERTEX(8, 7, 8, 128, 224, 112),
  STEVE_VERTEX(10, -6, 9, 83, 181, 78), STEVE_VERTEX(-10, -7, 9, 83, 181, 78),
  STEVE_VERTEX(8, 7, -8, 46, 127, 59), STEVE_VERTEX(-7, 8, -9, 46, 127, 59),
  STEVE_VERTEX(-10, -7, -9, 33, 93, 48), STEVE_VERTEX(10, -6, -8, 33, 93, 48)
};

/* Two blue eye quads on the local -Z face make it obvious where Steve is
   looking, even without a character texture. */
static Vtx steve_eye_verts[] = {
  STEVE_VERTEX(-11, 8, -17, 55, 125, 210), STEVE_VERTEX(-5, 8, -17, 55, 125, 210),
  STEVE_VERTEX(-5, 2, -17, 55, 125, 210), STEVE_VERTEX(-11, 2, -17, 55, 125, 210),
  STEVE_VERTEX(5, 8, -17, 55, 125, 210), STEVE_VERTEX(11, 8, -17, 55, 125, 210),
  STEVE_VERTEX(11, 2, -17, 55, 125, 210), STEVE_VERTEX(5, 2, -17, 55, 125, 210)
};

/*
 * Swords are the model held closest to the camera, so a rectangular blade
 * read more like a ruler than a weapon.  This five-sided prism gives it a
 * point and a broader base.  It remains one vertex load and one display-list
 * call; only four very small triangles are added over the old box.
 */
#define SWORD_BLADE_VERTS(name, fr, fg, fb, br, bg, bb) \
static Vtx name[] = { \
  STEVE_VERTEX(-6, 4, 3, fr, fg, fb), STEVE_VERTEX(6, 4, 3, fr, fg, fb), \
  STEVE_VERTEX(5, -32, 3, fr, fg, fb), STEVE_VERTEX(0, -46, 3, fr, fg, fb), \
  STEVE_VERTEX(-5, -32, 3, fr, fg, fb), \
  STEVE_VERTEX(-6, 4, -3, br, bg, bb), STEVE_VERTEX(6, 4, -3, br, bg, bb), \
  STEVE_VERTEX(5, -32, -3, br, bg, bb), STEVE_VERTEX(0, -46, -3, br, bg, bb), \
  STEVE_VERTEX(-5, -32, -3, br, bg, bb) \
}

SWORD_BLADE_VERTS(iron_sword_blade_verts, 230, 232, 221, 145, 154, 153);
SWORD_BLADE_VERTS(stone_sword_blade_verts, 171, 178, 175, 92, 100, 100);
SWORD_BLADE_VERTS(wood_sword_blade_verts, 178, 121, 60, 96, 58, 27);

/* One cheap crossguard per sword tier preserves the old draw-call count while
   making the upgrades visibly feel like different weapons. */
#define SWORD_GUARD_VERTS(name, fr, fg, fb, br, bg, bb) \
static Vtx name[] = { \
  STEVE_VERTEX(-12, 7, 5, fr, fg, fb), STEVE_VERTEX(12, 7, 5, fr, fg, fb), \
  STEVE_VERTEX(12, 1, 5, fr, fg, fb), STEVE_VERTEX(-12, 1, 5, fr, fg, fb), \
  STEVE_VERTEX(12, 7, -5, br, bg, bb), STEVE_VERTEX(-12, 7, -5, br, bg, bb), \
  STEVE_VERTEX(-12, 1, -5, br, bg, bb), STEVE_VERTEX(12, 1, -5, br, bg, bb) \
}

SWORD_GUARD_VERTS(wood_sword_guard_verts, 157, 92, 39, 94, 51, 24);
SWORD_GUARD_VERTS(stone_sword_guard_verts, 127, 112, 78, 70, 62, 48);
SWORD_GUARD_VERTS(iron_sword_guard_verts, 157, 166, 171, 78, 90, 99);

/* Pickaxes and axes share one low-poly handle, but keep distinct heads so
   every tool reads immediately both in first person and while spinning as a
   pickup.  Wood and stone tiers use different shaded head geometry. */
static Vtx tool_handle_verts[] = {
  STEVE_VERTEX(-4, 8, 4, 157, 98, 44), STEVE_VERTEX(4, 8, 4, 157, 98, 44),
  STEVE_VERTEX(3, -38, 3, 128, 73, 31), STEVE_VERTEX(-3, -38, 3, 128, 73, 31),
  STEVE_VERTEX(4, 8, -4, 102, 58, 25), STEVE_VERTEX(-4, 8, -4, 102, 58, 25),
  STEVE_VERTEX(-3, -38, -3, 77, 43, 20), STEVE_VERTEX(3, -38, -3, 77, 43, 20)
};

#define TOOL_HEAD_VERTS(name, left, right, top, bottom, r1, g1, b1, r2, g2, b2) \
static Vtx name[] = { \
  STEVE_VERTEX(left, top, 4, r1, g1, b1), STEVE_VERTEX(right, top, 4, r1, g1, b1), \
  STEVE_VERTEX(right, bottom, 4, r1, g1, b1), STEVE_VERTEX(left, bottom, 4, r1, g1, b1), \
  STEVE_VERTEX(right, top, -4, r2, g2, b2), STEVE_VERTEX(left, top, -4, r2, g2, b2), \
  STEVE_VERTEX(left, bottom, -4, r2, g2, b2), STEVE_VERTEX(right, bottom, -4, r2, g2, b2) \
}

TOOL_HEAD_VERTS(wood_pick_head_verts, -23, 23, 13, 1,
  177, 119, 61, 100, 61, 29);
TOOL_HEAD_VERTS(stone_pick_head_verts, -23, 23, 13, 1,
  183, 187, 181, 100, 106, 105);
TOOL_HEAD_VERTS(iron_pick_head_verts, -23, 23, 13, 1,
  231, 234, 224, 137, 148, 151);
/* The tapered axe bit keeps the same eight vertices and box display list,
   but its silhouette no longer reads as a second pickaxe head. */
#define AXE_HEAD_VERTS(name, fr, fg, fb, br, bg, bb) \
static Vtx name[] = { \
  STEVE_VERTEX(-19, 15, 4, fr, fg, fb), STEVE_VERTEX(11, 15, 4, fr, fg, fb), \
  STEVE_VERTEX(5, -5, 4, fr, fg, fb), STEVE_VERTEX(-14, -5, 4, fr, fg, fb), \
  STEVE_VERTEX(11, 15, -4, br, bg, bb), STEVE_VERTEX(-19, 15, -4, br, bg, bb), \
  STEVE_VERTEX(-14, -5, -4, br, bg, bb), STEVE_VERTEX(5, -5, -4, br, bg, bb) \
}

AXE_HEAD_VERTS(wood_axe_head_verts, 177, 119, 61, 100, 61, 29);
AXE_HEAD_VERTS(stone_axe_head_verts, 183, 187, 181, 100, 106, 105);
AXE_HEAD_VERTS(iron_axe_head_verts, 231, 234, 224, 137, 148, 151);

static Vtx coal_chunk_verts[] = {
  STEVE_VERTEX(-7, 11, 7, 61, 65, 66), STEVE_VERTEX(8, 9, 8, 61, 65, 66),
  STEVE_VERTEX(11, -8, 9, 44, 47, 49), STEVE_VERTEX(-10, -10, 8, 44, 47, 49),
  STEVE_VERTEX(8, 9, -7, 24, 27, 29), STEVE_VERTEX(-7, 11, -8, 24, 27, 29),
  STEVE_VERTEX(-10, -10, -8, 13, 15, 16), STEVE_VERTEX(11, -8, -7, 13, 15, 16)
};

static Vtx iron_chunk_verts[] = {
  STEVE_VERTEX(-8, 11, 7, 222, 167, 113), STEVE_VERTEX(9, 8, 8, 222, 167, 113),
  STEVE_VERTEX(12, -7, 8, 180, 122, 79), STEVE_VERTEX(-11, -10, 8, 180, 122, 79),
  STEVE_VERTEX(9, 8, -7, 137, 91, 65), STEVE_VERTEX(-8, 11, -8, 137, 91, 65),
  STEVE_VERTEX(-11, -10, -8, 94, 61, 48), STEVE_VERTEX(12, -7, -7, 94, 61, 48)
};

static Vtx apple_body_verts[] = {
  STEVE_VERTEX(-8, 11, 8, 228, 56, 43), STEVE_VERTEX(9, 10, 9, 228, 56, 43),
  STEVE_VERTEX(12, -7, 9, 204, 38, 32), STEVE_VERTEX(-11, -9, 9, 204, 38, 32),
  STEVE_VERTEX(9, 10, -8, 137, 23, 23), STEVE_VERTEX(-8, 11, -9, 137, 23, 23),
  STEVE_VERTEX(-11, -9, -9, 106, 18, 20), STEVE_VERTEX(12, -7, -8, 106, 18, 20)
};

static Vtx apple_stem_verts[] = {
  STEVE_VERTEX(-2, 17, 2, 83, 57, 28), STEVE_VERTEX(2, 17, 2, 83, 57, 28),
  STEVE_VERTEX(2, 9, 2, 83, 57, 28), STEVE_VERTEX(-2, 9, 2, 83, 57, 28),
  STEVE_VERTEX(2, 17, -2, 47, 86, 35), STEVE_VERTEX(-2, 17, -2, 47, 86, 35),
  STEVE_VERTEX(-2, 9, -2, 47, 86, 35), STEVE_VERTEX(2, 9, -2, 47, 86, 35)
};

static Vtx mutton_verts[] = {
  STEVE_VERTEX(-13, 8, 7, 172, 75, 67), STEVE_VERTEX(12, 8, 7, 172, 75, 67),
  STEVE_VERTEX(8, -9, 7, 172, 75, 67), STEVE_VERTEX(-10, -9, 7, 172, 75, 67),
  STEVE_VERTEX(12, 8, -7, 112, 48, 44), STEVE_VERTEX(-13, 8, -7, 112, 48, 44),
  STEVE_VERTEX(-10, -9, -7, 112, 48, 44), STEVE_VERTEX(8, -9, -7, 112, 48, 44)
};

static Vtx pork_verts[] = {
  STEVE_VERTEX(-12, 8, 8, 213, 125, 117), STEVE_VERTEX(11, 8, 8, 213, 125, 117),
  STEVE_VERTEX(9, -9, 7, 195, 100, 98), STEVE_VERTEX(-9, -9, 7, 195, 100, 98),
  STEVE_VERTEX(11, 8, -8, 143, 72, 76), STEVE_VERTEX(-12, 8, -8, 143, 72, 76),
  STEVE_VERTEX(-9, -9, -7, 112, 52, 59), STEVE_VERTEX(9, -9, -7, 112, 52, 59)
};

/* First person needs the forearm as well as the blade; otherwise a floating
   sword loses the "held" feeling that makes an attack easy to read.  Its hand
   ends at the same origin as the sword hilt. */
static Vtx first_person_arm_verts[] = {
  STEVE_VERTEX(-7, 48, 7, 198, 137, 90), STEVE_VERTEX(7, 48, 7, 198, 137, 90),
  STEVE_VERTEX(7, 0, 7, 198, 137, 90), STEVE_VERTEX(-7, 0, 7, 198, 137, 90),
  STEVE_VERTEX(7, 48, -7, 198, 137, 90), STEVE_VERTEX(-7, 48, -7, 198, 137, 90),
  STEVE_VERTEX(-7, 0, -7, 198, 137, 90), STEVE_VERTEX(7, 0, -7, 198, 137, 90)
};

static Gfx steve_box_display_list[] = {
  gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0),
  gsSP2Triangles(4, 5, 6, 0, 4, 6, 7, 0),
  gsSP2Triangles(1, 4, 7, 0, 1, 7, 2, 0),
  gsSP2Triangles(5, 0, 3, 0, 5, 3, 6, 0),
  gsSP2Triangles(5, 4, 1, 0, 5, 1, 0, 0),
  gsSP2Triangles(3, 2, 7, 0, 3, 7, 6, 0),
  gsSPEndDisplayList()
};

static Gfx sword_blade_display_list[] = {
  gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0),
  gsSP1Triangle(0, 3, 4, 0),
  gsSP2Triangles(5, 7, 6, 0, 5, 8, 7, 0),
  gsSP1Triangle(5, 9, 8, 0),
  gsSP2Triangles(0, 5, 6, 0, 0, 6, 1, 0),
  gsSP2Triangles(1, 6, 7, 0, 1, 7, 2, 0),
  gsSP2Triangles(2, 7, 8, 0, 2, 8, 3, 0),
  gsSP2Triangles(3, 8, 9, 0, 3, 9, 4, 0),
  gsSP2Triangles(4, 9, 5, 0, 4, 5, 0, 0),
  gsSPEndDisplayList()
};

static Gfx steve_eyes_display_list[] = {
  gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0),
  gsSP2Triangles(4, 5, 6, 0, 4, 6, 7, 0),
  gsSPEndDisplayList()
};

static Texture *loaded_texture;

void loadTexture(Texture *texture);

static Vp full_viewport = {
    SCREEN_WD*2, SCREEN_HT*2, G_MAXZ/2, 0,
    SCREEN_WD*2, SCREEN_HT*2, G_MAXZ/2, 0,
};

/* N64 viewport coordinates are 10.2 fixed point.  These two viewports fill
   the same framebuffer as solo play, so split-screen does not double fill-rate. */
static Vp two_player_viewports[2] = {
  {SCREEN_WD*2, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD*2, SCREEN_HT, G_MAXZ/2, 0},
  {SCREEN_WD*2, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD*2, SCREEN_HT*3, G_MAXZ/2, 0}
};

/* Four players use 160x120 quadrants.  vscale/vtrans use the N64's 10.2
   viewport representation, hence the doubled dimensions and centres. */
static Vp four_player_viewports[MAX_PLAYERS] = {
  {SCREEN_WD, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD, SCREEN_HT, G_MAXZ/2, 0},
  {SCREEN_WD, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD*3, SCREEN_HT, G_MAXZ/2, 0},
  {SCREEN_WD, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD, SCREEN_HT*3, G_MAXZ/2, 0},
  {SCREEN_WD, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD*3, SCREEN_HT*3, G_MAXZ/2, 0}
};

static u8 usesFourPlayerLayout(void) {
  return active_player_count >= 3;
}

static u32 playerViewportWidth(void) {
  return usesFourPlayerLayout() ? SCREEN_WD / 2 : SCREEN_WD;
}

static u32 playerViewportHeight(void) {
  return active_player_count > 1 ? SCREEN_HT / 2 : SCREEN_HT;
}

static u32 playerViewportX(u8 player_num) {
  return usesFourPlayerLayout() ? (player_num & 1) * (SCREEN_WD / 2) : 0;
}

static u32 playerViewportY(u8 player_num) {
  return active_player_count > 1 ? (usesFourPlayerLayout() ? player_num / 2 :
    player_num) * (SCREEN_HT / 2) : 0;
}

static void selectPlayerViewport(u8 player_num) {
  u32 x = playerViewportX(player_num);
  u32 y = playerViewportY(player_num);

  if (usesFourPlayerLayout()) {
    gSPViewport(dlp++, &four_player_viewports[player_num]);
  } else {
    gSPViewport(dlp++, &two_player_viewports[player_num]);
  }
  gDPSetScissor(dlp++, G_SC_NON_INTERLACE, x, y,
    x + playerViewportWidth(), y + playerViewportHeight());
}

static void setLightColor(Light *light, SkyColor color) {
  light->l.col[0] = color.r;
  light->l.col[1] = color.g;
  light->l.col[2] = color.b;
  light->l.colc[0] = color.r;
  light->l.colc[1] = color.g;
  light->l.colc[2] = color.b;
}

static void setAmbientColor(Ambient *ambient, SkyColor color) {
  ambient->l.col[0] = color.r;
  ambient->l.col[1] = color.g;
  ambient->l.col[2] = color.b;
  ambient->l.colc[0] = color.r;
  ambient->l.colc[1] = color.g;
  ambient->l.colc[2] = color.b;
}

/*
 * Everything drawn after the terrain -- mobs, other players, dropped items,
 * falling trees, the placed detail boxes -- carries vertex colours instead of
 * normals, so the RSP light cannot touch it and it stayed at full daylight
 * colour on ground that had gone dark.  This is the flat stand-in for that
 * light: the same ambient the terrain gets, plus the share of the directional
 * term a surface of no particular orientation catches.  It is handed to the
 * RDP as a primitive colour and the entity passes modulate their shade by it.
 *
 * The share is the average of N.L over the faces of a box that is mostly
 * facing sideways.  Higher and a sheep at noon comes out brighter than the
 * grass it is standing on; lower and it reads as permanently in shadow.
 */
#define ENTITY_DIRECT_SHARE .45f

static SkyColor entity_tint;

static u8 addLightChannel(u8 ambient, u8 direct) {
  u32 total = ambient + (u32) (direct * ENTITY_DIRECT_SHARE);

  return total > 255 ? 255 : (u8) total;
}

static void setWorldLight(u8 player_num) {
  Lights1 *lights = &world_lights[dl_no][player_num];
  Vector3 direction = dayCycleLightDirection();
  SkyColor ambient = dayCycleAmbientLight();
  SkyColor direct = dayCycleDirectLight();
  u8 torch = detailLightAt(players[player_num].position);

  /* One nearest-light sample per viewport sells a warm pool around the
     player without a per-voxel light volume or terrain remesh.  It is most
     visible at night and naturally becomes subtle in daylight. */
  ambient.r = min(255, ambient.r + (torch * 3) / 4);
  ambient.g = min(255, ambient.g + (torch * 2) / 5);
  ambient.b = min(255, ambient.b + torch / 8);
  setAmbientColor(&lights->a, ambient);
  setLightColor(&lights->l[0], direct);
  lights->l[0].l.dir[0] = direction.x * 127;
  lights->l[0].l.dir[1] = direction.y * 127;
  lights->l[0].l.dir[2] = direction.z * 127;

  /* Derived from the torch-boosted ambient, so an animal that walks into a
     lit doorway brightens with the floor under it. */
  entity_tint.r = addLightChannel(ambient.r, direct.r);
  entity_tint.g = addLightChannel(ambient.g, direct.g);
  entity_tint.b = addLightChannel(ambient.b, direct.b);
}

/*
 * PRIM * SHADE for untextured entity geometry, and PRIM * TEXEL0 for the
 * textured cubes, whose vertices are all white.  gbi.h has no canned mode for
 * the first, and the second is only correct because nothing in these passes
 * relies on shade for anything but the flat colour the tint replaces.
 */
static void setEntityShadeCombine(void) {
  gDPSetCombineLERP(dlp++, PRIMITIVE, 0, SHADE, 0, 0, 0, 0, SHADE,
    PRIMITIVE, 0, SHADE, 0, 0, 0, 0, SHADE);
}

static void setEntityTint(SkyColor tint) {
  gDPSetPrimColor(dlp++, 0, 0, tint.r, tint.g, tint.b, 255);
}

/* Geometry that is a light source rather than a lit surface takes this
   instead: a torch flame the day/night tint dimmed would be absurd. */
static const SkyColor emissive_tint = {255, 255, 255};

static void setPreviewLight() {
  Lights0 *lights = &preview_lights[dl_no];
  SkyColor ambient = {152, 164, 174};
  SkyColor direct = {94, 88, 70};

  setAmbientColor(&lights->a, ambient);
  setLightColor(&lights->l[0], direct);
  /* A fixed warm key light gives the terrain relief at any saved world
     time, rather than flattening the showcase with an all-ambient pass. */
  lights->l[0].l.dir[0] = -48;
  lights->l[0].l.dir[1] = 104;
  lights->l[0].l.dir[2] = 54;
}

static void setCelestialVertex(Vtx *vertex, Vector3 point, u16 s, u16 t,
    SkyColor tint) {
  vertex->v.ob[0] = point.x;
  vertex->v.ob[1] = point.y;
  vertex->v.ob[2] = point.z;
  vertex->v.flag = 0;
  vertex->v.tc[0] = s;
  vertex->v.tc[1] = t;
  /* Shade carries the body's colour; the combiner modulates the tile by it,
     so a horizon-red Sun costs nothing beyond these three bytes. */
  vertex->v.cn[0] = tint.r;
  vertex->v.cn[1] = tint.g;
  vertex->v.cn[2] = tint.b;
  vertex->v.cn[3] = 255;
}

/*
 * Texture coordinates are texels * 64, not * 32.  The whole renderer runs
 * with an 0x8000 (x0.5) SP texture scale -- terrain spans 1024 across a
 * 16-texel block -- so the halved convention this used to follow addressed
 * only the tile's top-left 8x8 quadrant and stretched it over the sprite.
 */
#define CELESTIAL_TILE_TC (16 << 6)

static void makeCelestialQuad(Vtx *vertices, Vector3 center, Vector3 right,
    Vector3 up, float size, SkyColor tint) {
  Vector3 horizontal = mul(right, size);
  Vector3 vertical = mul(up, size);
  Vector3 top_left = add(add(center, vertical), mul(horizontal, -1.f));
  Vector3 top_right = add(add(center, vertical), horizontal);
  Vector3 bottom_right = add(add(center, mul(vertical, -1.f)), horizontal);
  Vector3 bottom_left = add(add(center, mul(vertical, -1.f)), mul(horizontal, -1.f));

  setCelestialVertex(&vertices[0], top_left, 0, 0, tint);
  setCelestialVertex(&vertices[1], top_right, CELESTIAL_TILE_TC, 0, tint);
  setCelestialVertex(&vertices[2], bottom_right, CELESTIAL_TILE_TC,
    CELESTIAL_TILE_TC, tint);
  setCelestialVertex(&vertices[3], bottom_left, 0, CELESTIAL_TILE_TC, tint);
}

static Texture *moonTexture() {
  static Texture *moon_textures[] = {
    &moon_0_texture, &moon_1_texture, &moon_2_texture, &moon_3_texture,
    &moon_4_texture, &moon_5_texture, &moon_6_texture, &moon_7_texture
  };
  return moon_textures[dayCycleMoonPhase()];
}

/*
 * Sky pass.  Runs first in each viewport, before the terrain, and writes no
 * depth at all: everything drawn afterwards simply paints over it, which is
 * both correct (the sky is behind the world by definition) and immune to the
 * far-plane depth crunch a z-buffered sprite at the edge of the frustum has
 * to survive.
 */
static void drawCelestialBodies(u8 player_num) {
  Player *player = &players[player_num];
  float altitude = dayCycleSunAltitude();
  Vector3 camera = playerCameraPosition(player_num);
  Vector3 right = rotateY((Vector3) {1, 0, 0}, -player->yaw);
  Vector3 up = rotateY(rotateX((Vector3) {0, 1, 0}, player->pitch), -player->yaw);
  Vtx *vertices = celestial_verts[dl_no][player_num];
  u8 sun_up = altitude >= CELESTIAL_SET_ALTITUDE;
  u8 moon_up = -altitude >= CELESTIAL_SET_ALTITUDE;

  if (!sun_up && !moon_up) {
    return;
  }

  /* The pipe is still carrying the sky clear's fill rectangles; draining it
     before the mode changes below is the lockup documented in the README. */
  gDPPipeSync(dlp++);
  gSPClearGeometryMode(dlp++, G_CULL_BACK | G_LIGHTING | G_ZBUFFER);
  /* DECALA, not plain MODULATE: the plain form's alpha equation is
     (0,0,0,SHADE), which discards the tile's alpha entirely and drew the
     Moon's transparent surround as an opaque black square. */
  gDPSetCombineMode(dlp++, G_CC_MODULATERGBDECALA, G_CC_MODULATERGBDECALA);
  gDPSetRenderMode(dlp++, G_RM_AA_TEX_EDGE, G_RM_AA_TEX_EDGE2);
  /* G_AC_THRESHOLD compares against this register.  Nothing else in the
     program sets it, so leaving it out left the cutout at whatever the RDP
     happened to be holding. */
  gDPSetBlendColor(dlp++, 0, 0, 0, 1);
  gDPSetAlphaCompare(dlp++, G_AC_THRESHOLD);

  /* Camera-anchored, origin-relative -- the same subtraction every other
     world-space matrix in the renderer makes. */
  guTranslate(&celestial_model[dl_no][player_num],
    camera.x - render_origin_units_x, camera.y,
    camera.z - render_origin_units_z);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&celestial_model[dl_no][player_num]),
    G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);

  if (sun_up) {
    makeCelestialQuad(vertices, mul(dayCycleSunDirection(), CELESTIAL_DISTANCE),
      right, up, SUN_SIZE, dayCycleSunTint());
    loadTexture(&sun_texture);
    gSPVertex(dlp++, vertices, 4, 0);
    gSP1Quadrangle(dlp++, 0, 1, 2, 3, 0);
  }
  if (moon_up) {
    makeCelestialQuad(vertices + 4,
      mul(dayCycleMoonDirection(), CELESTIAL_DISTANCE),
      right, up, MOON_SIZE, dayCycleMoonTint());
    loadTexture(moonTexture());
    gSPVertex(dlp++, vertices + 4, 4, 0);
    gSP1Quadrangle(dlp++, 0, 1, 2, 3, 0);
  }

  gDPSetAlphaCompare(dlp++, G_AC_NONE);
  /* Terrain z-buffers; only the sky does not. */
  gSPSetGeometryMode(dlp++, G_ZBUFFER);
  loaded_texture = NULL;
}

static Gfx setup_display_list[] = {
  gsSPSegment(0, 0x0),
  gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),
  gsDPSetScissor(G_SC_NON_INTERLACE, 0,0, SCREEN_WD,SCREEN_HT),
  gsSPEndDisplayList()
};

static Gfx draw_setup_display_list[] = {
  gsDPSetCycleType(G_CYC_1CYCLE),
  gsDPSetRenderMode(G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2),
  gsSPClearGeometryMode(0xFFFFFFFF),
  gsSPSetGeometryMode(G_ZBUFFER | G_CULL_BACK | G_SHADE | G_SHADING_SMOOTH),
  
  gsSPTexture(0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON),
  gsDPSetTexturePersp(G_TP_PERSP),
  gsDPSetCombineMode(G_CC_MODULATERGB, G_CC_MODULATERGB),
  gsDPSetTextureLUT(G_TT_RGBA16),
  gsSPEndDisplayList()
};

static Gfx wireframe_setup_display_list[] = {
  /* Cycle type and render mode below reconfigure the RDP, which is only safe
     once the pipe has drained. */
  gsDPPipeSync(),
  gsDPSetCycleType(G_CYC_1CYCLE),
  gsDPSetRenderMode(G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2),
  /*
   * The outline set no combiner of its own, so it drew with whatever the last
   * thing on screen had left: shade if a mob or an untextured drop was in
   * view, modulate against a stale texture tile otherwise.  That made the
   * targeting box brighter or dimmer depending on whether an animal happened
   * to wander past.  Shade-only ties it to the vertex colour and nothing else.
   */
  gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),
  gsSPClearGeometryMode(0xFFFFFFFF),
  gsSPSetGeometryMode(G_ZBUFFER | G_CULL_BACK | G_SHADE | G_SHADING_SMOOTH),
  gsSPEndDisplayList()
};

static Gfx wireframe_display_list[] = {
  gsSPLine3D(0, 1, 0),
  gsSPLine3D(1, 2, 0),
  gsSPLine3D(2, 3, 0),
  gsSPLine3D(3, 0, 0),
  
  gsSPLine3D(4, 5, 0),
  gsSPLine3D(5, 6, 0),
  gsSPLine3D(6, 7, 0),
  gsSPLine3D(7, 4, 0),
  
  gsSPLine3D(0, 5, 0),
  gsSPLine3D(1, 4, 0),
  gsSPLine3D(2, 7, 0),
  gsSPLine3D(3, 6, 0),

  gsSPEndDisplayList()
};

/* Three stages of increasingly dense fractures are drawn over a log while it
   is being punched.  They use the same block-local coordinate space as the
   targeting outline, so no world mesh rebuild is needed every frame. */
#define CRACK_VERTEX(x, y, z) {x, y, z, 0, 0, 0, 66, 38, 20, 255}

static Vtx breaking_crack_verts[] = {
  /* Front face */
  CRACK_VERTEX(4, 65, 66), CRACK_VERTEX(22, 43, 66),
  CRACK_VERTEX(13, 19, 66), CRACK_VERTEX(34, -1, 66),
  CRACK_VERTEX(61, 65, 66), CRACK_VERTEX(45, 42, 66),
  CRACK_VERTEX(54, 21, 66),
  /* Right face */
  CRACK_VERTEX(66, 60, 6), CRACK_VERTEX(66, 40, 24),
  CRACK_VERTEX(66, 18, 13), CRACK_VERTEX(66, -1, 34),
  /* Top face */
  CRACK_VERTEX(7, 66, 9), CRACK_VERTEX(28, 66, 29),
  CRACK_VERTEX(17, 66, 51), CRACK_VERTEX(39, 66, 66),
  /* Left and back faces keep the effect visible from every direction. */
  CRACK_VERTEX(-2, 59, 60), CRACK_VERTEX(-2, 40, 42),
  CRACK_VERTEX(-2, 18, 53), CRACK_VERTEX(-2, -1, 31),
  CRACK_VERTEX(60, 59, -2), CRACK_VERTEX(41, 39, -2),
  CRACK_VERTEX(53, 18, -2), CRACK_VERTEX(32, -1, -2)
};

static Gfx breaking_crack_stage_one[] = {
  gsSPLine3D(0, 1, 0),
  gsSPLine3D(1, 2, 0),
  gsSPLine3D(2, 3, 0),
  gsSPEndDisplayList()
};

static Gfx breaking_crack_stage_two[] = {
  gsSPLine3D(0, 1, 0), gsSPLine3D(1, 2, 0), gsSPLine3D(2, 3, 0),
  gsSPLine3D(4, 5, 0), gsSPLine3D(5, 6, 0), gsSPLine3D(6, 3, 0),
  gsSPLine3D(7, 8, 0), gsSPLine3D(8, 9, 0),
  gsSPEndDisplayList()
};

static Gfx breaking_crack_stage_three[] = {
  gsSPLine3D(0, 1, 0), gsSPLine3D(1, 2, 0), gsSPLine3D(2, 3, 0),
  gsSPLine3D(4, 5, 0), gsSPLine3D(5, 6, 0), gsSPLine3D(6, 3, 0),
  gsSPLine3D(7, 8, 0), gsSPLine3D(8, 9, 0), gsSPLine3D(9, 10, 0),
  gsSPLine3D(11, 12, 0), gsSPLine3D(12, 13, 0), gsSPLine3D(13, 14, 0),
  gsSPLine3D(15, 16, 0), gsSPLine3D(16, 17, 0), gsSPLine3D(17, 18, 0),
  gsSPLine3D(19, 20, 0), gsSPLine3D(20, 21, 0), gsSPLine3D(21, 22, 0),
  gsSPEndDisplayList()
};

static Gfx *breaking_crack_stages[] = {
  breaking_crack_stage_one,
  breaking_crack_stage_two,
  breaking_crack_stage_three
};

void clearBuffers(u16 bg_color) {
  gDPSetDepthImage(dlp++, OS_K0_TO_PHYSICAL(nuGfxZBuffer));
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetColorImage(dlp++, G_IM_FMT_RGBA, G_IM_SIZ_16b,SCREEN_WD,
		   OS_K0_TO_PHYSICAL(nuGfxZBuffer));
  gDPSetFillColor(dlp++,(GPACK_ZDZ(G_MAXFBZ,0) << 16 |
			       GPACK_ZDZ(G_MAXFBZ,0)));
  gDPFillRectangle(dlp++, 0, 0, SCREEN_WD-1, SCREEN_HT-1);
  gDPPipeSync(dlp++);
  
  gDPSetColorImage(dlp++, G_IM_FMT_RGBA, G_IM_SIZ_16b, SCREEN_WD,
		   osVirtualToPhysical(nuGfxCfb_ptr));
  gDPSetFillColor(dlp++, (bg_color << 16 | bg_color));
  gDPFillRectangle(dlp++, 0, 0, SCREEN_WD-1, SCREEN_HT-1);
  gDPPipeSync(dlp++);
}

void loadTexture(Texture *texture) {
  if (texture != loaded_texture) {
    loaded_texture = texture;
    gDPLoadTLUT_pal16(dlp++, 0, texture->pallet);
    gDPLoadTextureBlock_4b(dlp++, texture->color_indices, G_IM_FMT_CI,
                      16, 16, 0, G_TX_WRAP, loaded_texture == &grass_side_texture? G_TX_CLAMP : G_TX_WRAP, 
                      4, 4, G_TX_NOLOD, G_TX_NOLOD);
  }
}

/* Index into mesh_blocks of the block for (slot, generation), or -1. */
static int meshBlockFind(u16 slot, u8 generation) {
  u16 i;

  for (i = 0; i < mesh_block_count; i++) {
    if (mesh_blocks[i].slot == slot &&
        mesh_blocks[i].generation == generation) {
      return i;
    }
  }
  return -1;
}

static void meshBlockRemove(u16 index) {
  u16 i;

  mesh_arena_used -= mesh_blocks[index].length;
  for (i = index; i + 1 < mesh_block_count; i++) {
    mesh_blocks[i] = mesh_blocks[i + 1];
  }
  mesh_block_count--;
}

static void meshBlockFree(u16 slot, u8 generation) {
  int index = meshBlockFind(slot, generation);

  if (index >= 0) {
    meshBlockRemove((u16) index);
  }
}

/*
 * First-fit into the lowest gap.  The list is sorted by start, so gaps are
 * simply the space between consecutive blocks; defrag squeezes them out one
 * block per frame, which keeps first-fit from ever fragmenting for long.
 * Returns the Gfx offset, or -1 when nothing fits.
 */
static s32 meshBlockAlloc(u32 length, u32 verts, u16 slot, u8 generation) {
  u32 prev_end = 0;
  u16 i, insert_at;
  u32 start;

  if (mesh_block_count >= MAX_MESH_BLOCKS) {
    return -1;
  }
  insert_at = mesh_block_count;
  start = MESH_ARENA_SIZE;  /* sentinel: no gap found */
  for (i = 0; i < mesh_block_count; i++) {
    if (mesh_blocks[i].start - prev_end >= length) {
      insert_at = i;
      start = prev_end;
      break;
    }
    prev_end = mesh_blocks[i].start + mesh_blocks[i].length;
  }
  if (start == MESH_ARENA_SIZE) {
    if (MESH_ARENA_SIZE - prev_end < length) {
      return -1;
    }
    start = prev_end;
  }
  for (i = mesh_block_count; i > insert_at; i--) {
    mesh_blocks[i] = mesh_blocks[i - 1];
  }
  mesh_blocks[insert_at].start = start;
  mesh_blocks[insert_at].length = length;
  mesh_blocks[insert_at].verts = verts;
  mesh_blocks[insert_at].slot = slot;
  mesh_blocks[insert_at].generation = generation;
  mesh_block_count++;
  mesh_arena_used += length;
  return (s32) start;
}

/*
 * Slide one block down into the lowest gap.  Only ever called from the
 * graphics callback with no task in flight, and every pointer that names
 * the block -- the per-texture starts and the vertex addresses embedded in
 * its own gSPVertex commands -- is rewritten before this callback's draw()
 * submits, so no frame observes the move.
 */
#define G_VTX_OPCODE 0x01

static void meshDefragStep(void) {
  u32 prev_end = 0;
  u16 i;
  u8 texture;

  for (i = 0; i < mesh_block_count; i++) {
    MeshBlock *block = &mesh_blocks[i];
    u32 gap = block->start - prev_end;
    s32 delta_bytes;
    u32 word;
    Gfx *cmd, *cmd_end;

    if (gap == 0) {
      prev_end = block->start + block->length;
      continue;
    }
    delta_bytes = -(s32) (gap * sizeof (Gfx));
    /* dst < src with possible overlap; copying forward is safe. */
    {
      u64 *dst = (u64 *) (mesh_arena + prev_end);
      u64 *src = (u64 *) (mesh_arena + block->start);
      u32 words = block->length;

      for (word = 0; word < words; word++) {
        dst[word] = src[word];
      }
    }
    /* Patch the vertex loads whose addresses fall inside the block that
       just moved -- a full-detail column's baked vertices travel with it.
       A shell column's vertex loads reference the shared static tables,
       which sit outside the old range and must be left alone. */
    {
      u32 old_lo = (u32) (mesh_arena + block->start);
      u32 old_hi = old_lo + block->length * sizeof (Gfx);

      cmd = mesh_arena + prev_end + block->verts;
      cmd_end = mesh_arena + prev_end + block->length;
      for (; cmd < cmd_end; cmd++) {
        if ((cmd->words.w0 >> 24) == G_VTX_OPCODE &&
            cmd->words.w1 >= old_lo && cmd->words.w1 < old_hi) {
          cmd->words.w1 = (u32) ((s32) cmd->words.w1 + delta_bytes);
        }
      }
    }
    /* Re-aim whichever pointer set references this block. */
    {
      Gfx *(*starts)[WINDOW_SLOTS] =
        block->generation == mesh_generation ? column_starts : staged_starts;

      for (texture = 0; texture < NUM_TEXTURES; texture++) {
        if (starts[texture][block->slot] != empty_column_display_list) {
          starts[texture][block->slot] -= gap;
        }
      }
    }
    block->start = prev_end;
    return;
  }
}

/*
 * Which texture bank draws each block face.  Derived once from textures[]'s
 * FaceSpec lists, so one pass over a column's quads can bucket them by
 * texture instead of rescanning every quad sixteen times -- once per bank --
 * as the old per-texture emitters did.
 */
#define FACE_TEXTURE_NONE 0xFF
static u8 face_texture[16][6];

static void buildFaceTextureTable(void) {
  u8 t, f, i;

  for (t = 0; t < 16; t++) {
    for (f = 0; f < 6; f++) {
      face_texture[t][f] = FACE_TEXTURE_NONE;
    }
  }
  for (t = 0; t < NUM_TEXTURES; t++) {
    for (i = 0; i < textures[t]->n_faces; i++) {
      FaceSpec *spec = &textures[t]->faces[i];

      if (spec->top) {
        face_texture[spec->block][TOP] = t;
      }
      if (spec->bottom) {
        face_texture[spec->block][BOTTOM] = t;
      }
      if (spec->sides) {
        face_texture[spec->block][FRONT] = t;
        face_texture[spec->block][BACK] = t;
        face_texture[spec->block][LEFT] = t;
        face_texture[spec->block][RIGHT] = t;
      }
    }
  }
}

/*
 * Level of detail per column mesh.
 *
 * FULL is the gameplay mesh in baked-vertex form: every quad's four vertices
 * are copied out of the shared origin-space table, pre-translated into
 * column-local space, and referenced by batched gSPVertex loads under a
 * single column matrix.  That trades roughly 81 bytes a quad against the old
 * 32, to remove the two per-quad matrix operations (a DMA'd LOAD and a full
 * fixed-point MUL each) that dominated the RSP's terrain cost.
 *
 * SHELL is the two-block surface skin: about 20-35 quads a column instead
 * of ~110-270.  It bakes exactly like FULL -- the compact matrix-pair
 * format it used to keep is gone, because its two per-quad matrix
 * operations across a hundred visible far columns were the measured bulk
 * of the RSP's standing frame; the arena grew by the per-block matrix
 * table those operations needed.  What SHELL still saves is quad count,
 * and it skips T-junction refinement -- the seams it repairs are
 * sub-pixel at that distance, and the refinement was the dominant cost of
 * compiling a freshly streamed column, which is exactly the moment the frame
 * can least afford it.
 *
 * The near disc is promoted at PROMOTE and demoted at DEMOTE, with the gap
 * between them the hysteresis that stops a column on the boundary from
 * re-meshing every time the player takes a step across it.
 */
#define MESH_LOD_FULL 0
#define MESH_LOD_SHELL 1
/*
 * Radius 1: full detail is a courtesy bubble around the player's own feet
 * and everything else is the surface shell.  W/B measurement on hardware
 * put the settled standing-still frame squarely on the RSP transform of
 * the full-detail disc -- the radius-3 disc alone held the game near 12-15
 * fps -- and the chosen trade is explicitly view distance over near
 * detail: the far ring is what the player asked to keep.
 */
#define MESH_LOD_PROMOTE_RADIUS 1
#define MESH_LOD_DEMOTE_RADIUS 3
/*
 * Runtime copies of the LOD radii, for the Z + C-left diagnostics preset
 * chord.  The right radius is a picture-quality trade that can only be
 * judged on a CRT -- so it is a knob, not a constant.  Columns re-LOD on
 * their own after a change: the periodic needs-mesh scan compares each
 * column's compiled LOD against these.
 */
u8 mesh_lod_promote_radius = MESH_LOD_PROMOTE_RADIUS;
u8 mesh_lod_demote_radius = MESH_LOD_DEMOTE_RADIUS;
static u8 column_mesh_lod[WINDOW_SLOTS];
static u8 staged_mesh_lod[WINDOW_SLOTS];

/* Chebyshev chunk distance from a column to the nearest active player --
   every LOD and compaction decision keys off this, so split-screen terrain
   follows all players rather than only player one. */
static int columnPlayerDistance(int cx, int cz) {
  int best = 1000;
  u8 i;

  for (i = 0; i < active_player_count; i++) {
    int pcx = floor(players[i].position.x / (BLOCK_SIZE * CHUNK_SIZE));
    int pcz = floor(players[i].position.z / (BLOCK_SIZE * CHUNK_SIZE));
    int dx = cx - pcx;
    int dz = cz - pcz;
    int d;

    if (dx < 0) dx = -dx;
    if (dz < 0) dz = -dz;
    d = dx > dz ? dx : dz;
    if (d < best) {
      best = d;
    }
  }
  return best;
}

/* The LOD a rebuild should compile now.  The midpoint between the promote and
   demote radii, so a column marked stale by either hysteresis edge settles
   into a state that edge no longer complains about.  A column whose deferred
   underground has not been carved yet only rates a shell -- a full mesh of it
   would bake solid stone where the caves belong. */
static u8 meshLodFor(int cx, int cz) {
  return columnPlayerDistance(cx, cz) <= mesh_lod_promote_radius + 1 &&
    worldColumnDeep(cx, cz) ? MESH_LOD_FULL : MESH_LOD_SHELL;
}

/*
 * One column's quads, resolved out of the axis-major greedy scratch into
 * plain column-local records, bucket-counted by texture.  Resolving once and
 * emitting per texture from this flat array replaced sixteen full rescans of
 * the scratch per column.
 *
 * The cap covers every realistic column several times over (a typical column
 * is 110-270 quads); a pathological player-built column loses its excess
 * faces, which is the same degradation the arena-space check always had.
 */
#define COLUMN_QUAD_CAP 1536
typedef struct {
  u8 x;       /* block offsets in column space; y spans the full 0..31 */
  u8 y;
  u8 z;
  u8 width;   /* stored minus one, as the shared vertex table indexes them */
  u8 height;
  u8 face;
  u8 texture;
  u8 water_top;
} BakedQuad;
static BakedQuad column_baked[COLUMN_QUAD_CAP];
static u16 column_baked_total;
static u16 column_baked_counts[NUM_TEXTURES];

/* TRUE for quads the surface shell keeps: the skin the world shows from a
   distance.  Same filter the scenic preview always used. */
static u8 shellKeepsQuad(int cx, int cz, u8 bx, u8 column_y, u8 bz, u8 height,
    u8 face) {
  int max_quad_y = column_y;
  int surface_y;

  /* The fixed world's underground outer wall is one long greedy quad from
     bedrock to surface -- the "chunks below the map" silhouette the title
     screen orbit used to show.  Only a whole-world build has a world edge to
     hide; a column rebuilt during play has streamed neighbours. */
  if (building_whole_world &&
      (cx == 0 || cx == CHUNKS_X - 1 || cz == 0 || cz == CHUNKS_Z - 1) &&
      face != TOP && face != BOTTOM) {
    return FALSE;
  }
  if (face == BOTTOM) {
    return FALSE;
  }
  if (face == TOP) {
    max_quad_y++;
  } else {
    max_quad_y += height + 1;
  }
  surface_y = surface_heights[bx][bz];
  return max_quad_y >= surface_y - 1;
}

static void resolveAxisQuads(DualQuadList *axis_quads, int cx, int cz, u8 cy,
    u8 axes, u8 face_front, u8 face_back, u8 shell) {
  u8 br, i;

  for (br = 0; br < CHUNK_SIZE; br++) {
    DualQuadList *both_quads = &axis_quads[br];
    u8 n = both_quads->n_front + both_quads->n_back;

    for (i = 0; i < n; i++) {
      Quad *quad = &both_quads->quads[i];
      u8 face = i < both_quads->n_front ? face_front : face_back;
      u8 texture = face_texture[quad->block][face];
      u8 bx, by, bz;
      BakedQuad *baked;

      if (texture == FACE_TEXTURE_NONE) {
        continue;
      }
      if (axes == ZXY) {
        bx = quad->bs; by = quad->bt; bz = br;
      } else if (axes == XZY) {
        bx = br; by = quad->bt; bz = quad->bs;
      } else {
        bx = quad->bs; by = br; bz = quad->bt;
      }
      if (shell && !shellKeepsQuad(cx, cz, bx,
          (u8) (cy * CHUNK_SIZE + by), bz, quad->height, face)) {
        continue;
      }
      if (column_baked_total >= COLUMN_QUAD_CAP) {
        return;
      }
      baked = &column_baked[column_baked_total++];
      baked->x = bx;
      baked->y = (u8) (cy * CHUNK_SIZE + by);
      baked->z = bz;
      baked->width = quad->width;
      baked->height = quad->height;
      baked->face = face;
      baked->texture = texture;
      baked->water_top = quad->block == WATER && face == TOP;
      column_baked_counts[texture]++;
    }
  }
}

static void resolveColumnQuads(int cx, int cz, u8 lod) {
  u8 cy, texture;
  u8 shell = lod == MESH_LOD_SHELL;

  column_baked_total = 0;
  for (texture = 0; texture < NUM_TEXTURES; texture++) {
    column_baked_counts[texture] = 0;
  }
  for (cy = 0; cy < CHUNKS_Y; cy++) {
    ChunkQuads *cq = &column_quads[cy];

    resolveAxisQuads(cq->z_quads, cx, cz, cy, ZXY, FRONT, BACK, shell);
    resolveAxisQuads(cq->x_quads, cx, cz, cy, XZY, RIGHT, LEFT, shell);
    resolveAxisQuads(cq->y_quads, cx, cz, cy, YXZ, TOP, BOTTOM, shell);
  }
}

/* Quads per gSPVertex batch: 8 quads' 32 vertices fill the F3DEX2 vertex
   buffer exactly. */
#define BAKED_QUADS_PER_BATCH 8

/* Gfx cost of one texture's command segment; the vertex data is accounted
   separately as the block's leading region. */
static u32 textureSegmentSize(u16 n) {
  if (n == 0) {
    return 0;
  }
  return 1 + ((u32) n + BAKED_QUADS_PER_BATCH - 1) / BAKED_QUADS_PER_BATCH +
    (u32) n + 1;
}

/* Emit one texture's segment at the cursors; returns the segment entry (or
   the shared empty list).  The caller sized the block, so this cannot run
   out of room. */
static Gfx *emitColumnTextureDL(u32 slot, u8 texture,
    Vtx **verts_cursor, Gfx **cmds_cursor) {
  u16 n = column_baked_counts[texture];
  u16 i, emitted, batch_n;
  Vtx *verts;
  Gfx *cmds = *cmds_cursor;
  Gfx *segment_start;

  if (n == 0) {
    /* Most columns use only a subset of the texture bank.  Sharing one empty
       list saves an EndDisplayList command for every absent material. */
    return empty_column_display_list;
  }

  /*
   * Baked-vertex form.  The vertices go to the block's leading data region
   * and are never executed; the published pointer enters at the command
   * stream.  Vertices are column-local (y spans all four chunks, 0..2047
   * units), so one matrix serves the whole column and batches cross chunk
   * seams freely.  A Vtx is two Gfx of arena space.
   */
  verts = *verts_cursor;
  emitted = 0;
  for (i = 0; i < column_baked_total; i++) {
    BakedQuad *q = &column_baked[i];
    const Vtx *src;
    u8 v;

    if (q->texture != texture) {
      continue;
    }
    src = q->water_top ? WATER_TOP_QUAD_ADDR(q->width, q->height) :
      QUAD_ADDR(q->face, q->width, q->height);
    for (v = 0; v < 4; v++) {
      Vtx *out = &verts[(u32) emitted * 4 + v];

      *out = src[v];
      out->v.ob[0] += (s16) ((s16) q->x * BLOCK_SIZE);
      out->v.ob[1] += (s16) ((s16) q->y * BLOCK_SIZE);
      out->v.ob[2] += (s16) ((s16) q->z * BLOCK_SIZE);
    }
    emitted++;
  }
  *verts_cursor = verts + (u32) n * 4;

  segment_start = cmds;
  gSPMatrix(cmds++, OS_K0_TO_PHYSICAL(&c_models[slot]),
    G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
  for (i = 0; i < n; i += BAKED_QUADS_PER_BATCH) {
    u16 batch;

    batch_n = n - i > BAKED_QUADS_PER_BATCH ? BAKED_QUADS_PER_BATCH :
      (u16) (n - i);
    gSPVertex(cmds++, verts + (u32) i * 4, batch_n * 4, 0);
    for (batch = 0; batch < batch_n; batch++) {
      gSP1Quadrangle(cmds++, batch * 4 + 3, batch * 4 + 2, batch * 4 + 1,
        batch * 4 + 0, 0);
    }
  }
  gSPEndDisplayList(cmds++);
  *cmds_cursor = cmds;
  return segment_start;
}

static u8 mesh_build_active;
static u16 mesh_build_cursor;
static u8 mesh_build_complete;

static u8 makeColumnDisplayLists(int cx, int cz) {
  u8 texture;
  u32 slot = WINDOW_SLOT(cx, cz);
  u8 lod = meshLodFor(cx, cz);
  /* World builds emit the incoming world's generation behind staged
     pointers; gameplay rebuilds replace the live column directly. */
  u8 generation = mesh_build_active ?
    mesh_building_generation : mesh_generation;
  Gfx *(*starts)[WINDOW_SLOTS] =
    mesh_build_active ? staged_starts : column_starts;
  u8 *meshed = mesh_build_active ? staged_meshed : column_meshed;
  u8 *lods = mesh_build_active ? staged_mesh_lod : column_mesh_lod;
  u32 old_start = 0;
  u8 had_old = FALSE;
  u32 total_cmds = 0;
  u32 total_verts;
  s32 block_start;
  Vtx *verts_cursor;
  Gfx *cmds_cursor;
  int old_index;

  /* The slot may have held a different column until now, so its translation
     is rewritten here rather than prebaked once for a fixed world.  Doing it
     alongside the mesh keeps the two from ever describing different places.
     Origin-relative: the absolute coordinate no longer fits an s15.16 Mtx. */
  guTranslate(&c_models[slot],
    (float) (cx * CHUNK_SIZE - render_origin_x) * BLOCK_SIZE, 0,
    (float) (cz * CHUNK_SIZE - render_origin_z) * BLOCK_SIZE);

  if (lod == MESH_LOD_SHELL) {
    u8 bx, bz;
    for (bx = 0; bx < CHUNK_SIZE; bx++) {
      for (bz = 0; bz < CHUNK_SIZE; bz++) {
        int world_x = cx * CHUNK_SIZE + bx;
        int world_z = cz * CHUNK_SIZE + bz;
        int scan_y;

        surface_heights[bx][bz] = 0;
        for (scan_y = MAX_Y - 1; scan_y >= 0; scan_y--) {
          u8 block = blockGet(world_x, scan_y, world_z);
          /* Trees and anything the player stacked sit above the terrain, so
             they must not be mistaken for the ground the shell follows. */
          if (block == AIR || block == WATER || block == WOOD ||
              block == LEAVES || block == PLANKS ||
              block == CRAFTING_TABLE || block == COBBLESTONE ||
              block == MOSSY_COBBLESTONE || block == BRICKS) {
            continue;
          }
          surface_heights[bx][bz] = (u8) scan_y;
          break;
        }
      }
    }
  }
  /* Seam refinement repairs hairline cracks the near eye can see; at shell
     distance they are sub-pixel, and skipping it removes the dominant cost
     of compiling a freshly streamed column.  Set per column here so every
     path -- streaming, edits, compaction, world builds -- gets the same
     policy. */
  geometrySetTjunctionRefinement(lod == MESH_LOD_FULL);
  makeColumnGeometry(cx, cz);
  resolveColumnQuads(cx, cz, lod);

  for (texture = 0; texture < NUM_TEXTURES; texture++) {
    total_cmds += textureSegmentSize(column_baked_counts[texture]);
  }
  /* Every LOD bakes its vertices now; a Vtx is two Gfx of arena space. */
  total_verts = (u32) column_baked_total * 8;

  /*
   * Allocate the replacement before releasing the old block: on failure the
   * column keeps its previous mesh and the dirty mark keeps the need alive.
   * The old block is remembered by its start so the right one is freed when
   * two blocks briefly share this slot and generation.
   */
  old_index = meshBlockFind((u16) slot, generation);
  if (old_index >= 0) {
    had_old = TRUE;
    old_start = mesh_blocks[old_index].start;
  }
  if (total_cmds == 0) {
    /* An all-air column's mesh is legitimately empty. */
    block_start = -1;
  } else {
    block_start = meshBlockAlloc(total_verts + total_cmds, total_verts,
      (u16) slot, generation);
    if (block_start < 0) {
      return FALSE;
    }
  }
  if (had_old) {
    u16 i;

    for (i = 0; i < mesh_block_count; i++) {
      if (mesh_blocks[i].slot == slot &&
          mesh_blocks[i].generation == generation &&
          mesh_blocks[i].start == old_start) {
        meshBlockRemove(i);
        break;
      }
    }
  }

  if (total_cmds == 0) {
    for (texture = 0; texture < NUM_TEXTURES; texture++) {
      starts[texture][slot] = empty_column_display_list;
    }
  } else {
    verts_cursor = (Vtx *) (mesh_arena + block_start);
    cmds_cursor = mesh_arena + block_start + total_verts;
    for (texture = 0; texture < NUM_TEXTURES; texture++) {
      starts[texture][slot] =
        emitColumnTextureDL(slot, texture, &verts_cursor, &cmds_cursor);
    }
  }
  meshed[slot] = TRUE;
  lods[slot] = lod;
  return TRUE;
}

/* Drop every block belonging to a generation -- an abandoned build's
   leftovers, or the outgoing world at publish. */
static void meshFreeGeneration(u8 generation) {
  u16 i = 0;

  while (i < mesh_block_count) {
    if (mesh_blocks[i].generation == generation) {
      meshBlockRemove(i);
    } else {
      i++;
    }
  }
}

/*
 * A world build emits the incoming world's columns as a new block
 * generation behind the staged pointer set, while the outgoing world keeps
 * rendering untouched from the live set -- both worlds share the one arena,
 * which two worlds' meshes fit comfortably at these LODs.  Publication copies
 * staged over live and frees the old generation.
 *
 * There is one kind of world build now.  It used to take a flag choosing
 * between a cheap scenic shell for the menu and the real thing for play,
 * which meant every world was compiled twice -- once to look at and once to
 * walk around in -- and the version you looked at was not the version you
 * were about to get.  What this builds is the gameplay mesh, so pressing
 * START is a screen change.
 *
 * This may only run while no graphics task is in flight; callbackGfx
 * guarantees that by stepping it only when NuSystem reports pendingGfx == 0.
 * Do not wait for the RSP here: nuGfxTaskAllEndWait() busy-spins at
 * priority 50 on a counter cleared at priority 17, which deadlocks.
 */
void beginWorldMeshBuild(void) {
  u16 slot;
  u8 texture;

  mesh_alloc_cooldown = 0;
  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    dirty_columns[slot] = FALSE;
    staged_meshed[slot] = FALSE;
  }
  for (texture = 0; texture < NUM_TEXTURES; texture++) {
    for (slot = 0; slot < WINDOW_SLOTS; slot++) {
      staged_starts[texture][slot] = empty_column_display_list;
    }
  }
  /* A build replacing an unfinished build inherits its abandoned blocks;
     free them before claiming the next generation number. */
  if (mesh_building_generation != mesh_generation) {
    meshFreeGeneration(mesh_building_generation);
  }
  mesh_building_generation = (u8) (mesh_generation + 1);
  mesh_build_cursor = 0;
  mesh_build_complete = TRUE;
  mesh_build_active = TRUE;
}

void cancelWorldMeshBuild(void) {
  if (!mesh_build_active) {
    return;
  }
  /* Only the staged generation goes back; column_starts still points at the
     world the RSP has been drawing all along, so the frame this is called
     from renders exactly what the previous one did. */
  meshFreeGeneration(mesh_building_generation);
  mesh_building_generation = mesh_generation;
  mesh_build_active = FALSE;
}

u8 worldMeshBuildProgress() {
  if (!mesh_build_active) {
    return 100;
  }
  return (u8) ((mesh_build_cursor * 100) / (CHUNKS_X * CHUNKS_Z));
}

u8 worldMeshBuildComplete() {
  return mesh_build_complete;
}

/* Returns TRUE once the staged world has been published. */
u8 stepWorldMeshBuild(u16 columns) {
  if (!mesh_build_active) {
    return TRUE;
  }
  /* Every column of the extent is compiled here, so its border columns have
     no streamed neighbours to hide their outward faces behind. */
  building_whole_world = TRUE;
  while (columns > 0 && mesh_build_cursor < CHUNKS_X * CHUNKS_Z) {
    u8 cx = mesh_build_cursor / CHUNKS_Z;
    u8 cz = mesh_build_cursor % CHUNKS_Z;

    if (!makeColumnDisplayLists(cx, cz)) {
      mesh_build_complete = FALSE;
      mesh_build_cursor = CHUNKS_X * CHUNKS_Z;
      break;
    }
    mesh_build_cursor++;
    columns--;
  }
  building_whole_world = FALSE;

  if (mesh_build_cursor >= CHUNKS_X * CHUNKS_Z) {
    u8 texture;
    u16 slot;
    u8 old_generation = mesh_generation;

    /* Publish: the staged set becomes the world on screen, and the old
       generation's blocks go back to the allocator.  Runs with no task in
       flight, so the swap is invisible. */
    for (texture = 0; texture < NUM_TEXTURES; texture++) {
      for (slot = 0; slot < WINDOW_SLOTS; slot++) {
        column_starts[texture][slot] = staged_starts[texture][slot];
      }
    }
    for (slot = 0; slot < WINDOW_SLOTS; slot++) {
      column_meshed[slot] = staged_meshed[slot];
      column_mesh_lod[slot] = staged_mesh_lod[slot];
    }
    mesh_generation = mesh_building_generation;
    meshFreeGeneration(old_generation);
    mesh_build_active = FALSE;
    return TRUE;
  }
  return FALSE;
}

void graphicsSetRenderOrigin(int block_x, int block_z) {
  u32 slot;

  render_origin_x = block_x;
  render_origin_z = block_z;
  render_origin_units_x = (float) block_x * BLOCK_SIZE;
  render_origin_units_z = (float) block_z * BLOCK_SIZE;

  /*
   * Every resident column's chunk matrices were written against the old
   * origin, and they persist across frames -- unlike the camera and entity
   * matrices, which are rewritten every frame and pick the new origin up on
   * their own.  Rewrite them all here, in one pass, so no frame is ever built
   * from a mixture.  256 slots of guTranslate is a bounded, occasional cost:
   * the origin moves at most once per 256 blocks walked.
   */
  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    int cx, cz;
    int off_x, off_z;

    if (!windowSlotResident(slot)) {
      continue;
    }
    cx = windowSlotChunkX(slot);
    cz = windowSlotChunkZ(slot);
    off_x = cx * CHUNK_SIZE - render_origin_x;
    off_z = cz * CHUNK_SIZE - render_origin_z;
    /*
     * Run 6 died exactly here: guTranslate's trunc.w.s of an offset past
     * +-512 blocks overflows s15.16 and raises the VR4300's unimplemented-
     * operation exception -- and the only way a *resident* slot gets such an
     * offset is a corrupted key (the audit in world.c hunts the writer).  A
     * skipped slot draws nothing for a frame and is repaired by the audit;
     * a converted one kills the console.
     */
    if (off_x > 500 || off_x < -500 || off_z > 500 || off_z < -500) {
      window_key_faults++;
      continue;
    }
    guTranslate(&c_models[slot], (float) off_x * BLOCK_SIZE, 0,
      (float) off_z * BLOCK_SIZE);
  }
}

void graphicsInvalidateColumnSlot(u32 slot) {
  u8 texture;

  /*
   * The arena still holds a mesh describing the column that just left, and
   * c_models[slot] still translates to where it used to be.  Point the slot
   * at the shared empty list and give the block back to the allocator --
   * nothing draws there until the incoming column has been compiled,
   * otherwise the old geometry renders at the old place, which reads as
   * terrain smeared across the world.
   */
  for (texture = 0; texture < NUM_TEXTURES; texture++) {
    column_starts[texture][slot] = empty_column_display_list;
  }
  column_meshed[slot] = FALSE;
  meshBlockFree((u16) slot, mesh_generation);
  /* Any pending rebuild referred to the departed column. */
  dirty_columns[slot] = FALSE;
}

static void markColumnDirty(int cx, int cz, u8 class) {
  /* Only a resident column has anything to rebuild; an edit that reaches past
     the window has no mesh to invalidate. */
  if (windowColumnResident(cx, cz)) {
    u32 slot = WINDOW_SLOT(cx, cz);

    if (dirty_columns[slot] < class) {
      dirty_columns[slot] = class;
    }
  }
}

void graphicsMarkColumnDirty(int cx, int cz) {
  markColumnDirty(cx, cz, DIRTY_STREAM);
}

void makeDisplayListsAt(int x, int z) {
  int cx = x >> CHUNK_SHIFT;
  int cz = z >> CHUNK_SHIFT;

  /* Block edits are cheap to mark now.  A later graphics callback bakes a
     bounded amount of geometry, so mining and tree felling cannot monopolize
     a gameplay frame. */
  markColumnDirty(cx, cz, DIRTY_EDIT);
  /* Mask, not modulo: a negative coordinate's remainder is negative, so the
     seam with the column to the west would be missed.  The cx > 0 guards go
     with it -- there is no column zero to stop at any more. */
  if ((x & CHUNK_MASK) == 0) {
    markColumnDirty(cx - 1, cz, DIRTY_EDIT);
  }
  if ((z & CHUNK_MASK) == 0) {
    markColumnDirty(cx, cz - 1, DIRTY_EDIT);
  }
}

/*
 * Nearest-player-first, so meshing radiates outward from the player.  The
 * round-robin this replaces picked dirty slots in window order, which is
 * spatially arbitrary -- harmless when the ring was small, but with
 * hundreds of columns to fill the player literally watched terrain paint
 * in from the fog toward them instead of from their feet out.
 */
static u8 takeDirtyColumn(int *cx, int *cz, u8 *class) {
  u16 slot;
  u16 best_slot = 0;
  int best_distance = 0;
  u8 best_class = DIRTY_NONE;

  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    int distance;

    if (dirty_columns[slot] == DIRTY_NONE) {
      continue;
    }
    /* The window can rebind a slot between an edit and this rebuild, which
       leaves a mark referring to a column that is no longer there. */
    if (!windowSlotResident(slot)) {
      dirty_columns[slot] = DIRTY_NONE;
      continue;
    }
    distance = columnPlayerDistance(windowSlotChunkX(slot),
      windowSlotChunkZ(slot));
    /* An edit outranks every stream mark whatever their distances: its
       changed faces are already on screen.  Within a class, nearest first. */
    if (dirty_columns[slot] > best_class ||
        (dirty_columns[slot] == best_class && distance < best_distance)) {
      best_class = dirty_columns[slot];
      best_distance = distance;
      best_slot = slot;
    }
  }
  if (best_class == DIRTY_NONE) {
    return FALSE;
  }
  dirty_columns[best_slot] = DIRTY_NONE;
  *cx = windowSlotChunkX(best_slot);
  *cz = windowSlotChunkZ(best_slot);
  *class = best_class;
  return TRUE;
}

/* TRUE only for a resident column with no geometry at all -- the state that
   reads as a hole in the world.  A pending LOD upgrade deliberately does not
   count: the urgency deadline boost keys off this, and treating routine
   promotions as emergencies made every chunk crossing hitch. */
u8 graphicsColumnMissingMesh(int cx, int cz) {
  return windowColumnResident(cx, cz) &&
    !column_meshed[WINDOW_SLOT(cx, cz)];
}

u8 graphicsColumnNeedsMesh(int cx, int cz) {
  u32 slot;
  u8 lod;
  int distance;

  if (!windowColumnResident(cx, cz)) {
    return FALSE;
  }
  slot = WINDOW_SLOT(cx, cz);
  if (!column_meshed[slot]) {
    return TRUE;
  }
  /* A meshed column is still stale when the player has crossed its LOD
     boundary: a shell they walked up to, or a full mesh they left behind.
     The promote/demote gap is the hysteresis that keeps a boundary column
     from re-meshing on every step. */
  lod = column_mesh_lod[slot];
  distance = columnPlayerDistance(cx, cz);
  if (lod == MESH_LOD_SHELL && distance <= mesh_lod_promote_radius &&
      worldColumnDeep(cx, cz)) {
    return TRUE;
  }
  if (lod == MESH_LOD_FULL && distance >= mesh_lod_demote_radius) {
    return TRUE;
  }
  return FALSE;
}

static void processColumnDisplayListUpdates(int can_reclaim_mesh_arena) {
  u8 budget;
  u8 overrun_used;

  if (!can_reclaim_mesh_arena) {
    return;
  }
  /* One block per callback slides into the lowest gap; a fragmented arena
     heals over a few dozen frames with no pass, no second arena, and no
     stop-the-world. */
  meshDefragStep();

  if (mesh_alloc_cooldown > 0) {
    mesh_alloc_cooldown--;
    return;
  }

  overrun_used = FALSE;
  for (budget = 0; budget < MESH_REBUILD_BUDGET; budget++) {
    int cx, cz;
    u8 class;

    if (!takeDirtyColumn(&cx, &cz, &class)) {
      break;
    }
    if (streamWorkExpired()) {
      /*
       * Past the deadline, at most one rebuild still runs, and only for the
       * two callers that cannot wait a frame: a player edit, whose changed
       * faces are already on screen, and the mesh stage's turn in the
       * streaming rotation, which keeps stream rebuilds from starving when
       * every frame's deadline is oversubscribed.  A full greedy mesh is
       * the single largest unit the callback can pay, so this admission is
       * most of what the rotation saves.
       */
      if (overrun_used || (class != DIRTY_EDIT &&
          !worldStreamStageGuaranteed(STREAM_STAGE_MESH))) {
        markColumnDirty(cx, cz, class);
        break;
      }
      overrun_used = TRUE;
    }
    if (!makeColumnDisplayLists(cx, cz)) {
      /* Allocation failed: the column keeps its old mesh and its mark.
         Back off so the retry does not pay a full greedy mesh every frame
         while eviction and defrag make room. */
      markColumnDirty(cx, cz, class);
      mesh_alloc_cooldown = MESH_ALLOC_COOLDOWN_FRAMES;
      break;
    }
  }
}

/* class_mask selects which fog classes draw: COLUMN_VISIBLE_NEAR,
   COLUMN_VISIBLE_FAR, or their OR for a single unfogged pass. */
void drawTextured(u8 texture, u8 player_num, u8 class_mask) {
  u16 slot;

  /*
   * Walks slots now rather than a world extent, because with the window
   * scrolling there is no longer a fixed range of columns to iterate -- and
   * visible_columns, which culling already filled in per slot, is the only
   * authority on what is on screen.
   *
   * This does change the order terrain is emitted in, which is not purely
   * cosmetic: water is alpha blended, and the budget below sheds whatever has
   * not been emitted yet, so order decides what gets dropped.  Slot order is
   * still spatially coherent -- the high bits are x -- so it degrades the same
   * way, just about a different axis.
   */
  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    if (visible_columns[player_num][slot] & class_mask) {
      Gfx *start = column_starts[texture][slot];

      /* Most columns use only a few of the sixteen banks, so most of these
         branches would jump straight to the shared empty list.  Each one
         still costs the RSP a command fetch and a return DMA, and across
         ~120 visible columns they were the majority of the terrain pass's
         commands.  The pointer compare answers it for free. */
      if (start == empty_column_display_list) {
        continue;
      }
      /* Dropping the most distant terrain is survivable; overrunning the
         frame buffer is not. */
      if (dlp >= frame_dlp_limit) {
        frame_overflows++;
        return;
      }
      gSPDisplayList(dlp++, start);
    }
  }
}

static void setWaterTopVertex(Vtx *vertex, s16 x, s16 z, s16 s, s16 t) {
  vertex->v.ob[0] = x;
  vertex->v.ob[1] = WATER_SURFACE_HEIGHT;
  vertex->v.ob[2] = z;
  vertex->v.flag = 0;
  vertex->v.tc[0] = s;
  vertex->v.tc[1] = t;
  /*
   * An upward normal, not white vertex colour.  These quads used to be drawn
   * with lighting switched off so their atlas blue survived the render-state
   * changes, which meant the sea stayed noon-bright on a shoreline that had
   * gone black.  Water is a horizontal surface like any block top; giving it
   * the normal the shared quad table already gives every other top face lets
   * the one directional light handle it, and it darkens and reddens with the
   * sand next to it for free.
   */
  vertex->v.cn[0] = 0;
  vertex->v.cn[1] = 127;
  vertex->v.cn[2] = 0;
  vertex->v.cn[3] = 255;
}

static void setStevePartTransform(u8 player_num, u8 part, Vector3 local_offset,
    float pitch, float yaw) {
  Player *player = &players[player_num];
  Vector3 offset = rotateY(local_offset, -player->body_yaw);

  guTranslate(&steve_translate[dl_no][player_num][part],
    player->position.x + offset.x - render_origin_units_x,
    player->position.y + offset.y,
    player->position.z + offset.z - render_origin_units_z);
  guRotateRPY(&steve_rotate[dl_no][player_num][part], pitch, yaw, 0);
}

static u8 playerHeldItem(Player *player) {
  ItemStack *held = &player->inventory[INVENTORY_HOTBAR_START +
    player->selected_hotbar_slot];
  return held->count > 0 ? held->item : AIR;
}

static float punchSwingAngle(Player *player) {
  float phase;

  if (player->attack_time <= 0) {
    return 0;
  }
  phase = 1.f - player->attack_time / PLAYER_ATTACK_DURATION;
  /* Start raised, sweep through the target at mid-animation, then settle. */
  return -58.f + sinf(phase * 180.f * M_DTOR) * 135.f;
}

static float miningSwingAngle(Player *player) {
  if (!player->breaking || player->break_time <= 0) {
    return 0;
  }
  return sinf(player->break_progress * 32.f * M_DTOR) * 28.f;
}

static void makeStevePose(u8 player_num) {
  Player *player = &players[player_num];
  float head_pitch = player->pitch > 180 ? player->pitch - 360 : player->pitch;
  float swing = sinf(player->walk_time) * 28 * player->walk_swing;
  float right_arm_pitch = -swing + punchSwingAngle(player) +
    miningSwingAngle(player);
  float left_arm_pitch = swing;
  float left_leg_pitch = -swing;
  float right_leg_pitch = swing;
  float hurt_bob = player->hurt_time > 0 ?
    sinf((PLAYER_ATTACK_DURATION - player->hurt_time) * 180.f * M_DTOR) * 7.f : 0;

  if (player->vault_time > 0) {
    float phase = 1.f - player->vault_time / PLAYER_VAULT_DURATION;
    float tuck = sinf(phase * 180.f * M_DTOR);
    left_arm_pitch = -55.f * tuck;
    right_arm_pitch = -65.f * tuck + punchSwingAngle(player) +
      miningSwingAngle(player);
    left_leg_pitch = 48.f * tuck;
    right_leg_pitch = -35.f * tuck;
  }

  /* Gameplay yaw rotates direction vectors clockwise around Y, whereas
     guRotateRPY's Y angle rotates model geometry counter-clockwise.  A model
     transform therefore uses the stored yaw directly; only the camera view
     transform negates it. */
  setStevePartTransform(player_num, STEVE_BODY, (Vector3) {hurt_bob, -30, 0},
    0, player->body_yaw);
  /* Unlike the torso, this orientation uses the current camera yaw/pitch. */
  setStevePartTransform(player_num, STEVE_HEAD, (Vector3) {hurt_bob, 8, 0},
    head_pitch, player->yaw);
  setStevePartTransform(player_num, STEVE_LEFT_ARM, (Vector3) {-25 + hurt_bob, -8, 0},
    left_arm_pitch, player->body_yaw);
  setStevePartTransform(player_num, STEVE_RIGHT_ARM, (Vector3) {25 + hurt_bob, -8, 0},
    right_arm_pitch, player->body_yaw);
  /* The sword's origin is the right hand, and it shares the arm's pitch so
     walking and attacks naturally carry it through the same arc. */
  setStevePartTransform(player_num, STEVE_SWORD, (Vector3) {25 + hurt_bob, -52, 0},
    right_arm_pitch, player->body_yaw);
  setStevePartTransform(player_num, STEVE_LEFT_LEG, (Vector3) {-10 + hurt_bob, -52, 0},
    left_leg_pitch, player->body_yaw);
  setStevePartTransform(player_num, STEVE_RIGHT_LEG, (Vector3) {10 + hurt_bob, -52, 0},
    right_leg_pitch, player->body_yaw);
}

static Vtx *toolHeadVertices(u8 item) {
  if (item == WOOD_PICKAXE) return wood_pick_head_verts;
  if (item == STONE_PICKAXE) return stone_pick_head_verts;
  if (item == IRON_PICKAXE) return iron_pick_head_verts;
  if (item == WOOD_AXE) return wood_axe_head_verts;
  if (item == STONE_AXE) return stone_axe_head_verts;
  if (item == IRON_AXE) return iron_axe_head_verts;
  return NULL;
}

static void drawToolGeometry(u8 item) {
  Vtx *head;
  Vtx *guard;

  if (itemIsSword(item)) {
    gSPVertex(dlp++, item == IRON_SWORD ? iron_sword_blade_verts :
      (item == STONE_SWORD ? stone_sword_blade_verts :
      wood_sword_blade_verts), 10, 0);
    gSPDisplayList(dlp++, sword_blade_display_list);
    guard = item == IRON_SWORD ? iron_sword_guard_verts :
      (item == STONE_SWORD ? stone_sword_guard_verts :
      wood_sword_guard_verts);
    gSPVertex(dlp++, guard, 8, 0);
    gSPDisplayList(dlp++, steve_box_display_list);
    return;
  }
  head = toolHeadVertices(item);
  if (head != NULL) {
    gSPVertex(dlp++, tool_handle_verts, 8, 0);
    gSPDisplayList(dlp++, steve_box_display_list);
    gSPVertex(dlp++, head, 8, 0);
    gSPDisplayList(dlp++, steve_box_display_list);
  }
}

static void drawStevePart(u8 player_num, u8 part, Vtx *verts, Gfx *part_dl) {
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&steve_translate[dl_no][player_num][part]),
    G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&steve_rotate[dl_no][player_num][part]),
    G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
  gSPVertex(dlp++, verts, 8, 0);
  gSPDisplayList(dlp++, part_dl);
}

static void drawSteve(u8 player_num) {
  makeStevePose(player_num);

  drawStevePart(player_num, STEVE_BODY, steve_body_verts, steve_box_display_list);
  drawStevePart(player_num, STEVE_LEFT_ARM, steve_arm_verts, steve_box_display_list);
  drawStevePart(player_num, STEVE_RIGHT_ARM, steve_arm_verts, steve_box_display_list);
  if (itemIsTool(playerHeldItem(&players[player_num]))) {
    u8 item = playerHeldItem(&players[player_num]);
    gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(
      &steve_translate[dl_no][player_num][STEVE_SWORD]),
      G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(
      &steve_rotate[dl_no][player_num][STEVE_SWORD]),
      G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
    drawToolGeometry(item);
  }
  drawStevePart(player_num, STEVE_LEFT_LEG, steve_leg_verts, steve_box_display_list);
  drawStevePart(player_num, STEVE_RIGHT_LEG, steve_leg_verts, steve_box_display_list);
  drawStevePart(player_num, STEVE_HEAD, steve_head_verts, steve_box_display_list);

  /* The eyes share the head transform, so their direction matches its pitch
     and yaw exactly. */
  drawStevePart(player_num, STEVE_HEAD, steve_eye_verts, steve_eyes_display_list);
}

/* The first-person arm pivots at the elbow, not at the hand.  Local +Y of
   first_person_arm_verts runs from the hand at the origin back to the elbow, so
   a pitch past 90 degrees lays the forearm back toward the camera and leaves
   the tool -- which grows from that same origin the other way -- pointing
   forward past the crosshair.  Pivoting at the hand instead pinned the one
   point a strike should move: the only part left free was the forearm's far
   end, which swung back across the camera, and the attack read as the player
   hitting their own face. */
#define FP_ARM_LENGTH 48.f
#define FP_ELBOW_X 44.f
#define FP_ELBOW_Y -54.f
#define FP_ELBOW_Z -42.f
#define FP_REST_PITCH 120.f
#define FP_ARM_ROLL 25.f
/* A forearm pinned at a fixed elbow can only sweep its hand perpendicular to
   itself, so rotation alone can never carry a strike out toward what the player
   is aiming at.  Driving the elbow forward through the swing does: perspective
   pulls anything travelling down -Z in toward the centre of the screen. */
#define FP_SWING_PITCH 38.f
#define FP_SWING_FORWARD 45.f
#define FP_SWING_INWARD 6.f
#define FP_SWING_RISE 4.f

/* 0 while the arm rests, 1 at the furthest reach of a strike. */
static float firstPersonReach(Player *player) {
  float reach = 0;

  if (player->attack_time > 0) {
    reach = sinf((1.f - player->attack_time / PLAYER_ATTACK_DURATION) *
      180.f * M_DTOR);
  }
  if (player->breaking && player->break_time > 0) {
    /* Mining is the same strike, shallower and repeating. */
    reach = max(reach, (sinf(player->break_progress * 32.f * M_DTOR) * .5f +
      .5f) * .6f);
  }
  return reach;
}

static void drawFirstPersonHand(u8 player_num) {
  Player *player = &players[player_num];
  u8 item = playerHeldItem(player);
  float reach, pitch, pitch_sin, pitch_cos, roll_sin, roll_cos;

  if (player->camera_mode != CAMERA_FIRST_PERSON) {
    return;
  }
  reach = firstPersonReach(player);
  pitch = FP_REST_PITCH + FP_SWING_PITCH * reach;
  pitch_sin = sinf(pitch * M_DTOR);
  pitch_cos = cosf(pitch * M_DTOR);
  roll_sin = sinf(FP_ARM_ROLL * M_DTOR);
  roll_cos = cosf(FP_ARM_ROLL * M_DTOR);
  /* Draw in camera space so looking around cannot rotate or displace the
     first-person model. Only the deliberate attack/mining swing changes it. */
  loadCameraProjection();
  /* The rotation turns the model about its own origin -- the hand -- so for the
     elbow to be what stays put the translation has to undo where the rotation
     sends it: translate = elbow - (0, FP_ARM_LENGTH, 0) * rotation. */
  guTranslate(&first_person_sword_translate[dl_no][player_num],
    FP_ELBOW_X - FP_SWING_INWARD * reach + FP_ARM_LENGTH * pitch_cos * roll_sin,
    FP_ELBOW_Y + FP_SWING_RISE * reach - FP_ARM_LENGTH * pitch_cos * roll_cos,
    FP_ELBOW_Z - FP_SWING_FORWARD * reach - FP_ARM_LENGTH * pitch_sin);
  guRotateRPY(&first_person_sword_rotate[dl_no][player_num], pitch, 0.f,
    FP_ARM_ROLL);
  gSPClearGeometryMode(dlp++, G_CULL_BACK);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&first_person_sword_translate[dl_no][player_num]),
    G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&first_person_sword_rotate[dl_no][player_num]),
    G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
  gSPVertex(dlp++, first_person_arm_verts, 8, 0);
  gSPDisplayList(dlp++, steve_box_display_list);
  if (itemIsTool(item)) {
    drawToolGeometry(item);
  }
  gSPSetGeometryMode(dlp++, G_CULL_BACK);
}

static u8 pointVisibleToPlayer(u8 viewer_num, Vector3 point,
    float max_distance) {
  Vector3 offset = add(point, mul(players[viewer_num].position, -1.f));
  /* floor, not truncation: west and north of the origin are negative now. */
  int cx = floor(point.x / (BLOCK_SIZE * CHUNK_SIZE));
  int cz = floor(point.z / (BLOCK_SIZE * CHUNK_SIZE));

  if (dot(offset, offset) > max_distance * max_distance) {
    return FALSE;
  }
  return windowColumnResident(cx, cz) &&
    visible_columns[viewer_num][WINDOW_SLOT(cx, cz)];
}

static void drawDetailBox(Vtx *vertices) {
  gSPVertex(dlp++, vertices, 8, 0);
  gSPDisplayList(dlp++, steve_box_display_list);
}

static void drawDetailsForPlayer(u8 viewer_num) {
  u16 selected[MAX_VISIBLE_DETAILS];
  float selected_distance[MAX_VISIBLE_DETAILS];
  u8 limit = usesFourPlayerLayout() ? 6 :
    (active_player_count > 1 ? 12 : MAX_VISIBLE_DETAILS);
  u8 count = 0;
  u16 index;
  u8 render_slot;
  u8 torch_slots[MAX_VISIBLE_DETAILS];
  u8 torch_count = 0;

  /* No records means no pass and no state changes to undo afterwards. */
  if (detail_count == 0) {
    return;
  }

  /* Maintain a tiny nearest set in one pass.  Detail count is globally
     bounded, while the selected list keeps matrix and command cost bounded
     independently for solo, split-screen, and four-player views. */
  for (index = 0; index < detail_scan_limit; index++) {
    DetailCell *detail = &details[index];
    Vector3 position;
    Vector3 offset;
    float distance;
    u8 farthest;
    u8 slot;

    if (!detail->active) {
      continue;
    }
    position = (Vector3) {(detail->x + .5f) * BLOCK_SIZE,
      detail->y * BLOCK_SIZE, (detail->z + .5f) * BLOCK_SIZE};
    if (!pointVisibleToPlayer(viewer_num, position,
        DETAIL_RENDER_DISTANCE)) {
      continue;
    }
    offset = add(position, mul(players[viewer_num].position, -1.f));
    distance = dot(offset, offset);
    if (count < limit) {
      selected[count] = index;
      selected_distance[count] = distance;
      count++;
      continue;
    }
    farthest = 0;
    for (slot = 1; slot < count; slot++) {
      if (selected_distance[slot] > selected_distance[farthest]) {
        farthest = slot;
      }
    }
    if (distance < selected_distance[farthest]) {
      selected[farthest] = index;
      selected_distance[farthest] = distance;
    }
  }

  gSPTexture(dlp++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
  setEntityShadeCombine();
  gSPClearGeometryMode(dlp++, G_CULL_BACK);
  for (render_slot = 0; render_slot < count; render_slot++) {
    DetailCell *detail = &details[selected[render_slot]];
    float yaw = detail->orientation * 90.f;

    if (detail->kind == DETAIL_WOOD_DOOR &&
        (detail->state & DETAIL_STATE_OPEN)) {
      yaw += 90.f;
    }
    guTranslate(&detail_translate[dl_no][render_slot],
      (detail->x + .5f) * BLOCK_SIZE - render_origin_units_x,
      detail->y * BLOCK_SIZE,
      (detail->z + .5f) * BLOCK_SIZE - render_origin_units_z);
    guRotateRPY(&detail_rotate[dl_no][render_slot], 0, yaw, 0);
    gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(
      &detail_translate[dl_no][render_slot]),
      G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(
      &detail_rotate[dl_no][render_slot]),
      G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);

    if (detail->kind == DETAIL_TORCH) {
      /* Stick only.  Its flame is emissive and goes in the second pass
         below, at a primitive colour the nightfall tint does not reach. */
      drawDetailBox(torch_stick_verts);
      torch_slots[torch_count++] = render_slot;
    } else if (detail->kind == DETAIL_WOOD_STAIRS) {
      drawDetailBox(wood_stair_lower_verts);
      drawDetailBox(wood_stair_upper_verts);
    } else if (detail->kind == DETAIL_STONE_STAIRS) {
      drawDetailBox(stone_stair_lower_verts);
      drawDetailBox(stone_stair_upper_verts);
    } else if (detail->kind == DETAIL_WOOD_DOOR) {
      drawDetailBox(door_verts);
    } else if (detail->kind == DETAIL_WINDOW) {
      drawDetailBox(window_left_verts);
      drawDetailBox(window_right_verts);
      drawDetailBox(window_top_verts);
      drawDetailBox(window_bottom_verts);
      drawDetailBox(window_cross_verts);
    }
  }

  /*
   * Flames last, in one batch, so the primitive colour changes twice a frame
   * rather than twice a torch.  Their matrices are the ones the loop above
   * already wrote for the same render slot, which is why this re-references
   * detail_translate rather than rebuilding anything.
   */
  if (torch_count > 0) {
    u8 i;

    gDPPipeSync(dlp++);
    setEntityTint(emissive_tint);
    for (i = 0; i < torch_count; i++) {
      gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&detail_translate[dl_no][torch_slots[i]]),
        G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
      gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&detail_rotate[dl_no][torch_slots[i]]),
        G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
      drawDetailBox(torch_flame_verts);
    }
    gDPPipeSync(dlp++);
    setEntityTint(entity_tint);
  }
  gSPSetGeometryMode(dlp++, G_CULL_BACK);
  loaded_texture = NULL;
}

static void setMobPartTransform(u8 mob_num, u8 part,
    Vector3 local_offset) {
  Mob *mob = &mobs[mob_num];
  Vector3 offset = rotateY(local_offset, -mob->yaw);

  guTranslate(&mob_translate[dl_no][mob_num][part],
    mob->position.x + offset.x - render_origin_units_x,
    mob->position.y + offset.y,
    mob->position.z + offset.z - render_origin_units_z);
  guRotateRPY(&mob_rotate[dl_no][mob_num][part], 0, -mob->yaw, 0);
}

static void drawMobPart(u8 mob_num, u8 part, Vtx *verts) {
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&mob_translate[dl_no][mob_num][part]),
    G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&mob_rotate[dl_no][mob_num][part]),
    G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
  gSPVertex(dlp++, verts, 8, 0);
  gSPDisplayList(dlp++, steve_box_display_list);
}

static void drawMob(u8 mob_num) {
  Mob *mob = &mobs[mob_num];
  Vtx *body = mob->type == MOB_PIG ? pig_body_verts : sheep_body_verts;
  Vtx *head = mob->type == MOB_PIG ? pig_head_verts : sheep_head_verts;
  Vtx *leg = mob->type == MOB_PIG ? pig_leg_verts : sheep_leg_verts;
  float step = sinf(mob->walk_time) * 4.f;
  float hurt = mob->hurt_time > 0 ?
    sinf((PLAYER_ATTACK_DURATION - mob->hurt_time) * 180.f * M_DTOR) * 4.f : 0;
  float graze = mob->state == MOB_IDLE ?
    sinf(mob->walk_time * .35f) * 3.f - 4.f : 0;

  if (mob->type == MOB_SLIME) {
    float bounce = sinf(mob->walk_time);
    if (bounce < 0) bounce = -bounce;
    setMobPartTransform(mob_num, MOB_BODY,
      (Vector3) {hurt, 21 + bounce * 8.f, 0});
    setMobPartTransform(mob_num, MOB_HEAD,
      (Vector3) {-9 + hurt, 27 + bounce * 8.f, -21});
    setMobPartTransform(mob_num, MOB_SNOUT,
      (Vector3) {9 + hurt, 27 + bounce * 8.f, -21});
    drawMobPart(mob_num, MOB_BODY, slime_body_verts);
    drawMobPart(mob_num, MOB_HEAD, slime_eye_verts);
    drawMobPart(mob_num, MOB_SNOUT, slime_eye_verts);
    return;
  }

  setMobPartTransform(mob_num, MOB_BODY, (Vector3) {hurt, 43, 0});
  setMobPartTransform(mob_num, MOB_HEAD,
    (Vector3) {hurt, 43 + graze, -29});
  setMobPartTransform(mob_num, MOB_FRONT_LEFT_LEG,
    (Vector3) {-18 + hurt, 18 + step, -13});
  setMobPartTransform(mob_num, MOB_FRONT_RIGHT_LEG,
    (Vector3) {18 + hurt, 18 - step, -13});
  setMobPartTransform(mob_num, MOB_BACK_LEFT_LEG,
    (Vector3) {-18 + hurt, 18 - step, 13});
  setMobPartTransform(mob_num, MOB_BACK_RIGHT_LEG,
    (Vector3) {18 + hurt, 18 + step, 13});

  drawMobPart(mob_num, MOB_BODY, body);
  drawMobPart(mob_num, MOB_HEAD, head);
  drawMobPart(mob_num, MOB_FRONT_LEFT_LEG, leg);
  drawMobPart(mob_num, MOB_FRONT_RIGHT_LEG, leg);
  drawMobPart(mob_num, MOB_BACK_LEFT_LEG, leg);
  drawMobPart(mob_num, MOB_BACK_RIGHT_LEG, leg);
  if (mob->type == MOB_PIG) {
    setMobPartTransform(mob_num, MOB_SNOUT,
      (Vector3) {hurt, 39 + graze, -46});
    drawMobPart(mob_num, MOB_SNOUT, pig_snout_verts);
  }
}

static void drawMobsForPlayer(u8 viewer_num) {
  u8 drawn[MAX_MOBS] = {FALSE};
  u8 visible;

  gSPTexture(dlp++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
  setEntityShadeCombine();
  gSPClearGeometryMode(dlp++, G_CULL_BACK);

  /* The entity pool keeps passive animals in its first slots and reserves
     later slots for slimes. Choosing by threat and distance prevents an
     off-screen herd from consuming the small per-view render budget while a
     nearby hostile mob disappears. */
  for (visible = 0; visible < MAX_VISIBLE_MOBS; visible++) {
    u8 mob_num;
    u8 best = MAX_MOBS;
    u8 best_priority = 2;
    float best_distance = MOB_RENDER_DISTANCE * MOB_RENDER_DISTANCE + 1.f;

    for (mob_num = 0; mob_num < MAX_MOBS; mob_num++) {
      Vector3 offset;
      float distance;
      u8 priority;

      if (drawn[mob_num] || !mobs[mob_num].active ||
          !pointVisibleToPlayer(viewer_num, mobs[mob_num].position,
            MOB_RENDER_DISTANCE)) {
        continue;
      }
      offset = add(mobs[mob_num].position,
        mul(players[viewer_num].position, -1.f));
      distance = dot(offset, offset);
      priority = (mobs[mob_num].type == MOB_SLIME ||
        mobs[mob_num].state == MOB_CHASE) ? 0 : 1;
      if (priority < best_priority ||
          (priority == best_priority && distance < best_distance)) {
        best = mob_num;
        best_priority = priority;
        best_distance = distance;
      }
    }
    if (best == MAX_MOBS) break;
    drawn[best] = TRUE;
    drawMob(best);
  }
  gSPSetGeometryMode(dlp++, G_CULL_BACK);
}

static void drawOtherPlayers(u8 viewer_num) {
  u8 player_num;

  gSPTexture(dlp++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
  setEntityShadeCombine();
  /* The box model has intentionally minimal geometry; disabling culling
     keeps its face and eye quads reliable from every camera angle. */
  gSPClearGeometryMode(dlp++, G_CULL_BACK);
  for (player_num = 0; player_num < active_player_count; player_num++) {
    if (players[player_num].active &&
        (player_num != viewer_num ||
         (players[viewer_num].camera_mode == CAMERA_THIRD_PERSON &&
          third_person_avatar_visible[viewer_num])) &&
        pointVisibleToPlayer(viewer_num, players[player_num].position,
          PLAYER_RENDER_DISTANCE)) {
      drawSteve(player_num);
    }
  }
  gSPSetGeometryMode(dlp++, G_CULL_BACK);
}

static void drawLooseItemGeometry(u8 item) {
  Vtx *body = NULL;

  if (itemIsTool(item)) {
    drawToolGeometry(item);
    return;
  }
  if (item == STICK) {
    body = tool_handle_verts;
  } else if (item == COAL) {
    body = coal_chunk_verts;
  } else if (item == IRON_CHUNK) {
    body = iron_chunk_verts;
  } else if (item == APPLE) {
    gSPVertex(dlp++, apple_body_verts, 8, 0);
    gSPDisplayList(dlp++, steve_box_display_list);
    body = apple_stem_verts;
  } else if (item == RAW_MUTTON || item == RAW_PORK) {
    body = item == RAW_PORK ? pork_verts : mutton_verts;
  } else if (item == SLIME_GEL) {
    body = slime_gel_verts;
  } else if (item == TORCH) {
    gSPVertex(dlp++, torch_stick_verts, 8, 0);
    gSPDisplayList(dlp++, steve_box_display_list);
    body = torch_flame_verts;
  }
  if (body != NULL) {
    gSPVertex(dlp++, body, 8, 0);
    gSPDisplayList(dlp++, steve_box_display_list);
  }
}

static void drawDroppedItems(u8 viewer_num) {
  u8 i;

  /* Small cubes are particularly sensitive to winding/culling mistakes on
     hardware.  Rendering both sides makes pickups visible from every angle. */
  gSPClearGeometryMode(dlp++, G_CULL_BACK);
  for (i = 0; i < MAX_DROPPED_ITEMS; i++) {
    DroppedItem *drop = &dropped_items[i];
    if (!drop->active) {
      continue;
    }
    if (!pointVisibleToPlayer(viewer_num, drop->position,
        DROPPED_ITEM_RENDER_DISTANCE)) {
      continue;
    }

    guTranslate(&dropped_item_translate[dl_no][i],
      drop->position.x - render_origin_units_x,
      drop->position.y + sinf(drop->rotation * M_DTOR) * 3.f,
      drop->position.z - render_origin_units_z);
    guRotateRPY(&dropped_item_rotate[dl_no][i], 0, drop->rotation, 0);
    gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&dropped_item_translate[dl_no][i]),
      G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&dropped_item_rotate[dl_no][i]),
      G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
    if (ITEM_IS_VALID(drop->item) && preview_textures[drop->item] != NULL) {
      gSPTexture(dlp++, 0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON);
      /* The cube's vertices are all white, so modulating the tile against the
         tint alone is the whole lighting these get -- shade would only
         multiply in a constant 255. */
      gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
      loadTexture(preview_textures[drop->item]);
      gSPVertex(dlp++, dropped_item_verts, 8, 0);
      gSPDisplayList(dlp++, dropped_item_display_list);
    } else {
      gSPTexture(dlp++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
      setEntityShadeCombine();
      drawLooseItemGeometry(drop->item);
      loaded_texture = NULL;
    }
  }
  gSPSetGeometryMode(dlp++, G_CULL_BACK);
}

static void drawFallingTreeBox(u8 slot, u8 box, u8 item, float x, float y,
    float z, float sx, float sy, float sz) {
  const float cube_scale = BLOCK_SIZE / 28.f;

  loadTexture(preview_textures[item]);
  guTranslate(&falling_tree_box_translate[dl_no][slot][box], x, y, z);
  guScale(&falling_tree_box_scale[dl_no][slot][box], sx * cube_scale,
    sy * cube_scale, sz * cube_scale);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&falling_tree_translate[dl_no][slot]),
    G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&falling_tree_rotate[dl_no][slot]),
    G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&falling_tree_box_translate[dl_no][slot][box]),
    G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&falling_tree_box_scale[dl_no][slot][box]),
    G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
  gSPVertex(dlp++, dropped_item_verts, 8, 0);
  gSPDisplayList(dlp++, dropped_item_display_list);
}

/*
 * Tree records store x and z in wrapping u8 (trees.c keys its lookup by a
 * window-relative table, so the wrap is deliberate).  A live tree is always
 * inside the residency window and therefore within 128 blocks of any player,
 * so the wrapped coordinate has exactly one absolute representative near the
 * viewer -- recover it before any world-space math.
 */
static int unwrapTreeCoord(u8 wrapped, float viewer_units) {
  int reference = floor(viewer_units / BLOCK_SIZE);
  int delta = (int) ((u8) (wrapped - (u8) reference));

  if (delta >= 128) {
    delta -= 256;
  }
  return reference + delta;
}

static void drawFallingTree(TreeRecord *tree, u8 slot, int abs_x, int abs_z) {
  float progress = tree->fall_progress;
  float eased = progress * progress * (3.f - 2.f * progress);
  float angle = eased * 88.f + sinf(progress * 720.f * M_DTOR) *
    (1.f - progress) * 3.f;
  float pitch = 0;
  float roll = 0;
  float height = tree->canopy_y - tree->base_y + 1;
  float canopy_y = (tree->canopy_y - tree->base_y) * BLOCK_SIZE;

  if (tree->fall_direction == 0) {
    roll = -angle;
  } else if (tree->fall_direction == 1) {
    pitch = angle;
  } else if (tree->fall_direction == 2) {
    roll = angle;
  } else {
    pitch = -angle;
  }

  guTranslate(&falling_tree_translate[dl_no][slot],
    (abs_x + 0.5f) * BLOCK_SIZE - render_origin_units_x,
    (tree->base_y + 1) * BLOCK_SIZE,
    (abs_z + 0.5f) * BLOCK_SIZE - render_origin_units_z);
  guRotateRPY(&falling_tree_rotate[dl_no][slot], pitch, 0, roll);

  drawFallingTreeBox(slot, 0, WOOD, 0, height * BLOCK_SIZE / 2.f, 0,
    1, height, 1);
  drawFallingTreeBox(slot, 1, LEAVES, 0, canopy_y + BLOCK_SIZE, 0,
    3, 3, 3);
  drawFallingTreeBox(slot, 2, LEAVES, 0, canopy_y + BLOCK_SIZE * 1.5f,
    -BLOCK_SIZE * 1.8f, 3, 1, 1);
  drawFallingTreeBox(slot, 3, LEAVES, 0, canopy_y + BLOCK_SIZE * 1.5f,
    BLOCK_SIZE * 1.8f, 3, 1, 1);
  drawFallingTreeBox(slot, 4, LEAVES, -BLOCK_SIZE * 1.8f,
    canopy_y + BLOCK_SIZE * 1.5f, 0, 1, 1, 3);
  drawFallingTreeBox(slot, 5, LEAVES, BLOCK_SIZE * 1.8f,
    canopy_y + BLOCK_SIZE * 1.5f, 0, 1, 1, 3);
}

/* One pass: the state test comes first so the common frame with nothing
   falling costs a 96-entry byte scan and no coordinate unwrapping, and the
   cull-mode commands are only emitted around trees actually drawn. */
static void drawFallingTrees(u8 viewer_num) {
  u8 tree_index;
  u8 slot = 0;

  for (tree_index = 0; tree_index < MAX_TREES &&
      slot < FALLING_TREE_RENDER_SLOTS; tree_index++) {
    int ax, az;
    Vector3 position;

    if (trees[tree_index].state != TREE_STATE_FALLING) {
      continue;
    }
    ax = unwrapTreeCoord(trees[tree_index].x, players[viewer_num].position.x);
    az = unwrapTreeCoord(trees[tree_index].z, players[viewer_num].position.z);
    position.x = (ax + .5f) * BLOCK_SIZE;
    position.y = (trees[tree_index].base_y + 1) * BLOCK_SIZE;
    position.z = (az + .5f) * BLOCK_SIZE;
    if (!pointVisibleToPlayer(viewer_num, position,
        DROPPED_ITEM_RENDER_DISTANCE)) {
      continue;
    }
    if (slot == 0) {
      gSPClearGeometryMode(dlp++, G_CULL_BACK);
      /* Set explicitly rather than inherited.  This pass used to run on
         whatever drawGroundShadows handed over, which the detail pass in
         between then overwrote with an untextured shade-only combiner -- so a
         tree felled anywhere near a torch or a door fell as a white box. */
      gSPTexture(dlp++, 0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON);
      gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
    }
    drawFallingTree(&trees[tree_index], slot++, ax, az);
  }
  if (slot > 0) {
    gSPSetGeometryMode(dlp++, G_CULL_BACK);
  }
}

/*
 * Cast shadows.
 *
 * A shadow map is not on the table here -- no depth textures, no second
 * pass over the terrain, and a mesh that is compiled once per column and
 * then left alone for as long as the column survives.  What the hardware can
 * afford is the classic: a soft blob per caster, laid on the ground where the
 * light says the caster's shadow falls.  Because the projection is computed
 * from the live light direction rather than dropped straight down, shadows
 * genuinely swing and stretch through the day -- and, on a clear night with
 * the Moon up, a fainter set swings the other way.
 *
 * Cost is a hard ceiling: SHADOW_SLOTS casters, nine vertices and eight
 * triangles each, one identity matrix for the whole pass, and no per-shadow
 * RDP state at all -- strength lives in vertex alpha, so a shadow costs the
 * RSP a transform and the RDP some fill, and nothing else.  The set is built
 * once per frame and drawn in every viewport: the vertices are world space,
 * so split-screen shares them.
 */
#define SHADOW_SLOTS 12
#define SHADOW_VERTS 9
/* A caster this far from every player is not worth a slot. */
#define SHADOW_RENDER_DISTANCE (BLOCK_SIZE * 22.f)
/* Ground search depth, in blocks, below a caster. */
#define SHADOW_GROUND_SCAN 6
/* A low light would otherwise project a shadow to the horizon.  The strength
   curve has already faded most of it out by the time this bites. */
#define SHADOW_MAX_OFFSET (BLOCK_SIZE * 4.f)
/* How far a caster can rise off the ground before its shadow has dissolved
   entirely.  This is airborne height only -- a tall caster is not a faint
   one, it is a wide one. */
#define SHADOW_FADE_HEIGHT (BLOCK_SIZE * 7.f)

static Vtx shadow_verts[NUM_DISPLAY_LISTS][SHADOW_SLOTS][SHADOW_VERTS];
static u8 shadow_count;
/*
 * Shadow vertices are already in origin-relative world space, which is what
 * the camera matrix expects, so the pass needs an identity modelview.  It is
 * written once and never again: nothing may rewrite a matrix an in-flight
 * task can still be reading.
 */
static Mtx shadow_model;

static Gfx shadow_blob_display_list[] = {
  gsSP2Triangles(0, 1, 4, 0, 0, 4, 3, 0),
  gsSP2Triangles(1, 2, 5, 0, 1, 5, 4, 0),
  gsSP2Triangles(3, 4, 7, 0, 3, 7, 6, 0),
  gsSP2Triangles(4, 5, 8, 0, 4, 8, 7, 0),
  gsSPEndDisplayList()
};

/* Surface of the first solid block at or below a caster.  FALSE when there is
   none within the scan: over a cliff edge, or above space that has not
   streamed in, where a blob would hang in the air. */
static u8 shadowGroundAt(float wx, float wy, float wz, float *surface_y) {
  int bx = floor(wx / BLOCK_SIZE);
  int bz = floor(wz / BLOCK_SIZE);
  int top = floor(wy / BLOCK_SIZE);
  int limit = top - SHADOW_GROUND_SCAN;
  int y;

  if (top >= MAX_Y) {
    top = MAX_Y - 1;
  }
  if (limit < 0) {
    limit = 0;
  }
  for (y = top; y >= limit; y--) {
    u8 block = blockGet(bx, y, bz);

    if (block != BLOCK_NOT_RESIDENT && BLOCK_IS_SOLID(block)) {
      *surface_y = (float) (y + 1) * BLOCK_SIZE;
      return TRUE;
    }
  }
  return FALSE;
}

static u8 shadowNearAnyPlayer(float wx, float wz) {
  u8 i;

  for (i = 0; i < active_player_count; i++) {
    float dx = wx - players[i].position.x;
    float dz = wz - players[i].position.z;

    if (dx * dx + dz * dz <= SHADOW_RENDER_DISTANCE * SHADOW_RENDER_DISTANCE) {
      return TRUE;
    }
  }
  return FALSE;
}

/*
 * One caster.  `body_height` is how far the middle of the caster sits above
 * its own footing -- a tree's canopy, a player's chest.  Height above the
 * *ground* is measured here and kept separate: the two do different jobs, and
 * conflating them faded a tree's shadow out precisely because the tree was
 * tall enough to cast a good one.
 */
static void addGroundShadow(Vector3 position, float body_height,
    float radius, float strength) {
  Vector3 light;
  float ground, airborne, height, offset_x, offset_z, spread, reach, alpha;
  Vtx *verts;
  u8 i;

  if (shadow_count >= SHADOW_SLOTS || strength <= .02f) {
    return;
  }
  if (!shadowNearAnyPlayer(position.x, position.z)) {
    return;
  }
  /* A unit below the caster's footing, not above it: starting the search at
     the caster's own base finds the caster -- a tree trunk reads as its own
     ground and lifts the blob a block into the air. */
  if (!shadowGroundAt(position.x, position.y - 1.f, position.z, &ground)) {
    return;
  }

  airborne = position.y - ground;
  if (airborne < 0) {
    airborne = 0;
  }
  /* Height of the caster's middle over the ground it shadows.  Both the
     throw and the penumbra scale with it. */
  height = airborne + body_height;
  spread = 1.f + height / (BLOCK_SIZE * 5.f);
  if (spread > 2.2f) {
    spread = 2.2f;
  }
  /* Only leaving the ground fades a shadow.  Spreading merely softens it, so
     it divides by a gentler curve than the spread itself. */
  alpha = strength * (1.f - min(1.f, airborne / SHADOW_FADE_HEIGHT)) /
    (.55f + .45f * spread);
  if (alpha <= .02f) {
    return;
  }

  /* Opposite the light, displaced by the caster's height over the tangent of
     the light's altitude.  The floor under the vertical term is what keeps a
     grazing light from projecting to infinity. */
  light = dayCycleLightDirection();
  reach = height / max(light.y, .30f);
  offset_x = -light.x * reach;
  offset_z = -light.z * reach;
  if (offset_x > SHADOW_MAX_OFFSET) offset_x = SHADOW_MAX_OFFSET;
  if (offset_x < -SHADOW_MAX_OFFSET) offset_x = -SHADOW_MAX_OFFSET;
  if (offset_z > SHADOW_MAX_OFFSET) offset_z = SHADOW_MAX_OFFSET;
  if (offset_z < -SHADOW_MAX_OFFSET) offset_z = -SHADOW_MAX_OFFSET;

  radius *= spread;
  verts = shadow_verts[dl_no][shadow_count++];
  for (i = 0; i < SHADOW_VERTS; i++) {
    /* A 3x3 grid: opaque in the middle, transparent all the way round the
       rim, which reads as a soft round blob for eight triangles and no
       texture at all. */
    static const float grid[3] = {-1.f, 0.f, 1.f};
    u8 col = i % 3;
    u8 row = i / 3;
    u8 edge = (col == 1) + (row == 1);

    verts[i].v.ob[0] = position.x + offset_x + grid[col] * radius -
      render_origin_units_x;
    verts[i].v.ob[1] = ground;
    verts[i].v.ob[2] = position.z + offset_z + grid[row] * radius -
      render_origin_units_z;
    verts[i].v.flag = 0;
    verts[i].v.tc[0] = 0;
    verts[i].v.tc[1] = 0;
    verts[i].v.cn[0] = 0;
    verts[i].v.cn[1] = 0;
    verts[i].v.cn[2] = 0;
    /* edge == 2 is the centre, 1 the four side midpoints, 0 the corners. */
    verts[i].v.cn[3] = edge == 2 ? (u8) (alpha * 255.f) :
      (edge == 1 ? (u8) (alpha * 140.f) : 0);
  }
}

/*
 * Choose the frame's casters, most valuable first, and stop at the cap.
 * Trees lead deliberately: a tree's shadow is long, it lands on ground the
 * player is walking over, and it is the one that sells a moving sun.
 */
static void buildGroundShadows(void) {
  float strength = dayCycleShadowStrength();
  u16 i;

  shadow_count = 0;
  if (strength <= .02f) {
    return;
  }

  for (i = 0; i < MAX_TREES && shadow_count < SHADOW_SLOTS; i++) {
    TreeRecord *tree = &trees[i];
    Vector3 position;
    float height;

    /* A tree mid-fall is a rigid body swinging through 88 degrees; a blob
       under its stump would contradict what the player is watching. */
    if (tree->base_y == TREE_INACTIVE_Y || tree->state != TREE_STATE_STANDING) {
      continue;
    }
    position.x = (unwrapTreeCoord(tree->x, players[0].position.x) + .5f) *
      BLOCK_SIZE;
    position.z = (unwrapTreeCoord(tree->z, players[0].position.z) + .5f) *
      BLOCK_SIZE;
    position.y = (float) (tree->base_y + 1) * BLOCK_SIZE;
    height = (float) (tree->canopy_y - tree->base_y) * BLOCK_SIZE;
    /* Measured from the canopy, not the trunk: the canopy is what actually
       blocks the light, and it is what the player expects to see on the
       ground. */
    addGroundShadow(position, height, BLOCK_SIZE * 1.35f, strength * .85f);
  }

  for (i = 0; i < active_player_count && shadow_count < SHADOW_SLOTS; i++) {
    addGroundShadow(players[i].position, BLOCK_SIZE * .9f, BLOCK_SIZE * .42f,
      strength);
  }

  for (i = 0; i < MAX_MOBS && shadow_count < SHADOW_SLOTS; i++) {
    if (!mobs[i].active) {
      continue;
    }
    addGroundShadow(mobs[i].position, BLOCK_SIZE * .5f, BLOCK_SIZE * .40f,
      strength);
  }

  for (i = 0; i < MAX_DROPPED_ITEMS && shadow_count < SHADOW_SLOTS; i++) {
    if (!dropped_items[i].active) {
      continue;
    }
    addGroundShadow(dropped_items[i].position, BLOCK_SIZE * .2f,
      BLOCK_SIZE * .18f, strength * .8f);
  }
}

/* Emitted after the terrain has laid down depth and before the entities, in
   every viewport, from the one set built above. */
static void drawGroundShadows(void) {
  u8 i;

  if (shadow_count == 0) {
    return;
  }
  gDPPipeSync(dlp++);
  /* The texture unit is deliberately left as the terrain set it: G_CC_SHADE
     never references TEXEL0, and drawFallingTrees below still expects
     texturing to be enabled. */
  gDPSetCombineMode(dlp++, G_CC_SHADE, G_CC_SHADE);
  /* Decal: the blob is coplanar with the block top it lies on, which is
     exactly the case ZMODE_DEC exists for. */
  gDPSetRenderMode(dlp++, G_RM_ZB_XLU_DECAL, G_RM_ZB_XLU_DECAL2);
  gSPClearGeometryMode(dlp++, G_CULL_BACK | G_LIGHTING);
  /* osVirtualToPhysical, not OS_K0_TO_PHYSICAL: the macro subtracts
     0x80000000 from the pointer, and against a plain static (rather than an
     array element reached by a runtime index) the compiler folds that into a
     constant and reports it as an out-of-bounds subscript. */
  gSPMatrix(dlp++, osVirtualToPhysical(&shadow_model),
    G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);

  for (i = 0; i < shadow_count; i++) {
    gSPVertex(dlp++, shadow_verts[dl_no][i], SHADOW_VERTS, 0);
    gSPDisplayList(dlp++, shadow_blob_display_list);
  }

  gDPPipeSync(dlp++);
  gDPSetRenderMode(dlp++, G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2);
  /* Every entity pass now sets its own combiner, so this only has to leave
     behind something harmless rather than the exact mode the next one
     wanted. */
  gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
  gSPSetGeometryMode(dlp++, G_CULL_BACK);
}

void drawWorld() {
  u8 i, player_num;
  u8 cinematic = screenShowsPreview(current_screen);
  u8 viewer_count = cinematic ? 1 : active_player_count;
  u8 fogged = !cinematic && fog_enabled;
  SkyColor sky = dayCycleSkyColor(255);

  /* One set of casters for the whole frame.  Shadow vertices are world
     space, so every viewport draws the same nine-vertex blobs. */
  if (cinematic) {
    shadow_count = 0;
  } else {
    buildGroundShadows();
  }

  if (screenIsWorldPicker(current_screen)) {
    /* Black background while the first world loads. */
    clearBuffers(GPACK_RGBA5551(0, 0, 0, 1));
  } else if (cinematic) {
    /* A brighter blue keeps the distant water legible while the warm
       terrain light provides depth in the foreground. */
    clearBuffers(GPACK_RGBA5551(48, 123, 211, 1));
  } else {
    /* A single time-varying clear is much cheaper than the former banded
     * gradient while retaining a daylight/nightfall sky behind the terrain.
     * Fog reuses the same color, so hazed terrain dissolves exactly into
     * the sky it stands against. */
    clearBuffers(GPACK_RGBA5551(sky.r, sky.g, sky.b, 1));
  }

  for (player_num = 0; player_num < viewer_count; player_num++) {
    if (viewer_count > 1) {
      selectPlayerViewport(player_num);
    } else {
      gSPViewport(dlp++, &full_viewport);
      gDPSetScissor(dlp++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WD, SCREEN_HT);
    }
    gSPDisplayList(dlp++, draw_setup_display_list);
    loadCameraMatrices(player_num);
    if (cinematic) {
      /* The loading/menu orbit makes subpixel terrain edges move continuously.
         Edge AA softens the continuously moving terrain silhouette. */
      gDPSetRenderMode(dlp++, G_RM_AA_ZB_OPA_SURF,
        G_RM_AA_ZB_OPA_SURF2);
    } else {
      drawCelestialBodies(player_num);
      /* The sky pass changed combine, render mode and alpha compare; drain
         the pipe before reconfiguring for terrain -- attributes changing
         under a primitive still in flight is the README's lockup. */
      gDPPipeSync(dlp++);
      /* Celestial sprites use AA texture-edge mode.  Terrain must
         explicitly restore its opaque no-read render mode afterwards.
         When fog is on this is the near pass's mode; the far pass
         reconfigures below. */
      gDPSetRenderMode(dlp++, G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2);
    }
    gSPSetGeometryMode(dlp++, G_CULL_BACK | G_LIGHTING);
    gDPSetCombineMode(dlp++, G_CC_MODULATERGB, G_CC_MODULATERGB);
    if (cinematic) {
      setPreviewLight();
      gSPSetLights0(dlp++, preview_lights[dl_no]);
    } else {
      setWorldLight(player_num);
      gSPSetLights1(dlp++, world_lights[dl_no][player_num]);
    }
    /* Water no longer needs an unlit exception here: its surface vertices
       carry an upward normal like every other top face, so the whole terrain
       bank goes through the one light. */
    if (fogged && visible_far_count[player_num] > 0) {
      /*
       * Terrain in two passes split at the distance where fog begins
       * (culling classified each visible column against the live fog_start).
       *
       * Fog needs two-cycle mode: cycle one runs the fog blender, cycle two
       * the ordinary opaque z-buffered write; the combiner's second cycle
       * passes the first cycle's texture result through untouched.  Two-cycle
       * is also roughly half the RDP's pixel rate, and perspective means the
       * near columns are most of the terrain's filled pixels -- pixels the
       * fog band cannot touch by construction.  So the near pass runs in
       * single-cycle with fog off and identical output, and only the far
       * pass, whose columns actually reach into the band, pays the two-cycle
       * rate.  Fog blended toward the sky clear color still makes the
       * terrain edge and the streamed-in pop both happen behind haze.
       *
       * At the shipped fog_start the band begins past the mesh ring, the
       * far class is empty, and this branch -- texture loads, mode changes
       * and all -- is skipped outright: fog costs nothing until the player
       * tunes it inward.
       */
      for (i = 0; i < NUM_TEXTURES; i++) {
        loadTexture(textures[i]->texture);
        drawTextured(i, player_num, COLUMN_VISIBLE_NEAR);
      }
      /* Reconfiguring cycle type and render mode under near-pass primitives
         still in flight is the README's lockup; drain the pipe first. */
      gDPPipeSync(dlp++);
      gDPSetCycleType(dlp++, G_CYC_2CYCLE);
      gDPSetRenderMode(dlp++, G_RM_FOG_SHADE_A, G_RM_ZB_OPA_SURF2);
      gDPSetFogColor(dlp++, sky.r, sky.g, sky.b, 255);
      gSPFogPosition(dlp++, fog_start, fog_start + FOG_BAND);
      gSPSetGeometryMode(dlp++, G_FOG);
      gDPSetCombineMode(dlp++, G_CC_MODULATERGB, G_CC_PASS2);
      for (i = 0; i < NUM_TEXTURES; i++) {
        loadTexture(textures[i]->texture);
        drawTextured(i, player_num, COLUMN_VISIBLE_FAR);
      }
    } else {
      for (i = 0; i < NUM_TEXTURES; i++) {
        loadTexture(textures[i]->texture);
        drawTextured(i, player_num,
          COLUMN_VISIBLE_NEAR | COLUMN_VISIBLE_FAR);
      }
    }
    if (!cinematic) {
      if (fogged) {
        /* Entities and the HUD run unfogged in single-cycle mode; drain the
           terrain pipe before switching the RDP back. */
        gDPPipeSync(dlp++);
        gDPSetCycleType(dlp++, G_CYC_1CYCLE);
        gDPSetRenderMode(dlp++, G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2);
        gSPClearGeometryMode(dlp++, G_FOG);
      }
      drawGroundShadows();
      /*
       * From here the RSP light is off -- this geometry carries colours, not
       * normals -- and the day/night level reaches it as a primitive colour
       * the passes below modulate against instead.  Emitted once per viewport
       * because the tint is a property of the view, not of any one entity.
       * The sync is the usual precaution before touching an RDP attribute:
       * drawGroundShadows returns without one when it has nothing to draw.
       */
      gDPPipeSync(dlp++);
      gSPClearGeometryMode(dlp++, G_LIGHTING);
      setEntityTint(entity_tint);
      drawDetailsForPlayer(player_num);
      drawFallingTrees(player_num);
      drawDroppedItems(player_num);
      drawMobsForPlayer(player_num);
      /* Wild creatures, or the two facing each other in a battle.  Same
         entity pass, same tint, same combiner -- and never both, so the
         cost is bounded by whichever is larger rather than their sum. */
      mon64DrawForPlayer(player_num);
    }
    if (!cinematic && (active_player_count > 1 ||
        players[player_num].camera_mode == CAMERA_THIRD_PERSON)) {
      drawOtherPlayers(player_num);
    }
    if (!cinematic) {
      drawFirstPersonHand(player_num);
    }
  }

}

void drawWireframes() {
  u8 player_num;

  for (player_num = 0; player_num < active_player_count; player_num++) {
    if (!players[player_num].target_present) continue;
    if (active_player_count > 1) {
      selectPlayerViewport(player_num);
    } else {
      gSPViewport(dlp++, &full_viewport);
      gDPSetScissor(dlp++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WD, SCREEN_HT);
    }
    gSPDisplayList(dlp++, wireframe_setup_display_list);
    loadCameraMatrices(player_num);
    /* One origin-relative translation straight to the targeted block.  This
       composed the column matrix with a 2048-entry per-block table when the
       shell format kept such a table alive; with every mesh baked, the
       product of those two translations is cheaper to write than to store. */
    guTranslate(&wireframe_target_model[dl_no][player_num],
      (float) (players[player_num].target_x - render_origin_x) * BLOCK_SIZE,
      (float) players[player_num].target_y * BLOCK_SIZE,
      (float) (players[player_num].target_z - render_origin_z) * BLOCK_SIZE);
    gSPMatrix(dlp++,
      OS_K0_TO_PHYSICAL(&wireframe_target_model[dl_no][player_num]),
      G_MTX_MODELVIEW|G_MTX_LOAD|G_MTX_NOPUSH);
    gSPVertex(dlp++, cube_verts, 8, 0);
    gSPDisplayList(dlp++, wireframe_display_list);
    if (players[player_num].breaking) {
      u8 stage = players[player_num].break_progress * 3 /
        players[player_num].break_time;
      if (stage > 2) {
        stage = 2;
      }
      gSPVertex(dlp++, breaking_crack_verts, 23, 0);
      gSPDisplayList(dlp++, breaking_crack_stages[stage]);
    }
  }

}

static void setHudFillColor(u8 r, u8 g, u8 b);

static void drawLoadingOverlay() {
  u32 progress = loadingPreviewProgress();
  u32 filled = progress * 224 / 100;
  u32 pulse = (progress / 8) % 3;

  /* A compact framed status deck preserves nearly all of the flyover while
     making the handoff feel intentional on a CRT. */
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(9, 16, 30);
  gDPFillRectangle(dlp++, 0, SCREEN_HT - 36, SCREEN_WD - 1, SCREEN_HT - 1);
  setHudFillColor(71, 160, 196);
  gDPFillRectangle(dlp++, 47, SCREEN_HT - 20, 272, SCREEN_HT - 14);
  setHudFillColor(13, 35, 53);
  gDPFillRectangle(dlp++, 49, SCREEN_HT - 18, 270, SCREEN_HT - 16);
  if (filled > 0) {
    setHudFillColor(100 + pulse * 24, 215, 192);
    gDPFillRectangle(dlp++, 49, SCREEN_HT - 18, 48 + filled,
      SCREEN_HT - 16);
  }
  gDPPipeSync(dlp++);
}

static void drawCrosshair(u32 x, u32 y, Player *player) {
  u8 red = 255;
  u8 green = 255;
  u8 blue = 255;

  if (player->breaking) {
    green = 180;
    blue = 48;
  } else if (player->target_present) {
    red = 150;
    blue = 150;
  }
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  gDPSetFillColor(dlp++, (GPACK_RGBA5551(0, 0, 0, 1) << 16 |
    GPACK_RGBA5551(0, 0, 0, 1)));
  gDPFillRectangle(dlp++, x - CROSSHAIR_SIZE / 2 - 1, y - 2,
    x + CROSSHAIR_SIZE / 2, y + 1);
  gDPFillRectangle(dlp++, x - 2, y - CROSSHAIR_SIZE / 2 - 1,
    x + 1, y + CROSSHAIR_SIZE / 2);
  gDPSetFillColor(dlp++, (GPACK_RGBA5551(red, green, blue, 1) << 16 |
    GPACK_RGBA5551(red, green, blue, 1)));
  gDPFillRectangle(dlp++, x - CROSSHAIR_SIZE / 2, y - 1, x + CROSSHAIR_SIZE / 2 - 1, y);
  gDPFillRectangle(dlp++, x - 1, y - CROSSHAIR_SIZE / 2, x, y + CROSSHAIR_SIZE / 2 - 1);
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_1CYCLE);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
  gDPSetPrimColor(dlp++, 0, 0, 255, 255, 255, 255);
  gDPSetTexturePersp(dlp++, G_TP_NONE);
  gDPSetTextureLUT(dlp++, G_TT_RGBA16);
}

static void drawBreakProgress(u32 x, u32 y, Player *player) {
  u32 width;

  if (!player->breaking || player->break_time <= 0) {
    return;
  }
  width = player->break_progress * 30 / player->break_time;
  if (width > 30) {
    width = 30;
  }
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(15, 15, 15);
  gDPFillRectangle(dlp++, x - 17, y + 10, x + 16, y + 14);
  if (width > 0) {
    setHudFillColor(255, 190, 48);
    gDPFillRectangle(dlp++, x - 15, y + 12, x - 16 + width, y + 12);
  }
}

static void setHudFillColor(u8 r, u8 g, u8 b) {
  u32 color = GPACK_RGBA5551(r, g, b, 1);
  gDPSetFillColor(dlp++, (color << 16) | color);
}

static void drawHealth(u8 player_num) {
  u8 compact = usesFourPlayerLayout();
  u32 size = compact ? 4 : 6;
  u32 total_width = (PLAYER_MAX_HEALTH / 2) * (size + 2) - 2;
  u32 x = playerViewportX(player_num) +
    (playerViewportWidth() - total_width) / 2;
  u32 start_x = x;
  u32 y = playerViewportY(player_num) + playerViewportHeight() -
    (compact ? 14 + 4 + 8 : HOTBAR_SLOT_SIZE + HOTBAR_MARGIN + 10);
  u8 heart;

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  for (heart = 0; heart < PLAYER_MAX_HEALTH / 2; heart++) {
    u8 value = players[player_num].health > heart * 2 ?
      min(2, players[player_num].health - heart * 2) : 0;

    setHudFillColor(72, 22, 22);
    /* A few filled pixels read as a heart on a CRT more clearly than a
       single health bar, while still fitting beside a four-player hotbar. */
    gDPFillRectangle(dlp++, x + 1, y, x + size - 2, y + 1);
    gDPFillRectangle(dlp++, x, y + 1, x + size - 1, y + size - 2);
    gDPFillRectangle(dlp++, x + 1, y + size - 1, x + size - 2, y + size);
    if (value > 0) {
      u32 fill_right = value == 2 ? x + size - 1 : x + size / 2;
      setHudFillColor(220, 48, 48);
      gDPFillRectangle(dlp++, x + 1, y,
        value == 2 ? x + size - 2 : fill_right, y + 1);
      gDPFillRectangle(dlp++, x, y + 1, fill_right, y + size - 2);
      gDPFillRectangle(dlp++, x + 1, y + size - 1,
        value == 2 ? x + size - 2 : fill_right, y + size);
    }
    x += size + 2;
  }

  /* Food pips share the same fill-mode pass as hearts, so survival adds no
     extra RDP state transition.  Their warm diamond silhouette remains
     distinct from health even on a soft composite CRT image. */
  x = start_x;
  y -= size + 3;
  for (heart = 0; heart < PLAYER_MAX_HUNGER / 2; heart++) {
    u8 value = players[player_num].hunger > heart * 2 ?
      min(2, players[player_num].hunger - heart * 2) : 0;

    setHudFillColor(61, 39, 24);
    gDPFillRectangle(dlp++, x + 1, y, x + size - 2, y + size - 1);
    gDPFillRectangle(dlp++, x, y + 1, x + size - 1, y + size - 2);
    if (value > 0) {
      u32 fill_right = value == 2 ? x + size - 1 : x + size / 2;
      setHudFillColor(226, 154, 55);
      gDPFillRectangle(dlp++, x + 1, y, value == 2 ? x + size - 2 : fill_right,
        y + size - 1);
      gDPFillRectangle(dlp++, x, y + 1, fill_right, y + size - 2);
    }
    x += size + 2;
  }
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_1CYCLE);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
  gDPSetPrimColor(dlp++, 0, 0, 255, 255, 255, 255);
  gDPSetTexturePersp(dlp++, G_TP_NONE);
  gDPSetTextureLUT(dlp++, G_TT_RGBA16);
}

static void drawObjectivePanel(Player *player) {
  u8 expanded = player->objective_time > 0;
  u32 left = expanded ? 174 : 190;
  u32 top = 7;
  u32 right = 313;
  u32 bottom = expanded ? 43 : 22;
  u8 segment;

  /* The beveled charcoal card borrows the reference shot's Rare-era
     hierarchy, then collapses once the player has read each new goal. */
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(8, 10, 13);
  gDPFillRectangle(dlp++, left, top, right, bottom);
  setHudFillColor(131, 137, 139);
  gDPFillRectangle(dlp++, left + 2, top + 2, right - 2, bottom - 2);
  setHudFillColor(24, 29, 34);
  gDPFillRectangle(dlp++, left + 4, top + 4, right - 4, bottom - 4);
  if (expanded) {
    for (segment = 0; segment < PLAYER_OBJECTIVE_COUNT; segment++) {
      u32 x = left + 7 + segment * 15;
      u8 complete = segment < player->objective_stage;
      setHudFillColor(complete ? 102 : 48, complete ? 196 : 55,
        complete ? 121 : 61);
      gDPFillRectangle(dlp++, x, bottom - 8, x + 11, bottom - 6);
    }
  }
  gDPPipeSync(dlp++);
}

/* The compass is a ribbon that slides under a fixed pointer rather than a
   dial that snaps between the four cardinals.  The window spans a 120 degree
   arc across 40 pixels: wide enough that two cardinal letters stay readable
   while the player faces an intercardinal, tight enough that a single degree
   of turn still moves the marks a third of a pixel, so they creep rather than
   jump at any turn rate the stick can produce. */
#define COMPASS_INNER_LEFT 128
#define COMPASS_INNER_RIGHT 167
#define COMPASS_CENTER_X 147.5f
#define COMPASS_PIXELS_PER_DEGREE (40.f / 120.f)

/* Pixel column the ribbon mark for a world bearing lands on.  Gameplay yaw
   runs the opposite way round the compass -- yaw 0 faces north but yaw 90
   faces west -- which is why bearing and yaw add rather than subtract.  The
   difference wraps into +/-180 so marks behind the player reappear from the
   far edge instead of piling up against one end. */
static s32 compassMarkX(float bearing, float yaw) {
  float delta = bearing + yaw;

  while (delta >= 180.f) {
    delta -= 360.f;
  }
  while (delta < -180.f) {
    delta += 360.f;
  }
  return (s32)(COMPASS_CENTER_X + delta * COMPASS_PIXELS_PER_DEGREE + 0.5f);
}

static void drawCompass(Player *player) {
  u8 mark;

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(7, 10, 13);
  gDPFillRectangle(dlp++, 124, 6, 171, 20);
  setHudFillColor(111, 118, 121);
  gDPFillRectangle(dlp++, 126, 8, 169, 18);
  setHudFillColor(25, 31, 35);
  gDPFillRectangle(dlp++, 128, 9, 167, 17);

  /* One tick every 15 degrees, taller on the intercardinals.  The cardinals
     are skipped: drawCompassLabels lays their letters in the same lane. */
  for (mark = 0; mark < 24; mark++) {
    s32 x = compassMarkX(mark * 15.f, player->yaw);

    if (mark % 6 == 0 || x <= COMPASS_INNER_LEFT ||
        x >= COMPASS_INNER_RIGHT) {
      continue;
    }
    if (mark % 3 == 0) {
      setHudFillColor(146, 153, 153);
      gDPFillRectangle(dlp++, x, 10, x, 16);
    } else {
      setHudFillColor(91, 98, 98);
      gDPFillRectangle(dlp++, x, 13, x, 16);
    }
  }
  setHudFillColor(238, 194, 67);
  gDPFillRectangle(dlp++, 147, 15, 148, 19);
  gDPPipeSync(dlp++);
}

static void drawCButtonGuide(Player *player) {
  u8 expanded = player->objective_time > 0;

  if (!expanded) {
    return;
  }
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(8, 10, 13);
  gDPFillRectangle(dlp++, 5, 160, 108, 198);
  setHudFillColor(131, 137, 139);
  gDPFillRectangle(dlp++, 7, 162, 106, 196);
  setHudFillColor(24, 29, 34);
  gDPFillRectangle(dlp++, 9, 164, 104, 194);

  setHudFillColor(225, 174, 42);
  gDPFillRectangle(dlp++, 18, 165, 23, 170);
  gDPFillRectangle(dlp++, 11, 173, 16, 178);
  gDPFillRectangle(dlp++, 25, 173, 30, 178);
  gDPFillRectangle(dlp++, 18, 181, 23, 186);
  gDPPipeSync(dlp++);
}

static void drawActionGuide(Player *player) {
  if (player->objective_time <= 0) {
    return;
  }
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(8, 10, 13);
  gDPFillRectangle(dlp++, 258, 160, 314, 198);
  setHudFillColor(131, 137, 139);
  gDPFillRectangle(dlp++, 260, 162, 312, 196);
  setHudFillColor(24, 29, 34);
  gDPFillRectangle(dlp++, 262, 164, 310, 194);
  setHudFillColor(47, 106, 191);
  gDPFillRectangle(dlp++, 266, 166, 277, 176);
  setHudFillColor(48, 144, 65);
  gDPFillRectangle(dlp++, 266, 177, 277, 187);
  setHudFillColor(116, 121, 118);
  gDPFillRectangle(dlp++, 266, 188, 277, 193);
  gDPPipeSync(dlp++);
}

/* A compact, deliberately chunky version of the Minecraft hotbar.  It uses
   the existing 16x16 block previews, keeping the HUD cheap enough for both
   split-screen players without introducing another texture atlas. */
static void drawItemIcon(u8 item, u32 x, u32 y, u32 size) {
  if (ITEM_IS_VALID(item) &&
      preview_textures[item] != NULL) {
    loadTexture(preview_textures[item]);
    gSPTextureRectangle(dlp++, x << 2, y << 2,
      ((x + size) << 2) - 2, ((y + size) << 2) - 2,
      G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);
    return;
  }

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  if (item == STICK) {
    setHudFillColor(142, 83, 38);
    gDPFillRectangle(dlp++, x + size / 2 - 1, y + 2, x + size / 2 + 1, y + size - 3);
  } else if (item == TORCH) {
    setHudFillColor(142, 83, 38);
    gDPFillRectangle(dlp++, x + size / 2 - 1, y + 5,
      x + size / 2 + 1, y + size - 2);
    setHudFillColor(255, 204, 54);
    gDPFillRectangle(dlp++, x + size / 2 - 3, y + 1,
      x + size / 2 + 3, y + 6);
    setHudFillColor(238, 82, 31);
    gDPFillRectangle(dlp++, x + size / 2 - 1, y + 2,
      x + size / 2 + 2, y + 4);
  } else if (itemIsSword(item)) {
    if (item == IRON_SWORD) {
      setHudFillColor(225, 225, 218);
    } else if (item == STONE_SWORD) {
      setHudFillColor(188, 188, 178);
    } else {
      setHudFillColor(174, 117, 61);
    }
    /* A two-step blade keeps the pointed 3D sword recognisable at 10-16 px. */
    gDPFillRectangle(dlp++, x + size / 2, y + 1, x + size / 2 + 1, y + 3);
    gDPFillRectangle(dlp++, x + size / 2 - 1, y + 3,
      x + size / 2 + 1, y + size - 6);
    setHudFillColor(142, 83, 38);
    gDPFillRectangle(dlp++, x + size / 2 - 3, y + size - 6, x + size / 2 + 3, y + size - 4);
    gDPFillRectangle(dlp++, x + size / 2 - 1, y + size - 3, x + size / 2 + 1, y + size - 1);
  } else if (itemIsPickaxe(item)) {
    if (item == IRON_PICKAXE) {
      setHudFillColor(225, 225, 218);
    } else if (item == STONE_PICKAXE) {
      setHudFillColor(188, 188, 178);
    } else {
      setHudFillColor(174, 117, 61);
    }
    gDPFillRectangle(dlp++, x + 2, y + 2, x + size - 3, y + 3);
    gDPFillRectangle(dlp++, x + 1, y + 4, x + size - 2, y + 5);
    setHudFillColor(142, 83, 38);
    gDPFillRectangle(dlp++, x + size / 2 - 1, y + 4, x + size / 2 + 1, y + size - 1);
  } else if (itemIsAxe(item)) {
    if (item == IRON_AXE) {
      setHudFillColor(225, 225, 218);
    } else if (item == STONE_AXE) {
      setHudFillColor(188, 188, 178);
    } else {
      setHudFillColor(174, 117, 61);
    }
    gDPFillRectangle(dlp++, x + 2, y + 2, x + size / 2 + 2,
      y + size / 2 - 2);
    gDPFillRectangle(dlp++, x + 4, y + size / 2 - 1, x + size / 2 + 1,
      y + size / 2 + 1);
    setHudFillColor(142, 83, 38);
    gDPFillRectangle(dlp++, x + size / 2, y + 4, x + size / 2 + 2,
      y + size - 1);
  } else if (item == COAL) {
    setHudFillColor(39, 42, 43);
    gDPFillRectangle(dlp++, x + 3, y + 3, x + size - 3, y + size - 3);
    setHudFillColor(77, 80, 78);
    gDPFillRectangle(dlp++, x + 4, y + 3, x + size - 5, y + 4);
  } else if (item == IRON_CHUNK) {
    setHudFillColor(185, 128, 86);
    gDPFillRectangle(dlp++, x + 2, y + 4, x + size - 3, y + size - 3);
    setHudFillColor(224, 168, 114);
    gDPFillRectangle(dlp++, x + 4, y + 3, x + size - 5, y + 5);
  } else if (item == APPLE) {
    setHudFillColor(210, 43, 37);
    gDPFillRectangle(dlp++, x + 3, y + 4, x + size - 3, y + size - 2);
    setHudFillColor(55, 116, 47);
    gDPFillRectangle(dlp++, x + size / 2, y + 1, x + size / 2 + 3, y + 4);
  } else if (item == RAW_MUTTON || item == RAW_PORK) {
    setHudFillColor(item == RAW_PORK ? 207 : 172,
      item == RAW_PORK ? 111 : 75, item == RAW_PORK ? 112 : 67);
    gDPFillRectangle(dlp++, x + 2, y + 4, x + size - 3, y + size - 3);
    setHudFillColor(232, 181, 164);
    gDPFillRectangle(dlp++, x + 4, y + 4, x + size - 5, y + 5);
  } else if (item == SLIME_GEL) {
    setHudFillColor(75, 174, 72);
    gDPFillRectangle(dlp++, x + 3, y + 3, x + size - 3, y + size - 3);
    setHudFillColor(132, 226, 119);
    gDPFillRectangle(dlp++, x + 4, y + 3, x + size - 5, y + 5);
  }
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_1CYCLE);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
  gDPSetPrimColor(dlp++, 0, 0, 255, 255, 255, 255);
  gDPSetTexturePersp(dlp++, G_TP_NONE);
  gDPSetTextureLUT(dlp++, G_TT_RGBA16);
}

static void drawHotbar(u8 player_num) {
  u8 compact = usesFourPlayerLayout();
  u32 viewport_width = playerViewportWidth();
  u32 viewport_height = playerViewportHeight();
  u32 x_offset = playerViewportX(player_num);
  u32 y_offset = playerViewportY(player_num);
  u32 slot_size = compact ? 14 : HOTBAR_SLOT_SIZE;
  u32 icon_size = compact ? 10 : HOTBAR_ICON_SIZE;
  u32 margin = compact ? 4 : HOTBAR_MARGIN;
  u32 bar_width = HOTBAR_SLOT_COUNT * slot_size;
  u32 bar_x = x_offset + (viewport_width - bar_width) / 2;
  u32 bar_y = y_offset + viewport_height - slot_size - margin;
  u8 slot;

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);

  /* One raised rim groups all nine slots into a single familiar item bar. */
  setHudFillColor(20, 20, 20);
  gDPFillRectangle(dlp++, bar_x - 2, bar_y - 2, bar_x + bar_width + 1,
    bar_y + slot_size + 1);

  for (slot = 0; slot < HOTBAR_SLOT_COUNT; slot++) {
    u32 x = bar_x + slot * slot_size;
    u8 selected = players[player_num].selected_hotbar_slot == slot;

    setHudFillColor(selected ? 250 : 78, selected ? 250 : 78, selected ? 250 : 78);
    gDPFillRectangle(dlp++, x, bar_y, x + slot_size - 1,
      bar_y + slot_size - 1);
    setHudFillColor(selected ? 118 : 42, selected ? 118 : 42, selected ? 118 : 42);
    gDPFillRectangle(dlp++, x + 2, bar_y + 2, x + slot_size - 3,
      bar_y + slot_size - 3);
  }

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_1CYCLE);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
  gDPSetPrimColor(dlp++, 0, 0, 255, 255, 255, 255);
  gDPSetTexturePersp(dlp++, G_TP_NONE);
  gDPSetTextureLUT(dlp++, G_TT_RGBA16);

  for (slot = 0; slot < HOTBAR_SLOT_COUNT; slot++) {
    u32 x = bar_x + slot * slot_size + (slot_size - icon_size) / 2;
    u32 y = bar_y + (slot_size - icon_size) / 2;
    ItemStack *stack = &players[player_num].inventory[INVENTORY_HOTBAR_START + slot];
    if (stack->count > 0) {
      drawItemIcon(stack->item, x, y, icon_size);
    }
  }

}

static void drawInventorySlot(u32 x, u32 y, ItemStack *stack, u8 selected) {
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(16, 19, 21);
  gDPFillRectangle(dlp++, x, y, x + INVENTORY_SLOT_SIZE - 1, y + INVENTORY_SLOT_SIZE - 1);
  setHudFillColor(102, 108, 104);
  gDPFillRectangle(dlp++, x + 1, y + 1, x + INVENTORY_SLOT_SIZE - 2, y + 2);
  gDPFillRectangle(dlp++, x + 1, y + 2, x + 2, y + INVENTORY_SLOT_SIZE - 2);
  setHudFillColor(39, 44, 45);
  gDPFillRectangle(dlp++, x + 3, y + 3, x + INVENTORY_SLOT_SIZE - 3, y + INVENTORY_SLOT_SIZE - 3);

  if (stack->item != AIR && stack->count > 0) {
    gDPPipeSync(dlp++);
    gDPSetCycleType(dlp++, G_CYC_1CYCLE);
    gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
    gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
    gDPSetPrimColor(dlp++, 0, 0, 255, 255, 255, 255);
    gDPSetTexturePersp(dlp++, G_TP_NONE);
    gDPSetTextureLUT(dlp++, G_TT_RGBA16);
    drawItemIcon(stack->item, x + 2, y + 2, INVENTORY_ICON_SIZE);
  }

  /* Equipped remains a restrained green underline, separate from focus. */
  if (selected) {
    gDPPipeSync(dlp++);
    gDPSetCycleType(dlp++, G_CYC_FILL);
    gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
    setHudFillColor(89, 213, 105);
    gDPFillRectangle(dlp++, x + 4, y + INVENTORY_SLOT_SIZE - 3,
      x + INVENTORY_SLOT_SIZE - 5, y + INVENTORY_SLOT_SIZE - 2);
  }
}

static void drawInventoryFocus(u32 x, u32 y) {
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(242, 193, 54);
  gDPFillRectangle(dlp++, x - 1, y - 1, x + INVENTORY_SLOT_SIZE, y);
  gDPFillRectangle(dlp++, x, y + INVENTORY_SLOT_SIZE - 2,
    x + INVENTORY_SLOT_SIZE - 1, y + INVENTORY_SLOT_SIZE);
  gDPFillRectangle(dlp++, x - 1, y, x, y + INVENTORY_SLOT_SIZE - 1);
  gDPFillRectangle(dlp++, x + INVENTORY_SLOT_SIZE - 2, y + 2,
    x + INVENTORY_SLOT_SIZE, y + INVENTORY_SLOT_SIZE - 3);
}

static void drawInventoryButton(u32 x, u32 y, u8 red, u8 green, u8 blue,
    u8 wide) {
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(11, 13, 15);
  gDPFillRectangle(dlp++, x, y, x + (wide ? 21 : 13), y + 13);
  setHudFillColor(red, green, blue);
  gDPFillRectangle(dlp++, x + 2, y + 2, x + (wide ? 19 : 11), y + 11);
  setHudFillColor(min(255, red + 55), min(255, green + 55),
    min(255, blue + 55));
  gDPFillRectangle(dlp++, x + 3, y + 3, x + (wide ? 18 : 10), y + 4);
}

static u32 inventoryItemCount(Player *player, u8 item);

static u8 recipeIngredientsAvailable(Player *player, u8 recipe_index) {
  const CraftRecipe *recipe = &craft_recipes[recipe_index];
  u8 ingredient;

  for (ingredient = 0; ingredient < 2; ingredient++) {
    if (recipe->ingredient_count[ingredient] > 0 &&
        inventoryItemCount(player, recipe->ingredient_item[ingredient]) <
          recipe->ingredient_count[ingredient]) {
      return FALSE;
    }
  }
  return TRUE;
}

static void drawInventory() {
  Player *player = &players[inventory_player];
  u8 recipe_count = playerRecipeCount(player);
  u8 recipe_start = player->crafting_cursor > 2 ?
    player->crafting_cursor - 2 : 0;
  u8 row, column;

  if (recipe_count > RECIPE_VISIBLE_ROWS &&
      recipe_start > recipe_count - RECIPE_VISIBLE_ROWS) {
    recipe_start = recipe_count - RECIPE_VISIBLE_ROWS;
  }
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  /* A single full-screen workbench with strong internal grouping survives
     composite blur better than nested translucent boxes. */
  setHudFillColor(7, 9, 12);
  gDPFillRectangle(dlp++, 6, 6, SCREEN_WD - 7, SCREEN_HT - 7);
  setHudFillColor(115, 121, 117);
  gDPFillRectangle(dlp++, 8, 8, SCREEN_WD - 9, SCREEN_HT - 9);
  setHudFillColor(26, 31, 33);
  gDPFillRectangle(dlp++, 11, 11, SCREEN_WD - 12, SCREEN_HT - 12);
  setHudFillColor(37, 43, 44);
  gDPFillRectangle(dlp++, 12, 12, SCREEN_WD - 13, 31);
  setHudFillColor(9, 11, 13);
  gDPFillRectangle(dlp++, 190, 31, 194, 180);
  setHudFillColor(100, 106, 102);
  gDPFillRectangle(dlp++, 191, 32, 191, 179);

  /* Selected-item summary and recipe details have fixed homes, so text never
     jumps as the cursor crosses between the two panes. */
  setHudFillColor(17, 21, 23);
  gDPFillRectangle(dlp++, 16, 129, 185, 178);
  setHudFillColor(68, 75, 75);
  gDPFillRectangle(dlp++, 17, 130, 184, 131);
  gDPFillRectangle(dlp++, 17, 130, 18, 177);
  setHudFillColor(17, 21, 23);
  gDPFillRectangle(dlp++, 197, 164, 304, 203);
  setHudFillColor(68, 75, 75);
  gDPFillRectangle(dlp++, 198, 165, 303, 166);

  /* Footer changes with focus; button shapes carry controller color even
     when the small font is softened by a CRT. */
  if (player->inventory_area == INVENTORY_AREA_OUTPUT) {
    setHudFillColor(12, 15, 17);
    gDPFillRectangle(dlp++, 12, 205, 307, 227);
    setHudFillColor(69, 75, 74);
    gDPFillRectangle(dlp++, 13, 206, 306, 207);
    drawInventoryButton(18, 210, 43, 104, 190, FALSE);
    drawInventoryButton(110, 210, 190, 146, 36, TRUE);
    drawInventoryButton(240, 210, 51, 145, 65, FALSE);
  } else {
    setHudFillColor(12, 15, 17);
    gDPFillRectangle(dlp++, 12, 184, 307, 227);
    setHudFillColor(69, 75, 74);
    gDPFillRectangle(dlp++, 13, 185, 306, 186);
    drawInventoryButton(18, 191, 43, 104, 190, FALSE);
    drawInventoryButton(115, 191, 51, 145, 65, FALSE);
    drawInventoryButton(18, 211, 190, 146, 36, TRUE);
    drawInventoryButton(93, 211, 190, 146, 36, TRUE);
    drawInventoryButton(178, 211, 190, 146, 36, TRUE);
    drawInventoryButton(248, 211, 190, 146, 36, TRUE);
  }

  for (row = 0; row < INVENTORY_STORAGE_ROWS + INVENTORY_HOTBAR_ROWS; row++) {
    for (column = 0; column < INVENTORY_COLUMNS; column++) {
      u8 index = row * INVENTORY_COLUMNS + column;
      u32 y = row == INVENTORY_STORAGE_ROWS ? INVENTORY_HOTBAR_Y :
        INVENTORY_GRID_Y + row * INVENTORY_SLOT_SIZE;
      drawInventorySlot(INVENTORY_GRID_X + column * INVENTORY_SLOT_SIZE, y,
        &player->inventory[index], row == INVENTORY_STORAGE_ROWS &&
        player->selected_hotbar_slot == column);
    }
  }
  if (player->inventory[player->inventory_cursor].count > 0) {
    drawItemIcon(player->inventory[player->inventory_cursor].item,
      22, 139, 28);
  }
  drawInventorySlot(164, 142, &player->carried_item, FALSE);

  /* Recipe rows use icons and an availability pip instead of exposing the
     old manual crafting matrix. */
  for (row = 0; row < RECIPE_VISIBLE_ROWS &&
      recipe_start + row < recipe_count; row++) {
    u8 recipe = recipe_start + row;
    u32 y = RECIPE_LIST_Y + row * RECIPE_ROW_HEIGHT;
    u8 selected = player->inventory_area == INVENTORY_AREA_OUTPUT &&
      player->crafting_cursor == recipe;
    u8 available = recipeIngredientsAvailable(player, recipe);

    gDPPipeSync(dlp++);
    gDPSetCycleType(dlp++, G_CYC_FILL);
    gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
    setHudFillColor(selected ? 99 : 17, selected ? 79 : 21,
      selected ? 25 : 23);
    gDPFillRectangle(dlp++, RECIPE_LIST_X, y,
      SCREEN_WD - 17, y + RECIPE_ROW_HEIGHT - 2);
    setHudFillColor(available ? 76 : 112, available ? 194 : 52,
      available ? 94 : 48);
    gDPFillRectangle(dlp++, RECIPE_LIST_X + 2, y + 3,
      RECIPE_LIST_X + 3, y + RECIPE_ROW_HEIGHT - 5);
    drawItemIcon(craft_recipes[recipe].result_item,
      RECIPE_LIST_X + 6, y + 2, INVENTORY_ICON_SIZE);
  }

  if (recipe_start > 0) {
    gDPPipeSync(dlp++);
    gDPSetCycleType(dlp++, G_CYC_FILL);
    setHudFillColor(230, 185, 49);
    gDPFillRectangle(dlp++, 299, 35, 302, 36);
    gDPFillRectangle(dlp++, 300, 34, 301, 34);
  }
  if (recipe_start + RECIPE_VISIBLE_ROWS < recipe_count) {
    gDPPipeSync(dlp++);
    gDPSetCycleType(dlp++, G_CYC_FILL);
    setHudFillColor(230, 185, 49);
    gDPFillRectangle(dlp++, 299, 160, 302, 161);
    gDPFillRectangle(dlp++, 300, 162, 301, 162);
  }

  if (player->inventory_area == INVENTORY_AREA_ITEMS) {
    u32 focus_y = player->inventory_cursor / INVENTORY_COLUMNS ==
      INVENTORY_STORAGE_ROWS ? INVENTORY_HOTBAR_Y :
      INVENTORY_GRID_Y + (player->inventory_cursor / INVENTORY_COLUMNS) *
      INVENTORY_SLOT_SIZE;
    drawInventoryFocus(INVENTORY_GRID_X +
      (player->inventory_cursor % INVENTORY_COLUMNS) * INVENTORY_SLOT_SIZE,
      focus_y);
  }
}

static void drawCompactDigit(char digit, u32 x, u32 y) {
  u8 index = digit - ' ';
  u32 source_x = index % 16;
  u32 source_y = (index / 16) + 2;

  gSPTextureRectangle(dlp++,
    x << 2, y << 2,
    ((x + 6) << 2) - 2, ((y + 6) << 2) - 2,
    G_TX_RENDERTILE,
    (source_x * 8) << 5, (source_y * 8) << 5,
    (8 << 10) / 6, (8 << 10) / 6);
}

static void drawStackCount(ItemStack *stack, u32 x, u32 y,
    u32 slot_size) {
  u32 digit_x;
  u32 digit_y;

  if (stack->count <= 1) {
    return;
  }
  digit_x = x + slot_size - (stack->count >= 10 ? 13 : 7);
  digit_y = y + slot_size - 7;

  /* Six-pixel digits sit against the lower-right rim instead of masking the
     icon.  A one-pixel shadow keeps them readable over bright materials. */
  gDPSetPrimColor(dlp++, 0, 0, 12, 14, 15, 255);
  if (stack->count >= 10) {
    drawCompactDigit('0' + stack->count / 10, digit_x + 1, digit_y + 1);
    drawCompactDigit('0' + stack->count % 10, digit_x + 7, digit_y + 1);
  } else {
    drawCompactDigit('0' + stack->count, digit_x + 1, digit_y + 1);
  }
  gDPSetPrimColor(dlp++, 0, 0, 248, 248, 238, 255);
  if (stack->count >= 10) {
    drawCompactDigit('0' + stack->count / 10, digit_x, digit_y);
    drawCompactDigit('0' + stack->count % 10, digit_x + 6, digit_y);
  } else {
    drawCompactDigit('0' + stack->count, digit_x, digit_y);
  }
}

static void drawInventoryStackCounts() {
  Player *player = &players[inventory_player];
  u8 row, column;

  for (row = 0; row < INVENTORY_STORAGE_ROWS + INVENTORY_HOTBAR_ROWS; row++) {
    for (column = 0; column < INVENTORY_COLUMNS; column++) {
      ItemStack *stack = &player->inventory[row * INVENTORY_COLUMNS + column];
      u32 x = INVENTORY_GRID_X + column * INVENTORY_SLOT_SIZE;
      u32 y = row == INVENTORY_STORAGE_ROWS ? INVENTORY_HOTBAR_Y :
        INVENTORY_GRID_Y + row * INVENTORY_SLOT_SIZE;

      drawStackCount(stack, x, y, INVENTORY_SLOT_SIZE);
    }
  }
  if (player->carried_item.count > 0) {
    drawStackCount(&player->carried_item, 164, 142, INVENTORY_SLOT_SIZE);
  }
}

static u32 inventoryItemCount(Player *player, u8 item) {
  u8 slot;
  u32 count = 0;

  for (slot = 0; slot < INVENTORY_SIZE; slot++) {
    if (player->inventory[slot].item == item) {
      count += player->inventory[slot].count;
    }
  }
  return count;
}

static u32 drawUnsigned(u32 value, u32 x, u32 y) {
  char digits[5];
  u8 count = 0;
  u8 i;

  do {
    digits[count++] = '0' + value % 10;
    value /= 10;
  } while (value > 0 && count < sizeof(digits));
  for (i = 0; i < count; i++) {
    char digit = digits[count - i - 1];
    drawChar(digit, x, y);
    x += charWidth(digit);
  }
  return x;
}

static void setHudTextColor(u8 red, u8 green, u8 blue) {
  gDPSetPrimColor(dlp++, 0, 0, red, green, blue, 255);
}

/* The cardinal glyphs ride the ribbon drawCompass laid down, so they scroll
   with the ticks instead of cutting from one letter to the next.  A scissor
   clipped to the strip lets the letter arriving at an edge come in a column
   at a time rather than popping in whole; north gets the warm tint so a
   glance still finds it without reading the glyph. */
static void drawCompassLabels(Player *player) {
  static const char cardinals[4] = { 'N', 'E', 'S', 'W' };
  u8 i;

  gDPPipeSync(dlp++);
  gDPSetScissor(dlp++, G_SC_NON_INTERLACE, COMPASS_INNER_LEFT + 1, 9,
    COMPASS_INNER_RIGHT, 18);
  for (i = 0; i < 4; i++) {
    s32 x = compassMarkX(i * 90.f, player->yaw) - 3;

    if (x + 8 <= COMPASS_INNER_LEFT || x >= COMPASS_INNER_RIGHT) {
      continue;
    }
    if (i == 0) {
      setHudTextColor(246, 214, 128);
    } else {
      setHudTextColor(241, 241, 232);
    }
    drawChar(cardinals[i], (u32)x, 10);
  }
  gDPPipeSync(dlp++);
  gDPSetScissor(dlp++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WD, SCREEN_HT);
}

static const char *shortIngredientName(u8 item) {
  if (item == COBBLESTONE) return "Cobble";
  if (item == IRON_CHUNK) return "Iron";
  return itemName(item);
}

static void drawInventoryText() {
  Player *player = &players[inventory_player];
  ItemStack *selected = &player->inventory[player->inventory_cursor];
  const CraftRecipe *recipe = &craft_recipes[player->crafting_cursor];
  u8 recipe_count = playerRecipeCount(player);
  u8 recipe_start = player->crafting_cursor > 2 ?
    player->crafting_cursor - 2 : 0;
  u8 row;

  if (recipe_count > RECIPE_VISIBLE_ROWS &&
      recipe_start > recipe_count - RECIPE_VISIBLE_ROWS) {
    recipe_start = recipe_count - RECIPE_VISIBLE_ROWS;
  }

  beginText();
  setHudTextColor(241, 195, 58);
  drawString("PACK", 18, 18);
  drawString(player->crafting_table_open ? "WORKBENCH" : "POCKET CRAFT",
    200, 18);
  setHudTextColor(185, 192, 187);
  drawString("STORAGE", 18, 34);
  drawString("HOTBAR", 18, 95);
  drawChar('P', 166, 18);
  drawChar('1' + inventory_player, 173, 18);

  if (selected->count > 0) {
    setHudTextColor(241, 241, 232);
    drawString(itemName(selected->item), 55, 137);
    setHudTextColor(155, 164, 160);
    drawString("STACK", 55, 151);
    setHudTextColor(241, 195, 58);
    drawUnsigned(selected->count, 91, 151);
  } else {
    setHudTextColor(126, 135, 132);
    drawString("EMPTY SLOT", 55, 143);
  }
  setHudTextColor(player->carried_item.count > 0 ?
    241 : 126, player->carried_item.count > 0 ? 241 : 135,
    player->carried_item.count > 0 ? 232 : 132);
  drawString("HAND", 151, 132);

  for (row = 0; row < RECIPE_VISIBLE_ROWS &&
      recipe_start + row < recipe_count; row++) {
    u8 recipe_index = recipe_start + row;
    u32 y = RECIPE_LIST_Y + row * RECIPE_ROW_HEIGHT + 6;
    u8 available = recipeIngredientsAvailable(player, recipe_index);

    if (player->inventory_area == INVENTORY_AREA_OUTPUT &&
        player->crafting_cursor == recipe_index) {
      setHudTextColor(255, 222, 105);
    } else if (available) {
      setHudTextColor(226, 231, 219);
    } else {
      setHudTextColor(116, 122, 119);
    }
    drawString(itemName(craft_recipes[recipe_index].result_item),
      RECIPE_LIST_X + 23, y);
  }

  if (player->inventory_area == INVENTORY_AREA_OUTPUT) {
    setHudTextColor(241, 195, 58);
    drawString("MAKES", 202, 168);
    drawString("MAX", 264, 168);
    drawUnsigned(recipeCraftableCount(player, player->crafting_cursor),
      286, 168);
    setHudTextColor(235, 237, 227);
    drawString(itemName(recipe->result_item), 202, 178);
    drawChar('x', 285, 178);
    drawUnsigned(recipe->result_count, 292, 178);
    for (row = 0; row < 2; row++) {
      u32 have;
      u32 x;
      u32 y;

      if (recipe->ingredient_count[row] == 0) {
        continue;
      }
      have = inventoryItemCount(player, recipe->ingredient_item[row]);
      y = 188 + row * 10;
      setHudTextColor(have >= recipe->ingredient_count[row] ?
        92 : 211, have >= recipe->ingredient_count[row] ? 214 : 79,
        have >= recipe->ingredient_count[row] ? 109 : 68);
      drawString(shortIngredientName(recipe->ingredient_item[row]), 202, y);
      x = drawUnsigned(have, 263, y);
      drawChar('/', x, y);
      drawUnsigned(recipe->ingredient_count[row], x + charWidth('/'), y);
    }
  }

  setHudTextColor(242, 242, 233);
  if (player->inventory_area == INVENTORY_AREA_OUTPUT) {
    drawChar('A', 21, 213);
    drawString("CRAFT ONE", 36, 213);
    drawString("CU", 114, 213);
    drawString("CRAFT MAX", 137, 213);
    drawChar('B', 243, 213);
    drawString("BACK", 258, 213);
  } else {
    drawChar('A', 21, 194);
    drawString("MOVE STACK", 36, 194);
    drawChar('B', 118, 194);
    drawString("BACK", 133, 194);
    drawString("CL", 22, 214);
    drawString("ONE", 44, 214);
    drawString("CR", 97, 214);
    drawString("QUICK", 120, 214);
    drawString("CU", 182, 214);
    drawString("ALL", 205, 214);
    drawString("CD", 252, 214);
    drawString("DROP", 275, 214);
  }
  setHudTextColor(255, 255, 255);
}

static u32 hudStringWidth(const char *text) {
  u32 width = 0;

  while (*text) {
    width += charWidth(*text);
    text++;
  }
  return width;
}

/*
 * Streaming state, on screen, because this console cannot be observed any
 * other way.  A vertical stack on the left edge, clear of the compass and
 * the objective panel.  Top to bottom:
 *
 *   F  frame heartbeat (mod 1000) -- frozen F means no frames are being
 *      built at all; a ticking F over a stalled world means the pipeline is
 *      alive and the fault is in streaming logic
 *   O  times the render origin has rebased
 *   R  columns resident in the window
 *   D  of those, fully decorated and therefore eligible to be meshed
 *   Q  columns queued for mesh compilation
 *   A  per-cent of the mesh arena still free
 *   C  0 idle, 1 compacting, 2 the last pass could not fit every column
 *   T  ring columns with no terrain yet (reads as solid invisible walls)
 *
 * FPS, beside the W row, is frames displayed in the last whole second -- the
 * rate the player sees, counted at the point of submission rather than
 * inferred from the callback interval.  Read it against W and B: a low FPS
 * with W and B both near 166 is the RSP/RDP being the bottleneck, since a
 * callback that finds a task still in flight returns early and never widens W.
 *
 * The failure this exists to catch is A pinned low with C stuck at 1: the
 * resident set is too large to fit after compaction, so it compacts forever
 * and Q never drains.  Terrain is then generated and walkable but never drawn.
 */
static u32 diag_frame_heartbeat;

static u32 drawDiagnosticRow(const char *label, u32 value, u32 y) {
  drawString(label, 6, y);
  drawUnsigned(value, 16, y);
  return y + 11;
}

static void drawStreamingDiagnostics() {
  u32 slot;
  u32 resident = 0, decorated = 0, queued = 0, pending_terrain = 0;
  u32 free_percent;
  u32 y = 6;

  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    if (windowSlotResident(slot)) {
      resident++;
      if (worldColumnState(windowSlotChunkX(slot), windowSlotChunkZ(slot)) ==
          COLUMN_DECORATED) {
        decorated++;
      }
    }
    if (dirty_columns[slot]) {
      queued++;
    }
  }
  {
    int pcx = floor(players[0].position.x / (BLOCK_SIZE * CHUNK_SIZE));
    int pcz = floor(players[0].position.z / (BLOCK_SIZE * CHUNK_SIZE));
    int dx, dz;

    for (dx = -STREAM_TERRAIN_RADIUS; dx <= STREAM_TERRAIN_RADIUS; dx++) {
      for (dz = -STREAM_TERRAIN_RADIUS; dz <= STREAM_TERRAIN_RADIUS; dz++) {
        if (worldColumnState(pcx + dx, pcz + dz) < COLUMN_TERRAIN) {
          pending_terrain++;
        }
      }
    }
  }
  free_percent = (MESH_ARENA_SIZE - mesh_arena_used) / (MESH_ARENA_SIZE / 100);
  diag_frame_heartbeat++;

  setHudTextColor(255, 220, 120);
  /* Player block position, so a freeze can be reported with its distance.
     drawUnsigned cannot show a sign; west/north of origin reads as the
     magnitude, which is enough for bisection. */
  {
    int px = floor(players[0].position.x / BLOCK_SIZE);
    int pz = floor(players[0].position.z / BLOCK_SIZE);

    y = drawDiagnosticRow("X", (u32) (px < 0 ? -px : px), y);
    y = drawDiagnosticRow("Z", (u32) (pz < 0 ? -pz : pz), y);
  }
  y = drawDiagnosticRow("F", diag_frame_heartbeat % 1000, y);
  y = drawDiagnosticRow("O", render_rebase_count, y);
  /* M near 6656 or V above 0 means the frame list overran its buffer. */
  y = drawDiagnosticRow("M", frame_command_peak, y);
  y = drawDiagnosticRow("V", frame_overflows, y);
  y = drawDiagnosticRow("R", resident, y);
  y = drawDiagnosticRow("D", decorated, y);
  y = drawDiagnosticRow("Q", queued, y);
  y = drawDiagnosticRow("A", free_percent, y);
  /* Once the boundary marcher fires, S is the swept-frame speed in world
     units and N is its final candidate boundary time in thousandths.  They
     borrow the two least relevant rows during this focused reproduction. */
  y = drawDiagnosticRow(diag_ray_clamps ? "S" : "C",
    diag_ray_clamps ? diag_ray_guard_speed : mesh_block_count, y);
  y = drawDiagnosticRow("T", pending_terrain, y);
  /* Worst frame gap and worst gated-CPU cost in the window, in tenths of a
     millisecond: W 166 is a clean 60 Hz frame, W 1000 is 10 fps.  B close to
     W blames the CPU work in the callback; W high with B low blames the
     RSP/RDP.  L counts runaway-loop guard trips in player collision code. */
  /* Frames displayed in the last whole second, riding the W line: it is the
     other frame-pacing number, and every row slot down to the hotbar is
     spoken for.  x 60 clears W's widest five-digit value at x 16, and this
     line sits above the C-button guide. */
  drawString("FPS", 60, y);
  drawUnsigned(diag_fps, 84, y);
  y = drawDiagnosticRow("W", diag_worst_frame_usec / 100, y);
  /* The active LOD/visibility preset, promote radius * 1000 + solo visible
     cap; Z + C-left cycles it.  Beside the B row the way FPS rides the W
     row -- a row of its own at the stack's foot sat in CRT overscan, where
     it read as a knob that never moved. */
  drawString("E", 60, y);
  drawUnsigned((u32) mesh_lod_promote_radius * 1000 +
    solo_max_visible_columns, 72, y);
  y = drawDiagnosticRow("B", diag_worst_gated_usec / 100, y);
  y = drawDiagnosticRow("L", diag_loop_clamps, y);
  y = drawDiagnosticRow("G", diag_position_glitches, y);
  /* Corrupt window keys caught and repaired.  Any non-zero K is the run-6
     guTranslate fault being absorbed; the first bad key's bits are in the
     freeze report. */
  y = drawDiagnosticRow(diag_ray_clamps ? "N" : "K",
    diag_ray_clamps ? diag_ray_guard_time : window_key_faults, y);
  /* Fog start (screen depth), 0 when fog is toggled off.  Tune with
     Z + D-pad Left/Right; Z + D-pad Down toggles. */
  y = drawDiagnosticRow("P", fog_enabled ? fog_start : 0, y);
  /* Cast-shadow casters drawn this frame, capped at SHADOW_SLOTS.  The pass
     costs eight triangles and some translucent fill per caster, so S is the
     number to watch if the sky work ever shows up in W or B. */
  drawDiagnosticRow("S", shadow_count, y);
  setHudTextColor(255, 255, 255);
}

static void drawGameText() {
  u8 player_num;

  beginText();
  /* An anomaly turns the overlay on by itself: the whole point of the
     self-healing guards is that they leave a visible confession. */
  if (!diagnostics_visible &&
      (diag_task_hung || diag_cpu_faulted || window_key_faults != 0 ||
       diag_position_glitches != 0)) {
    diagnostics_visible = TRUE;
  }
  if (active_player_count == 1 && diagnostics_visible) {
    drawStreamingDiagnostics();
  }
  /* The HUD's fills were skipped for the battle above; skip its labels too,
     or the objective panel's text is left floating with no panel under it. */
  for (player_num = 0; player_num < active_player_count &&
      !mon64BattleActive(); player_num++) {
    u32 x_offset = playerViewportX(player_num);
    u32 y_offset = playerViewportY(player_num);
    u32 viewport_height = playerViewportHeight();
    u32 bar_width = HOTBAR_SLOT_COUNT * HOTBAR_SLOT_SIZE;
    u32 bar_x;
    u32 bar_y;
    ItemStack *held_stack = &players[player_num].inventory[
      INVENTORY_HOTBAR_START + players[player_num].selected_hotbar_slot];
    u8 slot;

    if (usesFourPlayerLayout()) {
      bar_width = HOTBAR_SLOT_COUNT * 14;
      bar_x = x_offset + (playerViewportWidth() - bar_width) / 2;
      bar_y = y_offset + viewport_height - 14 - 4;
      drawChar('P', x_offset + 5, y_offset + 5);
      drawChar('1' + player_num, x_offset + 12, y_offset + 5);
    } else {
      const char *held_name = itemName(held_stack->count > 0 ?
        held_stack->item : AIR);

      bar_x = (SCREEN_WD - bar_width) / 2;
      bar_y = y_offset + viewport_height - HOTBAR_SLOT_SIZE - HOTBAR_MARGIN;
      if (active_player_count > 1) {
        drawChar('P', 6, y_offset + 6);
        drawChar('1' + player_num, 13, y_offset + 6);
      } else if (players[player_num].camera_mode == CAMERA_THIRD_PERSON) {
        setHudTextColor(241, 195, 58);
        drawString("3P", 7, 8);
      }
      if (pickup_message[player_num] > 0) {
        setHudTextColor(105, 225, 125);
        drawChar('+', 7, y_offset + (active_player_count > 1 ? 16 : 9));
        drawString(itemName(pickup_item[player_num]), 15,
          y_offset + (active_player_count > 1 ? 16 : 9));
      }
      if (active_player_count == 1) {
        u32 name_width;

        drawCompassLabels(&players[player_num]);
        setHudTextColor(241, 195, 58);
        drawString(playerObjectiveTitle(&players[player_num]),
          players[player_num].objective_time > 0 ? 181 : 197, 12);
        if (players[player_num].objective_time > 0) {
          setHudTextColor(224, 228, 219);
          drawString(playerObjectiveHint(&players[player_num]), 181, 23);
          drawString("UP CAMERA", 38, 164);
          drawString("LR ITEMS", 38, 174);
          drawString("DN PACK", 38, 184);
          drawChar('A', 269, 168);
          drawString("USE", 282, 168);
          drawChar('B', 269, 179);
          drawString("MINE", 282, 179);
          drawChar('R', 269, 188);
          drawString("JUMP", 282, 188);
        }
        if (held_stack->count > 0) {
          name_width = hudStringWidth(held_name);
          setHudTextColor(239, 239, 230);
          drawString(held_name, (SCREEN_WD - name_width) / 2, 190);
        }
      }
    }
    for (slot = 0; slot < HOTBAR_SLOT_COUNT; slot++) {
      ItemStack *stack = &players[player_num].inventory[
        INVENTORY_HOTBAR_START + slot];
      /* The 5x7 `1` glyph reads as a white underline at hotbar scale.
         Single items do not need a count label; retain labels only when a
         stack contains more than one item. */
      if (stack->count > 1) {
        u32 slot_size = usesFourPlayerLayout() ? 14 : HOTBAR_SLOT_SIZE;
        drawStackCount(stack, bar_x + slot * slot_size, bar_y, slot_size);
      }
    }
  }
  /* A refused edit is otherwise indistinguishable from a dropped controller
     input: the block simply does not appear.  Say which of the two it was.
     Drawn once rather than per viewport -- the pool is shared, so in co-op it
     is one world's limit and not one player's. */
  if (world_edit_full_message > 0) {
    static const char *full_text = "BUILD LIMIT REACHED";

    setHudTextColor(238, 120, 96);
    drawString(full_text, (SCREEN_WD - hudStringWidth(full_text)) / 2, 178);
  }
  setHudTextColor(255, 255, 255);
}

void drawHUD() {
  u8 player_num;

  if (current_screen != GAME && current_screen != INVENTORY &&
      !screenShowsPreview(current_screen)) {
    clearBuffers(GPACK_RGBA5551(0, 0, 0, 1));
  } else if (current_screen == LOADING_PREVIEW) {
    drawLoadingOverlay();
  } else if (screenIsWorldPicker(current_screen)) {
    /* The carousel has its own text overlay; never let the inventory panel
       fall through on top of its world preview.  Every other branch here ends
       by draining the pipe before drawMenu reconfigures the RDP for text --
       this one must too. */
    gDPPipeSync(dlp++);
  } else if (current_screen == GAME) {
    loaded_texture = NULL;
    /* World and targeting passes leave the scissor on their final player
       viewport.  HUD primitives use absolute framebuffer coordinates, so
       reset it before drawing every player's overlay. */
    gDPSetScissor(dlp++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WD, SCREEN_HT);
    /* A battle takes the screen.  The crosshair, hotbar, compass and hint
       panels all describe things nobody can do while one is running, and
       they would sit behind the battle's own panels saying so. */
    for (player_num = 0; player_num < active_player_count &&
        !mon64BattleActive(); player_num++) {
      u32 crosshair_x = playerViewportX(player_num) + playerViewportWidth() / 2;
      u32 crosshair_y = playerViewportY(player_num) + playerViewportHeight() / 2;
      drawCrosshair(crosshair_x, crosshair_y, &players[player_num]);
      drawBreakProgress(crosshair_x, crosshair_y, &players[player_num]);
      drawHotbar(player_num);
      drawHealth(player_num);
      if (active_player_count == 1) {
        drawCompass(&players[player_num]);
        drawObjectivePanel(&players[player_num]);
        drawCButtonGuide(&players[player_num]);
        drawActionGuide(&players[player_num]);
      }
    }
    if (active_player_count > 1) {
      gDPSetCycleType(dlp++, G_CYC_FILL);
      gDPSetFillColor(dlp++, (GPACK_RGBA5551(0, 0, 0, 1) << 16 | GPACK_RGBA5551(0, 0, 0, 1)));
      gDPFillRectangle(dlp++, 0, SCREEN_HT / 2 - 1, SCREEN_WD - 1, SCREEN_HT / 2);
      if (usesFourPlayerLayout()) {
        gDPFillRectangle(dlp++, SCREEN_WD / 2 - 1, 0, SCREEN_WD / 2, SCREEN_HT - 1);
      }
      gDPPipeSync(dlp++);
    }
    gDPPipeSync(dlp++);
  } else {
    /* Inventory input pauses movement, so redrawing the 3D world beneath a
     * mostly opaque panel only burns RSP/RDP time.  A stable dark backdrop is
     * also safe with NuSystem's rotating triple framebuffer. */
    clearBuffers(GPACK_RGBA5551(10, 16, 28, 1));
    loaded_texture = NULL;
    drawInventory();
    gDPPipeSync(dlp++);
  }
  drawMenu();
  if (current_screen == GAME) {
    drawGameText();
    /*
     * The creature interface goes last, over everything.  A battle owns the
     * screen, and the encounter prompt has to sit above the hotbar rather
     * than under it.  Both leave the RDP configured for text, which is what
     * every branch above them also leaves behind.
     */
    if (mon64BattleActive()) {
      mon64DrawBattleInterface();
    } else {
      for (player_num = 0; player_num < active_player_count; player_num++) {
        mon64DrawPrompt(player_num,
          playerViewportX(player_num) + playerViewportWidth() / 2,
          playerViewportY(player_num) + playerViewportHeight() - 34);
      }
    }
  } else if (current_screen == INVENTORY) {
    /* drawInventory may leave an item preview bound; restore the font before
       issuing count glyph rectangles. */
    beginText();
    drawInventoryStackCounts();
    drawInventoryText();
  }

}

/*
 * Slide fog_start toward wherever the view currently meets the void.
 *
 * Culling reported each viewer's nearest unmeshed frustum cell last frame;
 * the target start puts the band's fully opaque end at that distance, so
 * the frontier and its pop-in stay behind haze while everything meshed
 * stays clear.  With no hole in any view the target is the parked maximum,
 * where the band lies beyond the mesh ring and costs nothing.
 *
 * Runs before this frame's culling so the NEAR/FAR classification and the
 * drawn band always agree; the hole data being one frame old only matters
 * for one frame of slide.  Tighten fast (a fresh void should be covered
 * within a second) and relax slowly (meshed-in terrain fades clear instead
 * of snapping).
 */
static void updateAutoFog(void) {
  float nearest = VISIBLE_HOLE_NONE;
  u16 target = FOG_START_MAX;
  static u8 relax_divider;
  u8 i;

  for (i = 0; i < active_player_count; i++) {
    if (visible_hole_sq[i] < nearest) {
      nearest = visible_hole_sq[i];
    }
  }
  /*
   * A distant hole does not summon the fog.  Screen depth is so compressed
   * that "fully opaque at 60 blocks" drags the haze onset into the
   * mid-view at ~23 -- which read as fog rolling over a fully built,
   * beautiful area because somewhere far ahead the ring's leading edge had
   * not meshed yet.  A far hole is a few indistinct pixels on a CRT, and
   * the frontier wall in player.c keeps anyone from standing next to raw
   * void; only a hole close enough to actually read as one engages the
   * band.
   */
#define FOG_ENGAGE_BLOCKS 44.f
  if (nearest > FOG_ENGAGE_BLOCKS * FOG_ENGAGE_BLOCKS) {
    nearest = VISIBLE_HOLE_NONE;
  }
  if (nearest < VISIBLE_HOLE_NONE) {
    /* v(d), the screen-depth mapping documented over fog_start, at the
       co-op or solo far plane; minus the band, so the band *ends* at the
       hole.  The distance floor keeps a hole at the player's feet -- a
       fresh spawn, an urgent stream gap -- from fogging the screen white. */
    float far_plane = active_player_count > 1 ? 8000.f : 14000.f;
    float d = sqrtf(nearest);
    float v;

    if (d < 14.f) {
      d = 14.f;
    }
    v = 1000.f * far_plane * (64.f * d - 10.f) /
      (64.f * d * (far_plane - 10.f));
    v -= FOG_BAND;
    v += fog_auto_bias;
    if (v < FOG_START_MIN) {
      v = FOG_START_MIN;
    }
    if (v > FOG_START_MAX) {
      v = FOG_START_MAX;
    }
    target = (u16) v;
  }

  if (target < fog_start) {
    fog_start -= fog_start - target > 6 ? 3 : 1;
  } else if (target > fog_start && ++relax_divider >= 4) {
    relax_divider = 0;
    fog_start++;
  }
}

void draw(int can_reclaim_mesh_arena) {
  Gfx *frame_start = &frame_display_lists[dl_no][0];

  loaded_texture = NULL;
  dlp = frame_start;
  /* Leave room for every pass that runs after the terrain branches: HUD,
     menu text, and the trailing sync. */
  frame_dlp_limit = frame_start + FRAME_DISPLAY_LIST_SIZE -
    FRAME_COMMAND_TAIL_RESERVE;
  gSPDisplayList(dlp++, setup_display_list);

  if (current_screen == GAME) {
    u8 player_num;
    if (fog_enabled) {
      updateAutoFog();
    }
    for (player_num = 0; player_num < active_player_count; player_num++) {
      updateVisibleColumns(player_num);
      updateCameraMatrices(player_num);
    }
    processColumnDisplayListUpdates(can_reclaim_mesh_arena);
    drawWorld();
    drawWireframes();
  } else if (current_screen == LOADING_PREVIEW) {
    updateLoadingCamera();
    processColumnDisplayListUpdates(can_reclaim_mesh_arena);
    drawWorld();
  } else if (screenIsWorldPicker(current_screen)) {
    /* Keep orbiting the mesh that is already resident.  A requested preview
       has not touched the arena yet -- callbackGfx rebuilds it only with no
       task in flight -- so the previous slot stays on screen right up to the
       swap instead of blanking for the duration of the build.

       The scenic mesh never changes while the picker is open, so it needs no
       incremental rebuild or compaction pass. */
    updateLoadingCamera();
    drawWorld();
  }
  drawHUD();

  /* High-water mark for the M diagnostics row.  drawTextured sheds terrain
     at frame_dlp_limit, but everything after it -- wireframes, entities, the
     whole HUD, these diagnostics -- writes into the tail reserve unchecked.
     If M ever approaches FRAME_DISPLAY_LIST_SIZE (6656), commands have been
     written past the buffer into adjacent memory: silent corruption that
     would surface as exactly the kind of wandering freeze being hunted. */
  if ((u32) (dlp - frame_start) > frame_command_peak) {
    frame_command_peak = (u32) (dlp - frame_start);
  }

  if (dlp > frame_start + FRAME_DISPLAY_LIST_SIZE - 2) {
    /*
     * Every variable pass reserves space before drawing, but keep the release
     * build fail-safe too: submit a harmless empty frame rather than hand the
     * RSP a list that has already run past its buffer.  assert() is compiled
     * out by -DNDEBUG, so it cannot be the only thing standing here.
     */
    frame_overflows++;
    dlp = frame_start;
  }
  gDPFullSync(dlp++);
  gSPEndDisplayList(dlp++);
  assert(dlp - frame_start <= FRAME_DISPLAY_LIST_SIZE);
  nuGfxTaskStart(frame_start,
    (s32)(dlp - frame_start) * sizeof (Gfx),
    NU_GFX_UCODE_F3DEX, NU_SC_SWAPBUFFER);
  /* The one place a frame is handed to the RSP, and so the only honest place
     to count displayed frames from. */
  diagNoteFrameSubmitted();

  /* Switch display list buffers */
  dl_no ^= 1;
}

void initGraphics() {
  int x, z;

  nuGfxInit();
  nuGfxDisplayOn();

  buildFaceTextureTable();

  /* The shadow pass draws origin-relative world coordinates straight through,
     so its modelview is an identity -- written once here, because a matrix an
     in-flight task may still be reading must never be rewritten. */
  guTranslate(&shadow_model, 0.f, 0.f, 0.f);

  /* Chunk translations are no longer prebaked here.  A slot's matrices belong
     to whichever column is bound to it, so makeColumnDisplayLists writes them
     as it compiles that column.  Until the first world is built there is no
     terrain to draw, and nothing reads them. */

  for (x = 0; x < CHUNK_SIZE; x++) {
    for (z = 0; z < CHUNK_SIZE; z++) {
      Vtx *vertices = WATER_TOP_QUAD_ADDR(x, z);
      s16 width = (x + 1) * BLOCK_SIZE;
      s16 depth = (z + 1) * BLOCK_SIZE;
      s16 texture_width = (x + 1) * 1024;
      s16 texture_depth = (z + 1) * 1024;

      setWaterTopVertex(&vertices[0], 0, 0, 0, 0);
      setWaterTopVertex(&vertices[1], width, 0, texture_width, 0);
      setWaterTopVertex(&vertices[2], width, depth, texture_width,
        texture_depth);
      setWaterTopVertex(&vertices[3], 0, depth, 0, texture_depth);
    }
  }

  /* Point every column at the shared empty list before the first world is
     compiled.  drawTextured branches to these pointers unconditionally, and
     they are still NULL from BSS -- any terrain draw before the first build
     would send the RSP to address zero. */
  {
    u8 texture;
    u16 slot;

    for (texture = 0; texture < NUM_TEXTURES; texture++) {
      for (slot = 0; slot < WINDOW_SLOTS; slot++) {
        column_starts[texture][slot] = empty_column_display_list;
        staged_starts[texture][slot] = empty_column_display_list;
      }
    }
  }
  mesh_block_count = 0;
  mesh_arena_used = 0;
  mesh_generation = 0;
  mesh_building_generation = 0;
}
