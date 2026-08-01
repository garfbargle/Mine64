#include "edits.h"

#include "blocks.h"
#include "world.h"

WorldEdit world_edits[MAX_WORLD_EDITS];
u16 world_edit_column_first[EDIT_COLUMNS + 1];
u16 world_edit_count;
u32 world_edit_overflows;
u8 world_edit_full_message;

#define EDIT_MESSAGE_FRAMES 90

/* Bucket index for a cell already known to be inside the extent.  Major in x
   so the bucket of (cx, cz) is a single multiply-add, matching the order
   world_edit_column_first is walked in. */
static u16 bucketOf(int x, int z) {
  return (u16) (((x >> CHUNK_SHIFT) * CHUNKS_Z) + (z >> CHUNK_SHIFT));
}

u8 worldEditInExtent(int x, int y, int z) {
  return (u32) x < (u32) MAX_X && (u32) y < (u32) MAX_Y &&
    (u32) z < (u32) MAX_Z;
}

void initWorldEdits(void) {
  u16 index;

  world_edit_count = 0;
  world_edit_overflows = 0;
  world_edit_full_message = 0;
  for (index = 0; index <= EDIT_COLUMNS; index++) {
    world_edit_column_first[index] = 0;
  }
}

void worldEditTickMessage(void) {
  if (world_edit_full_message > 0) {
    world_edit_full_message--;
  }
}

/*
 * Position of `key` within its bucket, or of the slot it would occupy.
 *
 * Records inside a bucket are sorted by key, so this is a binary search over a
 * range holding one column's edits rather than a walk of the whole pool.
 * *found separates a hit from an insertion point, which are otherwise the same
 * number.
 */
static u16 findSlot(u16 bucket, u32 key, u8 *found) {
  u16 low = world_edit_column_first[bucket];
  u16 high = world_edit_column_first[bucket + 1];

  *found = FALSE;
  while (low < high) {
    u16 mid = (u16) (low + ((high - low) >> 1));
    u32 mid_key = EDIT_KEY_OF(world_edits[mid]);

    if (mid_key == key) {
      *found = TRUE;
      return mid;
    }
    if (mid_key < key) {
      low = (u16) (mid + 1);
    } else {
      high = mid;
    }
  }
  return low;
}

/* Open a slot at `at` and push every later bucket's start along with it. */
static void insertAt(u16 at, u16 bucket, WorldEdit record) {
  u16 index;

  for (index = world_edit_count; index > at; index--) {
    world_edits[index] = world_edits[index - 1];
  }
  world_edits[at] = record;
  world_edit_count++;
  for (index = (u16) (bucket + 1); index <= EDIT_COLUMNS; index++) {
    world_edit_column_first[index]++;
  }
}

static void removeAt(u16 at, u16 bucket) {
  u16 index;

  for (index = at; (u16) (index + 1) < world_edit_count; index++) {
    world_edits[index] = world_edits[index + 1];
  }
  world_edit_count--;
  for (index = (u16) (bucket + 1); index <= EDIT_COLUMNS; index++) {
    world_edit_column_first[index]--;
  }
}

/*
 * A cell can be edited once its column is fully built.
 *
 * Residency alone is not enough.  A new record captures the block the
 * generator left in the cell, so that later putting that block back can
 * release the slot, and a column that has had terrain but not yet structures
 * or trees has not finished writing.  An original captured there would record
 * a value decoration is about to overwrite, and the reclamation test would
 * then throw away an edit the player expected to keep.
 *
 * Outside the extent there is no record and so no original to get wrong; the
 * change is transient either way, and only residency matters.
 */
static u8 cellReady(int x, int y, int z) {
  return (u32) y < (u32) MAX_Y &&
    worldColumnState(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT) == COLUMN_DECORATED;
}

/* Refusal bookkeeping in one place, so a refused break and a refused
   placement look the same to the player.  Always answers FALSE, which is what
   both callers want to return. */
static u8 refusePoolFull(void) {
  world_edit_overflows++;
  world_edit_full_message = EDIT_MESSAGE_FRAMES;
  return FALSE;
}

u8 worldEditCanSet(int x, int y, int z) {
  u8 found;

  if (!cellReady(x, y, z)) {
    return FALSE;
  }
  if (!worldEditInExtent(x, y, z)) {
    return TRUE;
  }
  findSlot(bucketOf(x, z), EDIT_KEY(x, y, z), &found);
  /* An existing record is rewritten in place, and any edit can be undone back
     to its original, which frees a slot rather than taking one. */
  if (found) {
    return TRUE;
  }
  if (world_edit_count >= MAX_WORLD_EDITS) {
    return refusePoolFull();
  }
  return TRUE;
}

u8 worldEditSet(int x, int y, int z, u8 block) {
  u16 bucket;
  u16 at;
  u8 found;
  u32 key;

  /* Deliberately not worldEditCanSet: that would search the bucket a second
     time, and its full-pool answer cannot tell this function whether the cell
     already has a record (which needs no new slot) or not. */
  if (!BLOCK_IS_VALID(block) || !cellReady(x, y, z)) {
    return FALSE;
  }

  /* Outside the fixed extent the block window is the only home the change
     has.  It stands until the column is evicted and is never saved. */
  if (!worldEditInExtent(x, y, z)) {
    blockSet(x, y, z, block);
    return TRUE;
  }

  bucket = bucketOf(x, z);
  key = EDIT_KEY(x, y, z);
  at = findSlot(bucket, key, &found);

  if (found) {
    u8 original = (u8) EDIT_ORIGINAL(world_edits[at]);

    if (block == original) {
      /* Back to what the generator produces here, so this is no longer a
         deviation and the slot returns to the pool.  Mining a block and
         putting the same block back costs nothing permanently. */
      removeAt(at, bucket);
    } else {
      world_edits[at] = EDIT_MAKE(key, original, block);
    }
  } else {
    u8 original = blockGet(x, y, z);

    if (block == original) {
      /* Nothing to retain: the cell already holds what regeneration puts
         back. */
      blockSet(x, y, z, block);
      return TRUE;
    }
    if (world_edit_count >= MAX_WORLD_EDITS) {
      return refusePoolFull();
    }
    insertAt(at, bucket, EDIT_MAKE(key, original, block));
  }

  blockSet(x, y, z, block);
  return TRUE;
}

void worldApplyEditsToColumn(int cx, int cz) {
  u16 bucket;
  u16 index;
  u16 end;

  if (world_edit_count == 0 || !windowColumnResident(cx, cz)) {
    return;
  }
  /* Columns outside the extent hold no records at all, so the whole replay is
     one range check in the exploration case. */
  if ((u32) cx >= (u32) CHUNKS_X || (u32) cz >= (u32) CHUNKS_Z) {
    return;
  }

  bucket = (u16) ((cx * CHUNKS_Z) + cz);
  end = world_edit_column_first[bucket + 1];
  for (index = world_edit_column_first[bucket]; index < end; index++) {
    WorldEdit record = world_edits[index];

    blockSet((int) EDIT_X(record), (int) EDIT_Y(record), (int) EDIT_Z(record),
      (u8) EDIT_BLOCK(record));
  }
}
