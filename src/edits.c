#include "edits.h"

#include "blocks.h"
#include "home.h"
#include "world.h"

void initWorldEdits(void) {
  initHome();
}

u8 worldEditInExtent(int x, int y, int z) {
  return (u32) x < (u32) MAX_X && (u32) y < (u32) MAX_Y &&
    (u32) z < (u32) MAX_Z;
}

/*
 * A cell can be written once its column is fully built.
 *
 * Residency alone is not enough.  A column that has had terrain but not yet
 * structures or trees is still being written by the generator, and a block
 * placed into it would be overwritten by decoration a moment later -- the
 * player would watch their block vanish for no visible reason.
 */
u8 worldEditCanSet(int x, int y, int z) {
  return (u32) y < (u32) MAX_Y &&
    worldColumnState(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT) == COLUMN_DECORATED;
}

u8 worldEditSet(int x, int y, int z, u8 block) {
  if (!BLOCK_IS_VALID(block) || !worldEditCanSet(x, y, z)) {
    return FALSE;
  }
  blockSet(x, y, z, block);
  /* Inside the extent the store picks this up at the next flush, so the write
     itself is the whole of the work -- but until then the window holds the
     only copy, and the column has to say so.  Outside the extent the window
     write is also the whole of the work; there is just nothing that will
     outlive it. */
  if (worldEditInExtent(x, y, z)) {
    homeMarkDirty(x, z);
  }
  return TRUE;
}
