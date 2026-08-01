#ifndef WORLD_H
#define WORLD_H

#include <nusys.h>

#define MAX_X 112
#define MAX_Y 32
#define MAX_Z 112

#define CHUNK_SIZE 8
#define CHUNK_SHIFT 3
#define CHUNK_MASK (CHUNK_SIZE - 1)

#define CHUNKS_X (MAX_X / CHUNK_SIZE)
#define CHUNKS_Y (MAX_Y / CHUNK_SIZE)
#define CHUNKS_Z (MAX_Z / CHUNK_SIZE)

#define NUM_BLOCKS (MAX_X * MAX_Y * MAX_Z)
#define NUM_BLOCK_BYTES ((NUM_BLOCKS + 1) / 2)
#define NUM_CHUNKS (CHUNKS_X * CHUNKS_Y * CHUNKS_Z)

/*
 * The world's 16 block types fit in four bits, and terrain lives in a wrapping
 * window of full-height columns rather than in one array sized to a fixed
 * world.  A column maps to its slot by the low bits of its chunk coordinates,
 * so walking scrolls the window by recycling the slots that fall out of range
 * -- block data is never moved, only overwritten.
 *
 * The window has to extend at least one column past anything being meshed:
 * makeChunkPlaneQuads reads blockAt(r + 1, ...) across the far boundary of the
 * column it is compiling.
 *
 * WINDOW_COLUMNS is a power of two so slot lookup is masking rather than a
 * division; blockGet sits in the meshing inner loop.  32 columns is a 1 MiB
 * block window -- the budget the single mesh arena reclaimed -- and is what
 * lets the residency rings quadruple in area: view distance is bounded by
 * the decoration ring, which is bounded by the window span.
 */
#define WINDOW_SHIFT 5
#define WINDOW_COLUMNS (1 << WINDOW_SHIFT)
#define WINDOW_MASK (WINDOW_COLUMNS - 1)
#define WINDOW_SLOTS (WINDOW_COLUMNS * WINDOW_COLUMNS)

#define COLUMN_BLOCKS (CHUNK_SIZE * MAX_Y * CHUNK_SIZE)
#define COLUMN_BLOCK_BYTES (COLUMN_BLOCKS / 2)

/*
 * Slot occupancy is folded into the key so a residency test costs one load and
 * one compare.  Bit 31 marks a live slot, which keeps a cleared window
 * unambiguously empty -- a plain packed coordinate pair would make column
 * (0, 0) indistinguishable from an unused slot.  15 bits per axis is far more
 * range than the s15.16 matrix format allows before the origin must rebase.
 */
#define COLUMN_KEY(cx, cz) \
  (0x80000000u | (((u32) (cx) & 0x7FFFu) << 15) | ((u32) (cz) & 0x7FFFu))
#define COLUMN_KEY_EMPTY 0u

#define WINDOW_SLOT(cx, cz) \
  (((((u32) (cx)) & WINDOW_MASK) << WINDOW_SHIFT) | \
    (((u32) (cz)) & WINDOW_MASK))

/* Column-local block offset.  bx and bz wrap within the column and y spans the
   full world height, so the three pack into 0..COLUMN_BLOCKS-1. */
#define BLOCK_LOCAL_INDEX(x, y, z) \
  (((u32) ((x) & CHUNK_MASK) * MAX_Y + (u32) (y)) * CHUNK_SIZE + \
    (u32) ((z) & CHUNK_MASK))

/* Reported for a column that is not resident.  Distinct from every real block
   so callers can tell "nothing there" from "not loaded yet", letting meshing
   defer a column whose neighbours have not streamed in rather than baking a
   wall into the world. */
#define BLOCK_NOT_RESIDENT 0xFF

extern u8 window_blocks[WINDOW_SLOTS][COLUMN_BLOCK_BYTES];
extern u32 window_keys[WINDOW_SLOTS];

static __inline__ __attribute__((unused)) u8 *columnSlotData(int cx, int cz) {
  u32 slot = WINDOW_SLOT(cx, cz);
  if (window_keys[slot] != COLUMN_KEY(cx, cz)) {
    return (u8 *) 0;
  }
  return window_blocks[slot];
}

static __inline__ __attribute__((unused)) u8 columnBlockGet(const u8 *column,
    u32 local) {
  u8 packed = column[local >> 1];
  return (local & 1) ? packed & 0x0F : packed >> 4;
}

