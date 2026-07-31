#include <nusys.h>
#include <assert.h>
#include "graphics.h"
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
#define CELESTIAL_DISTANCE_SOLO 11000.f
#define CELESTIAL_DISTANCE_COOP 7000.f
#define SUN_SIZE 430.f
#define MOON_SIZE 360.f
#define DROPPED_ITEM_RENDER_DISTANCE (BLOCK_SIZE * 36.f)
#define PLAYER_RENDER_DISTANCE (BLOCK_SIZE * 64.f)
#define MOB_RENDER_DISTANCE (BLOCK_SIZE * 30.f)
#define MAX_VISIBLE_MOBS 4

Gfx *dlp;
u32 dl_no = 0;
Gfx frame_display_lists[NUM_DISPLAY_LISTS][FRAME_DISPLAY_LIST_SIZE];

/* Keep a complete, immutable mesh arena on screen while a replacement is
 * compacted incrementally into the other arena.  This eliminates the old
 * gameplay-time RSP wait and full-world rebuild when append-only updates ran
 * low on space. */
#define NUM_COLUMN_ARENAS 2
/*
 * One column a frame was sized for the occasional block edit in a world that
 * never moved.  Streaming queues a whole row of columns every time the player
 * crosses a chunk boundary, and at one a frame that is most of a second of
 * terrain trailing behind them.
 *
 * Compaction gets its own, larger budget because it blocks everything: while
 * it runs no queued column is compiled at all, so the player sees new terrain
 * simply stop arriving until it finishes.  Most of the slots it walks are not
 * finished columns and cost nothing to skip.
 */
#define MESH_REBUILD_BUDGET 3
/*
 * Compaction budgets count what they cost, not slots.  The old single budget
 * of six charged empty slots and full column rebuilds alike, so a frame that
 * happened upon six finished ring columns did six greedy meshes plus seam
 * refinement in one callback -- tens of milliseconds of hitch -- while frames
 * over empty window space did nothing and still only advanced six slots.
 * Charging rebuilds and slot walking separately bounds the worst frame at two
 * rebuilds without making the pass take longer over the empty stretches.
 */
#define MESH_COMPACTION_REBUILDS_PER_FRAME 2
#define MESH_COMPACTION_SLOTS_PER_FRAME 64
#define MESH_COMPACTION_RESERVE (DISPLAY_LIST_SIZE / 10)
/*
 * Frames to wait after a compaction publishes before another may start.  A
 * pass that ran short proved the resident working set does not fit, and a
 * pass that completed with the arena still under reserve proved the same
 * thing; recompacting either immediately reclaims nothing and costs the whole
 * pass again, forever -- the game keeps running but new terrain never gets a
 * turn to compile.  The pause gives the ring time to move on and the failure
 * a shape the player survives: some far columns arrive late instead of the
 * console silently spending every frame remeshing.
 */
#define MESH_COMPACTION_COOLDOWN_FRAMES 120
/* Headroom the terrain passes leave for the HUD, menu text and trailing sync. */
#define FRAME_COMMAND_TAIL_RESERVE 2048
static Gfx column_display_lists[NUM_COLUMN_ARENAS][DISPLAY_LIST_SIZE];
Gfx *column_dlp;
/*
 * Per-column tables are indexed by window slot, not by a position in a fixed
 * world.  A slot is the only name a column still has once the world stops
 * having edges, and it is what stays valid when the window scrolls and rebinds
 * the slot to a different column.
 */
static Gfx *column_starts[NUM_COLUMN_ARENAS][NUM_TEXTURES][WINDOW_SLOTS];
static Gfx *column_arena_ends[NUM_COLUMN_ARENAS];
static u8 active_column_arena;
/* Hard ceiling for terrain branches within one frame list.  Release builds
   compile out assert(), so an overflow here would silently run past this
   frame's buffer into the next one and hand the RSP a corrupt list -- which
   presents as a console that renders correctly and then hangs. */
static Gfx *frame_dlp_limit;
/* Non-zero means a frame had to shed terrain or was dropped outright.  Shown
   on the picker so this failure mode can never be silent again. */
u32 frame_overflows;
/* Set while compiling the reduced scenic mesh; makeQuadDL drops everything
   below the surface shell. */
static u8 building_surface_mesh;
/* Surface height of each block column in the footprint being compiled, so the
   scenic pass resolves it once per column instead of rescanning the full
   height for every candidate quad. */
static u8 surface_heights[CHUNK_SIZE][CHUNK_SIZE];
static u8 column_build_arena;
static u8 column_display_list_full;
static u8 dirty_columns[WINDOW_SLOTS];
static u16 dirty_column_cursor;
/* The column makeColumnDisplayLists is compiling, for the parts of quad
   emission that need to know where in the world they are. */
static int building_column_x;
static int building_column_z;
static u8 compacting_columns;
static u16 compaction_column_cursor;
/* Set when a compaction pass could not fit every resident column. */
static u8 compaction_ran_short;
/* Frames remaining before another compaction may start; see the cooldown
   constant above for why publishing one always arms this. */
static u8 compaction_cooldown;
/* Whether a slot has compiled geometry, tracked per arena.  A column can be
   finished generating yet have no mesh -- it drifted out of the mesh ring and
   was dropped by a compaction, or its slot was invalidated.

   Per arena is load-bearing: a compaction builds into the arena that is off
   screen, and while it runs the on-screen arena still has every mesh it ever
   had.  A single shared array cleared at compaction start told the streaming
   ring scan that every column had lost its mesh, so it queued the whole ring
   dirty; those rebuilds then re-orphaned most of the freshly compacted arena,
   dragging it straight back under its reserve -- a compaction that feeds
   itself forever, on the exact long walks streaming exists to serve. */
static u8 column_meshed[NUM_COLUMN_ARENAS][WINDOW_SLOTS];

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

static void diagPaintBuffer(u16 *frame_buffer, u16 color) {
  int x, y;

  if (frame_buffer == NULL) {
    return;
  }
  for (y = 0; y < DIAG_SQUARE_SIZE; y++) {
    u16 *row = frame_buffer + (DIAG_SQUARE_Y + y) * SCREEN_WD + DIAG_SQUARE_X;
    for (x = 0; x < DIAG_SQUARE_SIZE; x++) {
      row[x] = color;
    }
    /* The VI scans RDRAM; flush each painted row out of the data cache. */
    osWritebackDCache(row, DIAG_SQUARE_SIZE * sizeof (u16));
  }
}

