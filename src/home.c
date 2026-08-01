#include "home.h"

#include "world.h"

u8 home_blocks[HOME_COLUMNS][COLUMN_BLOCK_BYTES];
u8 home_present[HOME_COLUMNS];
/* Set where the player edits a column, cleared where it is captured.  A dirty
   column's window copy is newer than its snapshot, so the snapshot must not be
   copied back over it. */
static u8 home_dirty[HOME_COLUMNS];

void initHome(void) {
  u16 index;

  for (index = 0; index < HOME_COLUMNS; index++) {
    home_present[index] = FALSE;
    home_dirty[index] = FALSE;
  }
}

u8 homeColumnIndex(int cx, int cz, u16 *out) {
  if ((u32) cx >= (u32) CHUNKS_X || (u32) cz >= (u32) CHUNKS_Z) {
    return FALSE;
  }
  *out = (u16) (cx * CHUNKS_Z + cz);
  return TRUE;
}

static void copyColumn(u8 *destination, const u8 *source) {
  u32 index;

  for (index = 0; index < COLUMN_BLOCK_BYTES; index++) {
    destination[index] = source[index];
  }
}

void homeRestoreColumn(int cx, int cz) {
  u16 index;
  u8 *column;

  if (!homeColumnIndex(cx, cz, &index) || !home_present[index]) {
    return;
  }
  column = columnSlotData(cx, cz);
  if (column != (u8 *) 0) {
    copyColumn(column, home_blocks[index]);
  }
}

void homeCaptureExtent(void) {
  int cx, cz;

  for (cx = 0; cx < CHUNKS_X; cx++) {
    for (cz = 0; cz < CHUNKS_Z; cz++) {
      u16 index;
      const u8 *column = columnSlotData(cx, cz);

      if (column != (u8 *) 0 && homeColumnIndex(cx, cz, &index)) {
        copyColumn(home_blocks[index], column);
        home_present[index] = TRUE;
        home_dirty[index] = FALSE;
      }
    }
  }
}

void homeMarkDirty(int x, int z) {
  u16 index;

  if (homeColumnIndex(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT, &index)) {
    home_dirty[index] = TRUE;
  }
}

void homeFlushDirtyNeighbours(int cx, int cz) {
  int dx, dz;

  for (dx = -1; dx <= 1; dx++) {
    for (dz = -1; dz <= 1; dz++) {
      u16 index;

      if (dx == 0 && dz == 0) {
        continue;
      }
      if (homeColumnIndex(cx + dx, cz + dz, &index) && home_dirty[index]) {
        homeFlushColumn(cx + dx, cz + dz);
      }
    }
  }
}

void homeResyncNeighbours(int cx, int cz) {
  int dx, dz;

  for (dx = -1; dx <= 1; dx++) {
    for (dz = -1; dz <= 1; dz++) {
      u16 index;
      u8 *column;

      if ((dx == 0 && dz == 0) ||
          !homeColumnIndex(cx + dx, cz + dz, &index) ||
          !home_present[index]) {
        continue;
      }
      column = columnSlotData(cx + dx, cz + dz);
      if (column != (u8 *) 0) {
        copyColumn(column, home_blocks[index]);
      }
    }
  }
}

void homeFlushColumn(int cx, int cz) {
  u16 index;
  u8 *column;

  if (!homeColumnIndex(cx, cz, &index)) {
    return;
  }
  /* A column that never finished decorating is not a truth worth keeping:
     capturing it would freeze half-built terrain into the save. */
  if (worldColumnState(cx, cz) != COLUMN_DECORATED) {
    return;
  }
  column = columnSlotData(cx, cz);
  if (column == (u8 *) 0) {
    return;
  }
  copyColumn(home_blocks[index], column);
  home_present[index] = TRUE;
  home_dirty[index] = FALSE;
}

void homeFlushResident(void) {
  int cx, cz;

  for (cx = 0; cx < CHUNKS_X; cx++) {
    for (cz = 0; cz < CHUNKS_Z; cz++) {
      homeFlushColumn(cx, cz);
    }
  }
}

u8 homeBlockGet(int x, int y, int z) {
  u16 index;

  if ((u32) y >= (u32) MAX_Y ||
      !homeColumnIndex(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT, &index)) {
    return BLOCK_NOT_RESIDENT;
  }
  return columnBlockGet(home_blocks[index], BLOCK_LOCAL_INDEX(x, y, z));
}

void homeBlockSet(int x, int y, int z, u8 block) {
  u16 index;

  if ((u32) y >= (u32) MAX_Y ||
      !homeColumnIndex(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT, &index)) {
    return;
  }
  columnBlockSet(home_blocks[index], BLOCK_LOCAL_INDEX(x, y, z), block);
}

void homeMarkAllPresent(void) {
  u16 index;

  for (index = 0; index < HOME_COLUMNS; index++) {
    home_present[index] = TRUE;
    home_dirty[index] = FALSE;
  }
}
