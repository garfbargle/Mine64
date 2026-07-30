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
 * division; blockGet sits in the meshing inner loop.
 */
#define WINDOW_SHIFT 4
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
void windowReset();
/* Bind every column of the fixed MAX_X by MAX_Z extent, for whole-world
   generation and whole-world loading. */
void windowClaimFixedExtent();

/* Whole-world generation in one blocking pass.  Prefer the sliced form below
   from anything that runs on the graphics thread. */
void initWorld();

/* Sliced generation.  stepWorldGeneration consumes at most `columns` terrain
   columns and returns TRUE once the world is finished, so a caller can keep
   submitting frames while a world builds. */
void beginWorldGeneration();
u8 stepWorldGeneration(u32 columns);
u8 worldGenerationActive();
u8 worldGenerationProgress();
u8 tryPlantTree(int x, int y, int z);

#endif /* WORLD_H */