void diagPaintPhase(u16 color) {
  diag_current_phase = color;
  if (diag_task_hung) {
    return;
  }
  diagPaintBuffer((u16 *) osViGetCurrentFramebuffer(), color);
}

/* Called by the watchdog thread after the heartbeat stops: keep the fatal
   phase colour on screen no matter which buffer the VI ends up scanning. */
void diagPaintStalePhase(void) {
  u16 color = diag_task_hung ? GPACK_RGBA5551(255, 0, 0, 1)
    : diag_current_phase;

  diagPaintBuffer((u16 *) osViGetCurrentFramebuffer(), color);
  diagPaintBuffer((u16 *) osViGetNextFramebuffer(), color);
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

#define BLOCKS_PER_CHUNK (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)

/*
 * One translation per chunk, indexed by the window slot its column occupies.
 * These used to be prebaked once in initGraphics for a world whose chunks each
 * had a permanent address; a slot outlives the column in it, so a slot's
 * matrices are rewritten whenever the column bound to it is compiled.
 */
static Mtx c_models[WINDOW_SLOTS][CHUNKS_Y];
static Mtx b_models[CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE];

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

#define STEVE_VERTEX(x, y, z, r, g, b) {x, y, z, 0, 0, 0, r, g, b, 255}

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
  STEVE_VERTEX(-10, 9, 9, 105, 211, 96), STEVE_VERTEX(10, 9, 9, 105, 211, 96),
  STEVE_VERTEX(10, -9, 9, 105, 211, 96), STEVE_VERTEX(-10, -9, 9, 105, 211, 96),
  STEVE_VERTEX(10, 9, -9, 48, 136, 60), STEVE_VERTEX(-10, 9, -9, 48, 136, 60),
  STEVE_VERTEX(-10, -9, -9, 48, 136, 60), STEVE_VERTEX(10, -9, -9, 48, 136, 60)
};

/* Two blue eye quads on the local -Z face make it obvious where Steve is
   looking, even without a character texture. */
static Vtx steve_eye_verts[] = {
  STEVE_VERTEX(-11, 8, -17, 55, 125, 210), STEVE_VERTEX(-5, 8, -17, 55, 125, 210),
  STEVE_VERTEX(-5, 2, -17, 55, 125, 210), STEVE_VERTEX(-11, 2, -17, 55, 125, 210),
  STEVE_VERTEX(5, 8, -17, 55, 125, 210), STEVE_VERTEX(11, 8, -17, 55, 125, 210),
  STEVE_VERTEX(11, 2, -17, 55, 125, 210), STEVE_VERTEX(5, 2, -17, 55, 125, 210)
};

/* The blade points down from the hand pivot.  Keeping it as two shaded boxes
   gives every equipped sword a crisp silhouette without adding a texture or
   a costly animated skeleton. */
static Vtx steve_sword_blade_verts[] = {
  STEVE_VERTEX(-4, 4, 3, 205, 205, 188), STEVE_VERTEX(4, 4, 3, 205, 205, 188),
  STEVE_VERTEX(4, -42, 3, 205, 205, 188), STEVE_VERTEX(-4, -42, 3, 205, 205, 188),
  STEVE_VERTEX(4, 4, -3, 165, 165, 154), STEVE_VERTEX(-4, 4, -3, 165, 165, 154),
  STEVE_VERTEX(-4, -42, -3, 165, 165, 154), STEVE_VERTEX(4, -42, -3, 165, 165, 154)
};

static Vtx wood_sword_blade_verts[] = {
  STEVE_VERTEX(-4, 4, 3, 151, 103, 53), STEVE_VERTEX(4, 4, 3, 151, 103, 53),
  STEVE_VERTEX(4, -42, 3, 151, 103, 53), STEVE_VERTEX(-4, -42, 3, 151, 103, 53),
  STEVE_VERTEX(4, 4, -3, 105, 66, 31), STEVE_VERTEX(-4, 4, -3, 105, 66, 31),
  STEVE_VERTEX(-4, -42, -3, 105, 66, 31), STEVE_VERTEX(4, -42, -3, 105, 66, 31)
};

static Vtx stone_sword_blade_verts[] = {
  STEVE_VERTEX(-4, 4, 3, 151, 157, 154), STEVE_VERTEX(4, 4, 3, 151, 157, 154),
  STEVE_VERTEX(4, -42, 3, 151, 157, 154), STEVE_VERTEX(-4, -42, 3, 151, 157, 154),
  STEVE_VERTEX(4, 4, -3, 94, 100, 98), STEVE_VERTEX(-4, 4, -3, 94, 100, 98),
  STEVE_VERTEX(-4, -42, -3, 94, 100, 98), STEVE_VERTEX(4, -42, -3, 94, 100, 98)
};

static Vtx steve_sword_hilt_verts[] = {
  STEVE_VERTEX(-12, 7, 5, 142, 83, 38), STEVE_VERTEX(12, 7, 5, 142, 83, 38),
  STEVE_VERTEX(12, 1, 5, 142, 83, 38), STEVE_VERTEX(-12, 1, 5, 142, 83, 38),
  STEVE_VERTEX(12, 7, -5, 104, 58, 27), STEVE_VERTEX(-12, 7, -5, 104, 58, 27),
  STEVE_VERTEX(-12, 1, -5, 104, 58, 27), STEVE_VERTEX(12, 1, -5, 104, 58, 27)
};

/* Pickaxes and axes share one low-poly handle, but keep distinct heads so
   every tool reads immediately both in first person and while spinning as a
   pickup.  Wood and stone tiers use different shaded head geometry. */
static Vtx tool_handle_verts[] = {
  STEVE_VERTEX(-3, 8, 3, 145, 91, 43), STEVE_VERTEX(3, 8, 3, 145, 91, 43),
  STEVE_VERTEX(3, -38, 3, 145, 91, 43), STEVE_VERTEX(-3, -38, 3, 145, 91, 43),
  STEVE_VERTEX(3, 8, -3, 99, 57, 26), STEVE_VERTEX(-3, 8, -3, 99, 57, 26),
  STEVE_VERTEX(-3, -38, -3, 99, 57, 26), STEVE_VERTEX(3, -38, -3, 99, 57, 26)
};