static __inline__ __attribute__((unused)) void columnBlockSet(u8 *column,
    u32 local, u8 block) {
  u8 *packed = &column[local >> 1];
  if (local & 1) {
    *packed = (*packed & 0xF0) | (block & 0x0F);
  } else {
    *packed = (*packed & 0x0F) | (block << 4);
  }
}

static __inline__ __attribute__((unused)) u8 blockGet(int x, int y, int z) {
  const u8 *column;

  if ((u32) y >= (u32) MAX_Y) {
    return BLOCK_NOT_RESIDENT;
  }
  column = columnSlotData(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT);
  if (column == (u8 *) 0) {
    return BLOCK_NOT_RESIDENT;
  }
  return columnBlockGet(column, BLOCK_LOCAL_INDEX(x, y, z));
}

static __inline__ __attribute__((unused)) void blockSet(int x, int y, int z,
    u8 block) {
  u8 *column;

  if ((u32) y >= (u32) MAX_Y) {
    return;
  }
  column = columnSlotData(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT);
  if (column == (u8 *) 0) {
    return;
  }
  columnBlockSet(column, BLOCK_LOCAL_INDEX(x, y, z), block);
}

/* Bind a slot to a column and hand back its storage, evicting whatever it held.
   Generation and streaming both acquire columns through this. */
u8 *windowClaimColumn(int cx, int cz);
u8 windowColumnResident(int cx, int cz);

/*
 * Which column a slot currently holds.  Rendering indexes its per-column
 * tables by slot rather than by a position in a fixed world, so it needs to
 * get back from a slot to the coordinates it stands for.
 *
 * Each axis is a 15-bit two's complement field in the key.  Recovering the
 * sign by flipping the field's own sign bit and subtracting keeps it defined
 * for negative chunks, which shifting a value up into bit 31 would not.
 */
static __inline__ __attribute__((unused)) u8 windowSlotResident(u32 slot) {
  return (window_keys[slot] & 0x80000000u) != 0;
}

static __inline__ __attribute__((unused)) int windowSlotChunkX(u32 slot) {
  u32 field = (window_keys[slot] >> 15) & 0x7FFFu;
  return (int) (field ^ 0x4000u) - 0x4000;
}

static __inline__ __attribute__((unused)) int windowSlotChunkZ(u32 slot) {
  u32 field = window_keys[slot] & 0x7FFFu;
  return (int) (field ^ 0x4000u) - 0x4000;
}
void windowReset();
/* Bind every column of the fixed MAX_X by MAX_Z extent, for whole-world
   generation and whole-world loading. */
void windowClaimFixedExtent();

/* Whole-world generation in one blocking pass.  Prefer the sliced form below
   from anything that runs on the graphics thread. */
void initWorld();

/* Sliced generation.  stepWorldGeneration consumes at most `columns` terrain
   columns and returns TRUE once the world is finished, so a caller can keep
   submitting frames while a world builds.  The seed is the caller's, so the
   setup card can show it and roll it again before committing.  Cancelling
   abandons the partial world; the next begin claims the extent afresh. */
void beginWorldGeneration(u32 chosen_seed);
u8 stepWorldGeneration(u32 columns);
void cancelWorldGeneration(void);

/*
 * Per-column generation.  Terrain can always be built alone; decoration
 * reaches into neighbouring columns, so it advances through stages and only
 * when the neighbours it touches have caught up.  Driving these directly is
 * how streaming builds a column, and running them as three whole-extent passes
 * is how a fixed world is built -- both produce the same world.
 */
#define COLUMN_EMPTY 0
#define COLUMN_TERRAIN 1
#define COLUMN_STRUCTURED 2
#define COLUMN_DECORATED 3

u8 worldColumnState(int cx, int cz);
/* Declare the loaded extent fully built, so streaming does not regenerate over
   a world that came from a save rather than from the generator. */
void worldMarkFixedExtentBuilt();
/* TRUE while every column of the fixed save extent is resident.  The save
   format still writes the whole 0..MAX extent from live block data, so an
   evicted column would be written as garbage; until per-chunk diff saves land
   (task 6), saving is refused whenever this is FALSE. */
u8 worldFixedExtentResident();
/*
 * Advance residency around a point, in chunk coordinates.  Claims and builds
 * the columns that should be loaded and lets the ones that should not fall out
 * of the window.
 */