#define TOOL_HEAD_VERTS(name, left, right, top, bottom, r1, g1, b1, r2, g2, b2) \
static Vtx name[] = { \
  STEVE_VERTEX(left, top, 4, r1, g1, b1), STEVE_VERTEX(right, top, 4, r1, g1, b1), \
  STEVE_VERTEX(right, bottom, 4, r1, g1, b1), STEVE_VERTEX(left, bottom, 4, r1, g1, b1), \
  STEVE_VERTEX(right, top, -4, r2, g2, b2), STEVE_VERTEX(left, top, -4, r2, g2, b2), \
  STEVE_VERTEX(left, bottom, -4, r2, g2, b2), STEVE_VERTEX(right, bottom, -4, r2, g2, b2) \
}

TOOL_HEAD_VERTS(wood_pick_head_verts, -21, 21, 11, 3,
  165, 111, 59, 109, 68, 32);
TOOL_HEAD_VERTS(stone_pick_head_verts, -21, 21, 11, 3,
  177, 181, 176, 111, 116, 113);
TOOL_HEAD_VERTS(iron_pick_head_verts, -21, 21, 11, 3,
  220, 223, 216, 156, 163, 160);
TOOL_HEAD_VERTS(wood_axe_head_verts, -17, 9, 13, -3,
  165, 111, 59, 109, 68, 32);
TOOL_HEAD_VERTS(stone_axe_head_verts, -17, 9, 13, -3,
  177, 181, 176, 111, 116, 113);
TOOL_HEAD_VERTS(iron_axe_head_verts, -17, 9, 13, -3,
  220, 223, 216, 156, 163, 160);

static Vtx coal_chunk_verts[] = {
  STEVE_VERTEX(-10, 10, 9, 47, 50, 51), STEVE_VERTEX(10, 10, 9, 47, 50, 51),
  STEVE_VERTEX(10, -10, 9, 47, 50, 51), STEVE_VERTEX(-10, -10, 9, 47, 50, 51),
  STEVE_VERTEX(10, 10, -9, 24, 26, 27), STEVE_VERTEX(-10, 10, -9, 24, 26, 27),
  STEVE_VERTEX(-10, -10, -9, 24, 26, 27), STEVE_VERTEX(10, -10, -9, 24, 26, 27)
};

static Vtx iron_chunk_verts[] = {
  STEVE_VERTEX(-11, 9, 8, 198, 145, 99), STEVE_VERTEX(11, 9, 8, 198, 145, 99),
  STEVE_VERTEX(11, -9, 8, 198, 145, 99), STEVE_VERTEX(-11, -9, 8, 198, 145, 99),
  STEVE_VERTEX(11, 9, -8, 129, 90, 64), STEVE_VERTEX(-11, 9, -8, 129, 90, 64),
  STEVE_VERTEX(-11, -9, -8, 129, 90, 64), STEVE_VERTEX(11, -9, -8, 129, 90, 64)
};

static Vtx apple_body_verts[] = {
  STEVE_VERTEX(-11, 10, 10, 218, 49, 42), STEVE_VERTEX(11, 10, 10, 218, 49, 42),
  STEVE_VERTEX(11, -10, 10, 218, 49, 42), STEVE_VERTEX(-11, -10, 10, 218, 49, 42),
  STEVE_VERTEX(11, 10, -10, 139, 25, 24), STEVE_VERTEX(-11, 10, -10, 139, 25, 24),
  STEVE_VERTEX(-11, -10, -10, 139, 25, 24), STEVE_VERTEX(11, -10, -10, 139, 25, 24)
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

static void setWorldLight(u8 player_num) {
  Lights1 *lights = &world_lights[dl_no][player_num];
  Vector3 direction = dayCycleLightDirection();

  setAmbientColor(&lights->a, dayCycleAmbientLight());
  setLightColor(&lights->l[0], dayCycleDirectLight());
  lights->l[0].l.dir[0] = direction.x * 127;
  lights->l[0].l.dir[1] = direction.y * 127;
  lights->l[0].l.dir[2] = direction.z * 127;
}

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

static void setCelestialVertex(Vtx *vertex, Vector3 point, u16 s, u16 t) {
  vertex->v.ob[0] = point.x;
  vertex->v.ob[1] = point.y;
  vertex->v.ob[2] = point.z;
  vertex->v.flag = 0;
  vertex->v.tc[0] = s;
  vertex->v.tc[1] = t;
  vertex->v.cn[0] = 255;
  vertex->v.cn[1] = 255;
  vertex->v.cn[2] = 255;
  vertex->v.cn[3] = 255;
}

static void makeCelestialQuad(Vtx *vertices, Vector3 center, Vector3 right,
    Vector3 up, float size) {
  Vector3 horizontal = mul(right, size);
  Vector3 vertical = mul(up, size);
  Vector3 top_left = add(add(center, vertical), mul(horizontal, -1.f));
  Vector3 top_right = add(add(center, vertical), horizontal);
  Vector3 bottom_right = add(add(center, mul(vertical, -1.f)), horizontal);
  Vector3 bottom_left = add(add(center, mul(vertical, -1.f)), mul(horizontal, -1.f));

  setCelestialVertex(&vertices[0], top_left, 0, 0);
  setCelestialVertex(&vertices[1], top_right, 16 << 5, 0);
  setCelestialVertex(&vertices[2], bottom_right, 16 << 5, 16 << 5);
  setCelestialVertex(&vertices[3], bottom_left, 0, 16 << 5);
}

static Texture *moonTexture() {
  static Texture *moon_textures[] = {
    &moon_0_texture, &moon_1_texture, &moon_2_texture, &moon_3_texture,
    &moon_4_texture, &moon_5_texture, &moon_6_texture, &moon_7_texture
  };
  return moon_textures[dayCycleMoonPhase()];
}

static void drawCelestialBodies(u8 player_num) {
  Player *player = &players[player_num];
  Vector3 sun = dayCycleSunDirection();
  Vector3 moon = mul(sun, -1.f);
  Vector3 camera = playerCameraPosition(player_num);
  Vector3 right = rotateY((Vector3) {1, 0, 0}, -player->yaw);
  Vector3 up = rotateY(rotateX((Vector3) {0, 1, 0}, player->pitch), -player->yaw);
  Vtx *vertices = celestial_verts[dl_no][player_num];
  float distance = active_player_count > 1 ? CELESTIAL_DISTANCE_COOP :
    CELESTIAL_DISTANCE_SOLO;

  gSPClearGeometryMode(dlp++, G_CULL_BACK | G_LIGHTING);
  gDPSetCombineMode(dlp++, G_CC_MODULATERGB, G_CC_MODULATERGB);
  gDPSetRenderMode(dlp++, G_RM_AA_ZB_TEX_EDGE, G_RM_AA_ZB_TEX_EDGE2);
  gDPSetAlphaCompare(dlp++, G_AC_THRESHOLD);

  if (sun.y >= 0) {
    makeCelestialQuad(vertices, add(camera, mul(sun, distance)),
      right, up, SUN_SIZE);
    loadTexture(&sun_texture);
    gSPVertex(dlp++, vertices, 4, 0);
    gSP1Quadrangle(dlp++, 0, 1, 2, 3, 0);
  }
  if (moon.y >= 0) {
    makeCelestialQuad(vertices + 4, add(camera, mul(moon, distance)),
      right, up, MOON_SIZE);
    loadTexture(moonTexture());
    gSPVertex(dlp++, vertices + 4, 4, 0);
    gSP1Quadrangle(dlp++, 0, 1, 2, 3, 0);
  }

  gDPSetAlphaCompare(dlp++, G_AC_NONE);
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

static Gfx *columnArenaStart(void) {
  return column_display_lists[column_build_arena];
}

static u32 columnArenaFree(void) {
  return columnArenaStart() + DISPLAY_LIST_SIZE - column_dlp;
}

static void beginColumnArenaBuild(u8 arena) {
  column_build_arena = arena;
  column_dlp = column_display_lists[arena];
  column_display_list_full = FALSE;
}

static void resetColumnArenaBuild(u8 arena) {
  u16 slot;
  u8 texture;

  beginColumnArenaBuild(arena);
  for (texture = 0; texture < NUM_TEXTURES; texture++) {
    for (slot = 0; slot < WINDOW_SLOTS; slot++) {
      column_starts[arena][texture][slot] = empty_column_display_list;
    }
  }
  /* Only this arena's meshes are discarded; the other arena -- usually the
     one on screen -- keeps every mesh it had, and its flags must keep saying
     so or the streaming ring scan re-queues the entire ring. */
  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    column_meshed[arena][slot] = FALSE;
  }
}

static void makeQuadDL(u32 slot, u8 cy, u8 bx, u8 by, u8 bz, u8 width,
    u8 height, u8 face, u8 is_water) {
  u32 b = bx * CHUNK_SIZE * CHUNK_SIZE + by * CHUNK_SIZE + bz;

  if (building_surface_mesh) {
    int cx = building_column_x;
    int cz = building_column_z;
    int max_quad_y = cy * CHUNK_SIZE + by;
    int surface_y;

    /*
     * The scenic camera looks down on the world from outside it, so every
     * cave wall and ore face in a visible column is transformed by the RSP
     * and then hidden behind the terrain above it.  Column culling cannot
     * reject those -- there is no occlusion pass -- and they dominate the
     * frame.  Keep only the two-block surface shell here; gameplay compiles
     * the untouched full mesh into the other arena.
     */
    /* The world's underground outer wall is one long greedy quad spanning
       bedrock to surface.  It is what produced the "chunks below the map"
       silhouette on the title screen. */
    if ((cx == 0 || cx == CHUNKS_X - 1 ||
         cz == 0 || cz == CHUNKS_Z - 1) &&
        face != TOP && face != BOTTOM) {
      return;
    }
    if (face == TOP) {
      max_quad_y++;
    } else if (face != BOTTOM) {
      max_quad_y += height + 1;
    }
    surface_y = surface_heights[bx][bz];
    if (max_quad_y < surface_y - 1 || face == BOTTOM) {
      return;
    }
  }

  /* Reserve one final command for the column's EndDisplayList.  A maximally
     fragmented player-built chunk can exceed the normal greedy-mesh budget;
     dropping only excess faces is far safer than overwriting adjacent RAM. */
  if (columnArenaFree() < 6) {
    column_display_list_full = TRUE;
    return;
  }
  gSPMatrix(column_dlp++,OS_K0_TO_PHYSICAL(&c_models[slot][cy]),
    G_MTX_MODELVIEW|G_MTX_LOAD|G_MTX_NOPUSH);
  gSPMatrix(column_dlp++,OS_K0_TO_PHYSICAL(b_models + b),
    G_MTX_MODELVIEW|G_MTX_MUL|G_MTX_NOPUSH);
  gSPVertex(column_dlp++, is_water && face == TOP ?
    WATER_TOP_QUAD_ADDR(width, height) : QUAD_ADDR(face, width, height), 4, 0);
  gSP1Quadrangle(column_dlp++, 3, 2, 1, 0, 0);
}

static void makeQuadDLRST(u32 slot, u8 cy, u8 br, u8 bs, u8 bt, u8 axes,
    u8 width, u8 height, u8 face, u8 is_water) {
  if (face == NONE) {
    return;
  }

  if (axes == ZXY) {
    makeQuadDL(slot, cy, bs, bt, br, width, height, face, is_water);
  } else if (axes == XZY) {
    makeQuadDL(slot, cy, br, bt, bs, width, height, face, is_water);
  } else if (axes == YXZ) {
    makeQuadDL(slot, cy, bs, br, bt, width, height, face, is_water);
  }
}

static void makeChunkAxisDL(DualQuadList *axis_quads, u32 slot, u8 cy, u8 axes,
    u8 face1, u8 face2, u8 block) {
  u8 br, i;
  DualQuadList *both_quads;
  for (br = 0; br < CHUNK_SIZE; br++) {
    both_quads = &(axis_quads)[br];
    for (i = 0; i < both_quads->n_front + both_quads->n_back; i++) {
      if (both_quads->quads[i].block == block) {
        makeQuadDLRST(slot, cy, br, both_quads->quads[i].bs,
          both_quads->quads[i].bt, axes,
          both_quads->quads[i].width, both_quads->quads[i].height,
          i < both_quads->n_front ? face1 : face2, block == WATER);
      }
    }
  }
}

static u8 makeColumnDL(u32 slot, u8 texture, Gfx **new_start) {
  u8 cy, i;
  Gfx *texture_start;
  ChunkQuads *c_quads;
  FaceSpec *faces = textures[texture]->faces;

  if (column_display_list_full ||
      columnArenaFree() < 1) {
    column_display_list_full = TRUE;
    return FALSE;
  }
  texture_start = column_dlp;

  for (cy = 0; cy < CHUNKS_Y; cy++) {
    c_quads = &column_quads[cy];

    for (i = 0; i < textures[texture]->n_faces; i++) {
      if (faces[i].sides) {
        makeChunkAxisDL(c_quads->z_quads, slot, cy, ZXY, FRONT, BACK, faces[i].block);
        makeChunkAxisDL(c_quads->x_quads, slot, cy, XZY, RIGHT, LEFT, faces[i].block);
      }

      if (faces[i].top || faces[i].bottom) {
        makeChunkAxisDL(c_quads->y_quads, slot, cy, YXZ, faces[i].top? TOP : NONE, faces[i].bottom? BOTTOM : NONE, faces[i].block);
      }
    }
  }

  if (column_display_list_full) {
    return FALSE;
  }

  if (column_dlp == texture_start) {
    /* Most columns use only a subset of the texture bank. Sharing one empty
       list saves an EndDisplayList command for every absent material. */
    *new_start = empty_column_display_list;
    return TRUE;
  }

  gSPEndDisplayList(column_dlp++);
  *new_start = texture_start;
  return TRUE;
}

static u8 makeColumnDisplayLists(int cx, int cz) {
  u8 texture, cy;
  u32 slot = WINDOW_SLOT(cx, cz);
  Gfx *column_start = column_dlp;
  Gfx *new_starts[NUM_TEXTURES];

  building_column_x = cx;
  building_column_z = cz;

  /* The slot may have held a different column until now, so its translations
     are rewritten here rather than prebaked once for a fixed world.  Doing it
     alongside the mesh keeps the two from ever describing different places.
     Origin-relative: the absolute coordinate no longer fits an s15.16 Mtx. */
  for (cy = 0; cy < CHUNKS_Y; cy++) {
    guTranslate(&c_models[slot][cy],
      (float) (cx * CHUNK_SIZE - render_origin_x) * BLOCK_SIZE,
      (float) cy * CHUNK_SIZE * BLOCK_SIZE,
      (float) (cz * CHUNK_SIZE - render_origin_z) * BLOCK_SIZE);
  }

  /* Do not publish any new pointer until every material in the replacement
     column fits. Previous RSP tasks can keep reading the old immutable lists,
     and an overflow can safely roll this unpublished append back. */
  if (building_surface_mesh) {
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
  makeColumnGeometry(cx, cz);
  for (texture = 0; texture < NUM_TEXTURES; texture++) {
    if (!makeColumnDL(slot, texture, &new_starts[texture])) {
      column_dlp = column_start;
      column_display_list_full = TRUE;
      return FALSE;
    }
  }
  for (texture = 0; texture < NUM_TEXTURES; texture++) {
    column_starts[column_build_arena][texture][slot] = new_starts[texture];
  }
  column_meshed[column_build_arena][slot] = TRUE;
  return TRUE;
}

static u8 mesh_build_active;
static u16 mesh_build_cursor;
static u8 mesh_build_arena;
static u8 mesh_build_surface_only;
static u8 mesh_build_complete;

/*
 * A complete mesh rebuild rewrites an arena the RSP executes terrain from, so
 * it may only run while no graphics task is in flight.  callbackGfx guarantees
 * that by stepping it only when NuSystem reports pendingGfx == 0, and by
 * submitting the next frame afterwards rather than before.
 *
 * Do not wait for the RSP here.  nuGfxTaskAllEndWait() busy-spins on
 * nuGfxTaskSpool, and this runs on the NuSystem graphics thread at priority
 * 50.  The completion that clears that counter is posted by the scheduler's
 * own graphics thread at priority 17, which can never preempt a busy-wait at
 * 50, so the wait deadlocks the console instead of ending.
 *
 * The build always targets the arena that is not on screen and publishes it
 * only when complete, so the outgoing world keeps rendering throughout.
 */
void beginWorldMeshBuild(u8 surface_only) {
  u16 slot;

  compacting_columns = FALSE;
  compaction_cooldown = 0;
  dirty_column_cursor = 0;
  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    dirty_columns[slot] = FALSE;
  }
  mesh_build_arena = active_column_arena ^ 1;
  mesh_build_surface_only = surface_only;
  mesh_build_cursor = 0;
  mesh_build_complete = TRUE;
  mesh_build_active = TRUE;
  resetColumnArenaBuild(mesh_build_arena);
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

/* Returns TRUE once the arena has been published. */
u8 stepWorldMeshBuild(u16 columns) {
  if (!mesh_build_active) {
    return TRUE;
  }
  /* resetColumnArenaBuild left column_dlp at the target arena; nothing else
     touches it while a build is running, because the incremental gameplay
     rebuild only runs from the GAME branch of draw(). */
  building_surface_mesh = mesh_build_surface_only;
  /* Seam refinement is a cosmetic fix for hairline cracks between quads.  The
     scenic camera never gets close enough to see one, and it is the single
     most expensive part of compiling a world. */
  geometrySetTjunctionRefinement(!mesh_build_surface_only);
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
  building_surface_mesh = FALSE;
  geometrySetTjunctionRefinement(TRUE);

  if (mesh_build_cursor >= CHUNKS_X * CHUNKS_Z) {
    column_arena_ends[mesh_build_arena] = column_dlp;
    active_column_arena = mesh_build_arena;
    mesh_build_active = FALSE;
    return TRUE;
  }
  return FALSE;
}

void graphicsSetRenderOrigin(int block_x, int block_z) {
  u32 slot;
  u8 cy;

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

    if (!windowSlotResident(slot)) {
      continue;
    }
    cx = windowSlotChunkX(slot);
    cz = windowSlotChunkZ(slot);
    for (cy = 0; cy < CHUNKS_Y; cy++) {
      guTranslate(&c_models[slot][cy],
        (float) (cx * CHUNK_SIZE - render_origin_x) * BLOCK_SIZE,
        (float) cy * CHUNK_SIZE * BLOCK_SIZE,
        (float) (cz * CHUNK_SIZE - render_origin_z) * BLOCK_SIZE);
    }
  }
}

void graphicsInvalidateColumnSlot(u32 slot) {
  u8 arena, texture;

  /*
   * Both arenas still hold a mesh describing the column that just left, and
   * c_models[slot] still translates to where it used to be.  Point the slot at
   * the shared empty list so nothing draws until the incoming column has been
   * compiled -- otherwise the old geometry renders at the old place, which
   * reads as terrain smeared across the world.
   *
   * Only the pointers change; the display lists themselves are untouched, so
   * a task already submitted keeps reading valid commands.
   */
  for (arena = 0; arena < NUM_COLUMN_ARENAS; arena++) {
    for (texture = 0; texture < NUM_TEXTURES; texture++) {
      column_starts[arena][texture][slot] = empty_column_display_list;
    }
    column_meshed[arena][slot] = FALSE;
  }
  /* Any pending rebuild referred to the departed column. */
  dirty_columns[slot] = FALSE;
}

static void markColumnDirty(int cx, int cz) {
  /* Only a resident column has anything to rebuild; an edit that reaches past
     the window has no mesh to invalidate. */
  if (windowColumnResident(cx, cz)) {
    dirty_columns[WINDOW_SLOT(cx, cz)] = TRUE;
  }
}

void graphicsMarkColumnDirty(int cx, int cz) {
  markColumnDirty(cx, cz);
}

void makeDisplayListsAt(int x, int z) {
  int cx = x >> CHUNK_SHIFT;
  int cz = z >> CHUNK_SHIFT;

  /* Block edits are cheap to mark now.  A later graphics callback bakes a
     bounded amount of geometry, so mining and tree felling cannot monopolize
     a gameplay frame. */
  markColumnDirty(cx, cz);
  /* Mask, not modulo: a negative coordinate's remainder is negative, so the
     seam with the column to the west would be missed.  The cx > 0 guards go
     with it -- there is no column zero to stop at any more. */
  if ((x & CHUNK_MASK) == 0) {
    markColumnDirty(cx - 1, cz);
  }
  if ((z & CHUNK_MASK) == 0) {
    markColumnDirty(cx, cz - 1);
  }
}

static u8 takeDirtyColumn(int *cx, int *cz) {
  u16 offset;

  for (offset = 0; offset < WINDOW_SLOTS; offset++) {
    u16 slot = (dirty_column_cursor + offset) % WINDOW_SLOTS;
    if (dirty_columns[slot]) {
      dirty_columns[slot] = FALSE;
      dirty_column_cursor = (slot + 1) % WINDOW_SLOTS;
      /* The window can rebind a slot between an edit and this rebuild, which
         leaves a mark referring to a column that is no longer there. */
      if (!windowSlotResident(slot)) {
        continue;
      }
      *cx = windowSlotChunkX(slot);
      *cz = windowSlotChunkZ(slot);
      return TRUE;
    }
  }
  return FALSE;
}

static void startColumnCompaction(void) {
  /* Keep dirty marks until a complete replacement arena is active. If this
     rebuild cannot fit, the old arena remains valid and no edit is forgotten. */
  compaction_column_cursor = 0;
  compacting_columns = TRUE;
  resetColumnArenaBuild(active_column_arena ^ 1);
}

/*
 * Only columns inside the mesh ring are worth arena space.  Without this bound
 * a compaction rebuilds every column that has ever finished generating, which
 * near spawn is the whole starting world on top of everything streamed since
 * -- more than the arena holds, so the pass runs short every time and terrain
 * updates never get a turn.
 */
static u8 columnInMeshRing(u32 slot) {
  int pcx = floor(players[0].position.x / (BLOCK_SIZE * CHUNK_SIZE));
  int pcz = floor(players[0].position.z / (BLOCK_SIZE * CHUNK_SIZE));
  int dx = windowSlotChunkX(slot) - pcx;
  int dz = windowSlotChunkZ(slot) - pcz;

  if (dx < 0) dx = -dx;
  if (dz < 0) dz = -dz;
  return (dx > dz ? dx : dz) <= STREAM_TREE_RADIUS;
}

u8 graphicsColumnNeedsMesh(int cx, int cz) {
  /* The active arena is the one the player is looking at, so it is the only
     authority on whether a column is visibly missing.  The build arena's
     flags are a work list mid-compaction, not the on-screen truth. */
  return windowColumnResident(cx, cz) &&
    !column_meshed[active_column_arena][WINDOW_SLOT(cx, cz)];
}

static void processColumnDisplayListUpdates(int can_reclaim_mesh_arena) {
  u8 budget;

  if (compacting_columns) {
    u8 rebuilt = 0;
    u16 walked = 0;

    while (rebuilt < MESH_COMPACTION_REBUILDS_PER_FRAME &&
        walked < MESH_COMPACTION_SLOTS_PER_FRAME &&
        compaction_column_cursor < WINDOW_SLOTS) {
      u32 slot = compaction_column_cursor;
      /* Walks slots, and rebuilds only the columns that are actually there and
         finished.  resetColumnArenaBuild already emptied the rest. */
      if (windowSlotResident(slot) && columnInMeshRing(slot)) {
        int cx = windowSlotChunkX(slot);
        int cz = windowSlotChunkZ(slot);
        if (worldColumnState(cx, cz) == COLUMN_DECORATED) {
          if (!makeColumnDisplayLists(cx, cz)) {
            /*
             * Out of room part way through.  Publish what fits rather than
             * discarding the pass: every slot in this arena is valid, either
             * freshly built or pointing at the shared empty list, so the world
             * renders with its last columns missing.
             *
             * Discarding instead was a trap.  The arena is still below its
             * reserve next frame, so compaction restarts, fails at the same
             * column, and discards again -- terrain stops updating for good,
             * and no amount of waiting recovers it.  Missing far columns that
             * fill in once the ring moves is a far better failure.
             */
            column_arena_ends[column_build_arena] = column_dlp;
            active_column_arena = column_build_arena;
            compacting_columns = FALSE;
            compaction_ran_short = TRUE;
            compaction_cooldown = MESH_COMPACTION_COOLDOWN_FRAMES;
            return;
          }
          rebuilt++;
        }
      }
      compaction_column_cursor++;
      walked++;
    }
    if (compaction_column_cursor == WINDOW_SLOTS) {
      compaction_ran_short = FALSE;
      column_arena_ends[column_build_arena] = column_dlp;
      active_column_arena = column_build_arena;
      compacting_columns = FALSE;
      compaction_cooldown = MESH_COMPACTION_COOLDOWN_FRAMES;
    }
    return;
  }

  if (compaction_cooldown > 0) {
    compaction_cooldown--;
  }

  column_build_arena = active_column_arena;
  column_dlp = column_arena_ends[active_column_arena];
  column_display_list_full = FALSE;
  if (columnArenaFree() < MESH_COMPACTION_RESERVE) {
    /* The old arena can become the inactive build target only after every
     * submitted RSP task has finished reading it. */
    if (can_reclaim_mesh_arena && compaction_cooldown == 0) {
      startColumnCompaction();
    }
    return;
  }

  for (budget = 0; budget < MESH_REBUILD_BUDGET; budget++) {
    int cx, cz;
    if (!takeDirtyColumn(&cx, &cz)) {
      break;
    }
    if (!makeColumnDisplayLists(cx, cz)) {
      /* takeDirtyColumn clears the mark; restore it because the live column
         pointers were deliberately left unchanged on failure. */
      markColumnDirty(cx, cz);
      if (can_reclaim_mesh_arena && compaction_cooldown == 0) {
        startColumnCompaction();
      }
      break;
    }
    column_arena_ends[active_column_arena] = column_dlp;
    if (column_display_list_full || columnArenaFree() < MESH_COMPACTION_RESERVE) {
      break;
    }
  }
}

void drawTextured(u8 texture, u8 player_num) {
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
    if (visible_columns[player_num][slot]) {
      /* Dropping the most distant terrain is survivable; overrunning the
         frame buffer is not. */
      if (dlp >= frame_dlp_limit) {
        frame_overflows++;
        return;
      }
      gSPDisplayList(dlp++,
        column_starts[active_column_arena][texture][slot]);
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
  /* Water is rendered unlit so its atlas colours remain stable through the
   * day/night render states.  White modulation keeps the blue texture intact. */
  vertex->v.cn[0] = 255;
  vertex->v.cn[1] = 255;
  vertex->v.cn[2] = 255;
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

  if (itemIsSword(item)) {
    gSPVertex(dlp++, item == IRON_SWORD ? steve_sword_blade_verts :
      (item == STONE_SWORD ? stone_sword_blade_verts :
      wood_sword_blade_verts), 8, 0);
    gSPDisplayList(dlp++, steve_box_display_list);
    gSPVertex(dlp++, steve_sword_hilt_verts, 8, 0);
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

static void drawFirstPersonHand(u8 player_num) {
  Player *player = &players[player_num];
  u8 item = playerHeldItem(player);
  float swing;

  if (player->camera_mode != CAMERA_FIRST_PERSON) {
    return;
  }
  swing = punchSwingAngle(player) + miningSwingAngle(player);
  /* Draw in camera space so looking around cannot rotate or displace the
     first-person model. Only the deliberate attack/mining swing changes it. */
  loadCameraProjection();
  guTranslate(&first_person_sword_translate[dl_no][player_num], 34.f, -36.f,
    -72.f);
  guRotateRPY(&first_person_sword_rotate[dl_no][player_num],
    20.f + swing, 0.f, -18.f);
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
  gDPSetCombineMode(dlp++, G_CC_SHADE, G_CC_SHADE);
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
  gDPSetCombineMode(dlp++, G_CC_SHADE, G_CC_SHADE);
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
    body = mutton_verts;
  } else if (item == SLIME_GEL) {
    body = slime_gel_verts;
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
      gDPSetCombineMode(dlp++, G_CC_MODULATERGB, G_CC_MODULATERGB);
      loadTexture(preview_textures[drop->item]);
      gSPVertex(dlp++, dropped_item_verts, 8, 0);
      gSPDisplayList(dlp++, dropped_item_display_list);
    } else {
      gSPTexture(dlp++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
      gDPSetCombineMode(dlp++, G_CC_SHADE, G_CC_SHADE);
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
    }
    drawFallingTree(&trees[tree_index], slot++, ax, az);
  }
  if (slot > 0) {
    gSPSetGeometryMode(dlp++, G_CULL_BACK);
  }
}

void drawWorld() {
  u8 i, player_num;
  u8 cinematic = current_screen == LOADING_PREVIEW || current_screen == MENU ||
    current_screen == WORLD_NAMING;
  u8 viewer_count = cinematic ? 1 : active_player_count;

  if (cinematic) {
    /* A brighter blue keeps the distant water legible while the warm
       terrain light provides depth in the foreground. */
    clearBuffers(GPACK_RGBA5551(48, 123, 211, 1));
  } else {
    /* A single time-varying clear is much cheaper than the former banded
     * gradient while retaining a daylight/nightfall sky behind the terrain. */
    SkyColor sky = dayCycleSkyColor(255);
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
      /* Celestial sprites use AA texture-edge mode.  Terrain must explicitly
       * restore its opaque no-read render mode afterwards. */
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
    for (i = 0; i < NUM_TEXTURES; i++) {
      if (textures[i] == &water_spec) {
        /* Water's dedicated vertices carry white RGB, not normals. */
        gSPClearGeometryMode(dlp++, G_LIGHTING);
      }
      loadTexture(textures[i]->texture);
      drawTextured(i, player_num);
      if (textures[i] == &water_spec) {
        gSPSetGeometryMode(dlp++, G_LIGHTING);
      }
    }
    if (!cinematic) {
      gSPClearGeometryMode(dlp++, G_LIGHTING);
      drawFallingTrees(player_num);
      drawDroppedItems(player_num);
      drawMobsForPlayer(player_num);
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
    /* Shift and mask, not / and %: a negative target's quotient truncates
       toward zero and its remainder goes negative, which walked b_models out
       of bounds west or north of the origin.  Routing through c_models also
       keeps the highlight origin-relative for free. */
    gSPMatrix(dlp++,OS_K0_TO_PHYSICAL(&c_models
      [WINDOW_SLOT(players[player_num].target_x >> CHUNK_SHIFT,
                   players[player_num].target_z >> CHUNK_SHIFT)]
      [players[player_num].target_y / CHUNK_SIZE]),
      G_MTX_MODELVIEW|G_MTX_LOAD|G_MTX_NOPUSH);
    gSPMatrix(dlp++,OS_K0_TO_PHYSICAL(b_models + (players[player_num].target_x & CHUNK_MASK) * CHUNK_SIZE * CHUNK_SIZE + (players[player_num].target_y % CHUNK_SIZE) * CHUNK_SIZE + (players[player_num].target_z & CHUNK_MASK)),
      G_MTX_MODELVIEW|G_MTX_MUL|G_MTX_NOPUSH);
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

static void drawCompass() {
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(7, 10, 13);
  gDPFillRectangle(dlp++, 124, 6, 171, 20);
  setHudFillColor(111, 118, 121);
  gDPFillRectangle(dlp++, 126, 8, 169, 18);
  setHudFillColor(25, 31, 35);
  gDPFillRectangle(dlp++, 128, 9, 167, 17);
  setHudFillColor(91, 98, 98);
  gDPFillRectangle(dlp++, 134, 10, 134, 13);
  gDPFillRectangle(dlp++, 161, 10, 161, 13);
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

static const char *playerHeading(Player *player) {
  if (player->yaw >= 45.f && player->yaw < 135.f) return "W";
  if (player->yaw >= 135.f && player->yaw < 225.f) return "S";
  if (player->yaw >= 225.f && player->yaw < 315.f) return "E";
  return "N";
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
  } else if (itemIsSword(item)) {
    if (item == IRON_SWORD) {
      setHudFillColor(225, 225, 218);
    } else if (item == STONE_SWORD) {
      setHudFillColor(188, 188, 178);
    } else {
      setHudFillColor(174, 117, 61);
    }
    gDPFillRectangle(dlp++, x + size / 2 - 1, y + 1, x + size / 2 + 1, y + size - 6);
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
    gDPFillRectangle(dlp++, x + 1, y + 2, x + size - 2, y + 4);
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
      y + size / 2);
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
  free_percent = (u32) ((column_display_lists[active_column_arena] +
    DISPLAY_LIST_SIZE - column_arena_ends[active_column_arena]) /
    (DISPLAY_LIST_SIZE / 100));
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
  y = drawDiagnosticRow("C",
    compacting_columns ? 1 : (compaction_ran_short ? 2 : 0), y);
  drawDiagnosticRow("T", pending_terrain, y);
  setHudTextColor(255, 255, 255);
}

static void drawGameText() {
  u8 player_num;

  beginText();
  if (active_player_count == 1) {
    drawStreamingDiagnostics();
  }
  for (player_num = 0; player_num < active_player_count; player_num++) {
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
        const char *heading = playerHeading(&players[player_num]);

        setHudTextColor(241, 241, 232);
        drawString(heading, 148 - hudStringWidth(heading) / 2, 10);
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
  setHudTextColor(255, 255, 255);
}

void drawHUD() {
  u8 player_num;

  if (current_screen != GAME && current_screen != INVENTORY &&
      current_screen != LOADING_PREVIEW && current_screen != MENU &&
      current_screen != WORLD_NAMING) {
    clearBuffers(GPACK_RGBA5551(0, 0, 0, 1));
  } else if (current_screen == LOADING_PREVIEW) {
    drawLoadingOverlay();
  } else if (current_screen == MENU || current_screen == WORLD_NAMING) {
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
    for (player_num = 0; player_num < active_player_count; player_num++) {
      u32 crosshair_x = playerViewportX(player_num) + playerViewportWidth() / 2;
      u32 crosshair_y = playerViewportY(player_num) + playerViewportHeight() / 2;
      drawCrosshair(crosshair_x, crosshair_y, &players[player_num]);
      drawBreakProgress(crosshair_x, crosshair_y, &players[player_num]);
      drawHotbar(player_num);
      drawHealth(player_num);
      if (active_player_count == 1) {
        drawCompass();
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
  } else if (current_screen == INVENTORY) {
    /* drawInventory may leave an item preview bound; restore the font before
       issuing count glyph rectangles. */
    beginText();
    drawInventoryStackCounts();
    drawInventoryText();
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
  } else if (current_screen == MENU || current_screen == WORLD_NAMING) {
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

  /* Switch display list buffers */
  dl_no ^= 1;
}

void initGraphics() {
  int x, y, z;

  nuGfxInit();
  nuGfxDisplayOn();

  /* Chunk translations are no longer prebaked here.  A slot's matrices belong
     to whichever column is bound to it, so makeColumnDisplayLists writes them
     as it compiles that column.  Until the first world is built there is no
     terrain to draw, and nothing reads them. */

  for (x = 0; x < CHUNK_SIZE; x++) {
    for (y = 0; y < CHUNK_SIZE; y++) {
      for (z = 0; z < CHUNK_SIZE; z++) {
        guTranslate(&(b_models[x * CHUNK_SIZE * CHUNK_SIZE + y * CHUNK_SIZE + z]), x * BLOCK_SIZE, y * BLOCK_SIZE, z * BLOCK_SIZE);
      }
    }
  }

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

  active_column_arena = 0;
  compacting_columns = FALSE;
  /* Point every column at the shared empty list before the first world is
     compiled.  drawTextured branches to these pointers unconditionally, and
     until the arena is reset they are still NULL from BSS -- any terrain draw
     before the first build would send the RSP to address zero. */
  resetColumnArenaBuild(active_column_arena);
  column_arena_ends[active_column_arena] = column_dlp;
}