/* Chebyshev chunk radii.  Terrain one wider than structures, structures one
   wider than trees: the neighbour gates decorate a column only when
   everything it can write into is already claimed.  A 32-column window
   could hold radius 15, but the mesh ring is what the RSP and the arena
   actually pay for: radius 12 was measurably choppy on hardware, radius 10
   -- an ~80-block view, still double the original ring -- is the middle
   ground that keeps movement smooth.  The window slack also cushions the
   wrap. */
#define STREAM_TERRAIN_RADIUS 12
#define STREAM_STRUCTURE_RADIUS 11
#define STREAM_TREE_RADIUS 10
/* Within this ring the deferred underground carve (caves, ores) has run.
   Must stay ahead of both the full-detail LOD promote radius and the
   player's reach; the surface never changes when a column deepens, so the
   frontier is invisible. */
#define STREAM_DEEPEN_RADIUS 5

/* TRUE once a resident column's underground has been carved; the full-detail
   mesh waits for it. */
u8 worldColumnDeep(int cx, int cz);

/*
 * Wall-clock ceiling for the per-callback streaming and meshing work.
 *
 * The column budgets bound the *count* of expensive steps, but a terrain
 * column is multi-octave noise over 2048 blocks and a full-detail mesh is a
 * greedy pass plus seam refinement -- a callback that lands three of each
 * spends over 100 ms of a 93 MHz CPU while physics keeps being called at
 * retrace rate: the measured "quicksand" (W 1430 B 1419 on run 5).  Work
 * checks this deadline between columns and stops early; whatever remains
 * simply happens next callback.  Zero disables the ceiling, which is what
 * the host harness and the loading screen use -- the loading screen has no
 * gameplay to starve.
 */
extern OSTime stream_work_deadline;

static __inline__ __attribute__((unused)) u8 streamWorkExpired() {
  return stream_work_deadline != 0 && osGetTime() >= stream_work_deadline;
}

/*
 * The streaming pipeline's stages, in order.  MESH is the compilation queue
 * in graphics.c; the rest live in stepWorldStreaming.
 *
 * Exactly one stage per callback may take a step after the deadline has
 * expired -- the stage whose turn worldStreamStageGuaranteed reports.  Every
 * stage used to hold that exemption every frame, which kept any one stage
 * from starving but let the exemptions stack: the worst walking frame paid
 * one large unit of every stage at once, on top of the budgeted work
 * (measured as B 850 against a 250 deadline).  Rotating the exemption keeps
 * the liveness -- each stage still advances at least once every
 * STREAM_STAGE_COUNT frames however oversubscribed the deadline is -- while
 * bounding the overrun to one unit per frame.
 */
#define STREAM_STAGE_TERRAIN 0
#define STREAM_STAGE_DEEPEN 1
#define STREAM_STAGE_STRUCTURES 2
#define STREAM_STAGE_TREES 3
#define STREAM_STAGE_MESH 4
#define STREAM_STAGE_COUNT 5

/* TRUE when the stage holds this callback's deadline exemption.  Advanced at
   the top of stepWorldStreaming; the mesh queue runs later in the same
   callback (inside draw()), so it reads the same frame's turn. */
u8 worldStreamStageGuaranteed(u8 stage);

void stepWorldStreaming(int pcx, int pcz, u32 terrain_budget,
  u32 decorate_budget);
/* Prefetch bias in chunks: ranking (never ring membership) leans toward the
   player's heading so the terrain being walked toward builds first. */
void worldSetStreamBias(int bias_cx, int bias_cz);
/* Window-key corruption audit; see world.c.  The counters feed the K row and
   the freeze report. */
void windowAuditKeys(void);
extern u32 window_key_faults;
extern u32 window_key_fault_value;
extern u32 window_key_fault_slot;
void worldGenerateColumnTerrain(int cx, int cz);
/* FALSE while the column still needs a neighbour to catch up. */
u8 worldAdvanceColumnDecoration(int cx, int cz);
/*
 * A cottage in the hamlet whose plan covers this block, or FALSE.
 *
 * The plan is coordinate-pure and cached, so asking is arithmetic rather than
 * a search, and the answer is the same in every session of the same world.
 * Villagers use it for both halves of who they are: where home is, and -- via
 * the coordinate itself -- which person lives there.
 */
#define WORLD_HAMLET_HOUSES 4
u8 worldHamletHouse(int block_x, int block_z, u8 house, int *house_x,
  int *house_z);

u8 worldGenerationActive();
u8 worldGenerationProgress();
u8 tryPlantTree(int x, int y, int z);

#endif /* WORLD_H */
