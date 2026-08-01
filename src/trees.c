#include "trees.h"

#include "blocks.h"
#include "geometry.h"
#include "graphics.h"
#include "items.h"

#define TREE_NONE 255
/* Eight is the whole of trunk_mask, and raising the ceiling to it is what a
   jungle tree needs: at five logs the tall species could not clear its own
   canopy.  The record grows by nothing -- the mask was already a u8 with
   three bits spare -- so the save layout is byte-identical either way. */
#define TREE_TRUNK_PARTS 8
#define TREE_LEAF_LAYERS 4
#define TREE_LEAF_BITS (25 * TREE_LEAF_LAYERS)
#define TREE_DEBRIS_PARTS (TREE_TRUNK_PARTS + TREE_LEAF_BITS)
#define TREE_FALL_FRAMES 42.f

TreeRecord trees[MAX_TREES];

/*
 * Root lookup is derived state and is never written to save files.  The old
 * direct table covered the whole 256 x 256 coordinate wrap even though at
 * most MAX_TREES entries can be live, spending 64 KiB to index 160 records.
 * Keep the same constant-time shape with a fixed sparse hash instead: 512
 * slots put its worst-case load at 31.25%, cost 1,536 bytes, and need no heap.
 *
 * Tree records store the low byte of each world coordinate.  Absolute lookup
 * coordinates are folded to that same 16-bit key.  This remains unambiguous:
 * the residency window also spans 256 blocks, and rebinding a window slot
 * retires its trees before a column one whole wrap away can take the slot.
 * Consequently two live roots can never differ by 256 on either axis.
 *
 * Linear probing keeps lookup compact and cache-friendly.  Removal repairs
 * the following probe cluster immediately instead of leaving tombstones, so
 * a long streaming walk cannot slowly turn every miss into a full-table scan.
 */
#define TREE_ROOT_SPAN (WINDOW_COLUMNS * CHUNK_SIZE)
#define TREE_ROOT_HASH_SLOTS 512
#define TREE_ROOT_HASH_MASK (TREE_ROOT_HASH_SLOTS - 1)
#define TREE_ROOT_KEY(x, z) \
  ((u16) (((u16) (u8) (x) << 8) | (u16) (u8) (z)))

typedef char TreeRootHashMustBePowerOfTwo[
  (TREE_ROOT_HASH_SLOTS & TREE_ROOT_HASH_MASK) == 0 ? 1 : -1
];
typedef char TreeRootHashMustExceedPool[
  TREE_ROOT_HASH_SLOTS > MAX_TREES ? 1 : -1
];
typedef char TreeRootWindowMustFitStoredCoordinates[
  TREE_ROOT_SPAN <= 256 ? 1 : -1
];
/* rebuildTreeLookup no longer range-checks trunk_mask, because at eight parts
   every bit of the u8 names a real log.  Should the ceiling ever come back
   down, this fails the build rather than letting an unchecked high bit
   through as a phantom trunk. */
typedef char TreeTrunkMustFillItsMask[
  TREE_TRUNK_PARTS == 8 ? 1 : -1
];

/* A zero value marks an empty slot; live values are tree index + 1.  Separate
   arrays avoid the padding a { u16, u8 } entry would acquire on MIPS. */
static u16 tree_root_keys[TREE_ROOT_HASH_SLOTS];
static u8 tree_root_values[TREE_ROOT_HASH_SLOTS];

static u16 treeRootHash(u16 key) {
  u16 hash = key;

  /* A cheap reversible mix spreads both coordinate bytes across all nine
     slot bits without putting a multiply in canopy generation's hot path. */
  hash ^= hash >> 7;
  hash ^= hash << 9;
  hash ^= hash >> 8;
  return hash & TREE_ROOT_HASH_MASK;
}

static u8 treeRootInsert(u16 key, u8 value) {
  u16 slot = treeRootHash(key);
  u16 probes;

  for (probes = 0; probes < TREE_ROOT_HASH_SLOTS; probes++) {
    if (tree_root_values[slot] == 0 || tree_root_keys[slot] == key) {
      tree_root_keys[slot] = key;
      tree_root_values[slot] = value;
      return TRUE;
    }
    slot = (slot + 1) & TREE_ROOT_HASH_MASK;
  }
  return FALSE;
}

static void treeRootRemove(u16 key, u8 value) {
  u16 slot = treeRootHash(key);
  u16 probes;

  for (probes = 0; probes < TREE_ROOT_HASH_SLOTS; probes++) {
    u16 scan;
    u16 rehashes;

    if (tree_root_values[slot] == 0) {
      return;
    }
    if (tree_root_keys[slot] != key) {
      slot = (slot + 1) & TREE_ROOT_HASH_MASK;
      continue;
    }

    /* A duplicate-root record can only arise from corrupt state.  Do not let
       retiring that stale record erase a newer record's mapping. */
    if (tree_root_values[slot] != value) {
      return;
    }
    tree_root_values[slot] = 0;

    /* Closing and reinserting the rest of this cluster preserves the rule
       that lookup may stop at its first empty slot. */
    scan = (slot + 1) & TREE_ROOT_HASH_MASK;
    for (rehashes = 0;
        rehashes < TREE_ROOT_HASH_SLOTS && tree_root_values[scan] != 0;
        rehashes++) {
      u16 rehash_key = tree_root_keys[scan];
      u8 rehash_value = tree_root_values[scan];

      tree_root_values[scan] = 0;
      treeRootInsert(rehash_key, rehash_value);
      scan = (scan + 1) & TREE_ROOT_HASH_MASK;
    }
    return;
  }
}

static u8 leafBitSet(TreeRecord *tree, u8 bit) {
  return (tree->leaf_mask[bit >> 3] & (1 << (bit & 7))) != 0;
}

static void setLeafBit(TreeRecord *tree, u8 bit, u8 value) {
  u8 mask = 1 << (bit & 7);
  if (value) {
    tree->leaf_mask[bit >> 3] |= mask;
  } else {
    tree->leaf_mask[bit >> 3] &= ~mask;
  }
}

static u8 treeIndexAtRoot(int x, int z) {
  u16 key = TREE_ROOT_KEY(x, z);
  u16 slot = treeRootHash(key);
  u16 probes;

  for (probes = 0; probes < TREE_ROOT_HASH_SLOTS; probes++) {
    u8 value = tree_root_values[slot];

    if (value == 0) {
      return TREE_NONE;
    }
    if (tree_root_keys[slot] == key) {
      return value <= MAX_TREES ? value - 1 : TREE_NONE;
    }
    slot = (slot + 1) & TREE_ROOT_HASH_MASK;
  }
  return TREE_NONE;
}

/*
 * Recover a live tree's absolute root coordinates from its wrapped u8 record.
 *
 * The 256-block u8 wrap matches the 256-block window span, so a wrapped
 * coordinate still selects the tree's own window slot, whose key stores the
 * full absolute chunk coordinate.  Eviction retires every tree in a column
 * before its slot rebinds, so a live record's slot is its column and the
 * recovery is exact, with no viewer anchor needed.
 *
 * This matters because blockGet/blockSet check residency against absolute
 * coordinates: past the first 256 blocks the wrapped u8 no longer names a
 * resident column, and every read through it comes back BLOCK_NOT_RESIDENT
 * while every write silently misses.
 */
static u8 treeAbsoluteRoot(const TreeRecord *tree, int *abs_x, int *abs_z) {
  u32 slot = WINDOW_SLOT((u32) tree->x >> CHUNK_SHIFT,
    (u32) tree->z >> CHUNK_SHIFT);

  if (!windowSlotResident(slot)) {
    return FALSE;
  }
  *abs_x = windowSlotChunkX(slot) * CHUNK_SIZE + (tree->x & CHUNK_MASK);
  *abs_z = windowSlotChunkZ(slot) * CHUNK_SIZE + (tree->z & CHUNK_MASK);
  return TRUE;
}

static void retireTree(u8 index) {
  TreeRecord *tree = &trees[index];
  treeRootRemove(TREE_ROOT_KEY(tree->x, tree->z), index + 1);
  tree->base_y = TREE_INACTIVE_Y;
  tree->state = TREE_STATE_STANDING;
}

void initTrees() {
  u8 i;
  u16 slot;

  for (i = 0; i < MAX_TREES; i++) {
    trees[i].base_y = TREE_INACTIVE_Y;
    trees[i].state = TREE_STATE_STANDING;
  }
  for (slot = 0; slot < TREE_ROOT_HASH_SLOTS; slot++) {
    tree_root_values[slot] = 0;
  }
}

void treesEvictColumn(int cx, int cz) {
  u8 i;

  for (i = 0; i < MAX_TREES; i++) {
    TreeRecord *tree = &trees[i];

    if (tree->base_y == TREE_INACTIVE_Y) {
      continue;
    }
    /* Only a window-relative position survives in a u8 record, which is
       exactly enough to say which resident column a tree stands in. */
    if ((((u32) tree->x & (TREE_ROOT_SPAN - 1)) >> CHUNK_SHIFT) ==
          ((u32) cx & WINDOW_MASK) &&
        (((u32) tree->z & (TREE_ROOT_SPAN - 1)) >> CHUNK_SHIFT) ==
          ((u32) cz & WINDOW_MASK)) {
      retireTree(i);
    }
  }
}

/*
 * Retire every record whose root lies outside the fixed 0..MAX extent.
 *
 * Saving requires the whole fixed extent resident, but the residency ring is
 * wider than the world: columns at chunk -1 or 14 can be live at save time,
 * and their trees carry wrapped coordinates of 112 and up.  The v10 format
 * writes the pool raw, and loadGame validates it with treesValid, which
 * rejects any root at or past MAX_X -- so one such record makes the whole
 * save read as corrupt on the next boot and quietly falls back to the backup
 * world.  The records themselves are expendable: the save cannot carry their
 * columns' blocks anyway, and streaming regenerates tree and record together
 * when the column next arrives.
 */
void treesDropOutsideFixedExtent() {
  u8 i;
  u8 fill = 0;

  for (i = 0; i < MAX_TREES; i++) {
    TreeRecord *tree = &trees[i];
    int abs_x, abs_z;

    if (tree->base_y == TREE_INACTIVE_Y) {
      continue;
    }
    if (!treeAbsoluteRoot(tree, &abs_x, &abs_z) ||
        abs_x < 0 || abs_x >= MAX_X || abs_z < 0 || abs_z >= MAX_Z) {
      retireTree(i);
    }
  }

  /*
   * The save writes only the first TREE_SAVE_COUNT records (the frozen v10
   * layout), while the live pool is larger to cover the wider decorated
   * ring.  Pack the survivors below that line so none of them is silently
   * dropped from the file.  Indices are derived state -- the lookup table is
   * rebuilt afterwards, and nothing else holds one across frames.
   */
  for (i = 0; i < MAX_TREES; i++) {
    if (trees[i].base_y == TREE_INACTIVE_Y) {
      continue;
    }
    if (fill != i) {
      trees[fill] = trees[i];
      trees[i].base_y = TREE_INACTIVE_Y;
      trees[i].state = TREE_STATE_STANDING;
    }
    fill++;
  }
  rebuildTreeLookup();
}

u8 createTree(u8 x, u8 z, u8 base_y, u8 height) {
  u8 i;
  u8 byte;
  u8 part;
  TreeRecord *tree;

  for (i = 0; i < MAX_TREES; i++) {
    if (trees[i].base_y == TREE_INACTIVE_Y) {
      break;
    }
  }
  if (i == MAX_TREES) {
    return TREE_NONE;
  }

  tree = &trees[i];
  tree->x = x;
  tree->z = z;
  tree->base_y = base_y;
  tree->trunk_mask = 0;
  tree->canopy_y = base_y + height - 1;
  tree->state = TREE_STATE_STANDING;
  tree->fall_direction = 0;
  tree->debris_cursor = 0;
  tree->fall_progress = 0;
  for (byte = 0; byte < TREE_LEAF_MASK_BYTES; byte++) {
    tree->leaf_mask[byte] = 0;
  }
  for (part = 0; part < height && part < TREE_TRUNK_PARTS; part++) {
    if (base_y + 1 + part < MAX_Y) {
      tree->trunk_mask |= 1 << part;
    }
  }
  if (!treeRootInsert(TREE_ROOT_KEY(x, z), i + 1)) {
    tree->base_y = TREE_INACTIVE_Y;
    return TREE_NONE;
  }
  return i;
}

void treeAddLeaf(u8 tree_index, u8 x, u8 y, u8 z) {
  TreeRecord *tree;
  int local_x, local_y, local_z;
  u8 bit;

  if (tree_index >= MAX_TREES) {
    return;
  }
  tree = &trees[tree_index];
  /* x and tree->x are both the low byte of an unbounded world coordinate, so
     the offset must be taken in that same width.  Widening first and
     subtracting in int gives the wrong answer wherever the two straddle a
     256-block wrap, which a fixed world could never reach. */
  local_x = (u8) (x - tree->x + 2);
  local_y = y - tree->canopy_y;
  local_z = (u8) (z - tree->z + 2);
  if (local_x >= 5 || local_y < 0 ||
      local_y >= TREE_LEAF_LAYERS || local_z >= 5) {
    return;
  }
  bit = local_y * 25 + local_x * 5 + local_z;
  setLeafBit(tree, bit, TRUE);
}

static u8 treeOwnsLeafAt(TreeRecord *tree, u8 x, u8 y, u8 z, u8 *bit_out) {
  /* Same wrapping offset as treeAddLeaf; see the note there. */
  int local_x = (u8) (x - tree->x + 2);
  int local_y = y - tree->canopy_y;
  int local_z = (u8) (z - tree->z + 2);
  u8 bit;

  if (local_x >= 5 || local_y < 0 ||
      local_y >= TREE_LEAF_LAYERS || local_z >= 5) {
    return FALSE;
  }
  bit = local_y * 25 + local_x * 5 + local_z;
  if (!leafBitSet(tree, bit)) {
    return FALSE;
  }
  *bit_out = bit;
  return TRUE;
}

static u8 leafOwnedByExistingTree(u8 x, u8 y, u8 z) {
  u8 dx, dz;
  int root_x, root_z;

  for (dx = 0; dx < 5; dx++) {
    /* Any coordinate is a safe lookup because the sparse index folds roots
       into the window's coordinate wrap, so far trees are still found. */
    root_x = x - 2 + dx;
    for (dz = 0; dz < 5; dz++) {
      u8 index;
      u8 bit;
      root_z = z - 2 + dz;
      index = treeIndexAtRoot(root_x, root_z);
      if (index != TREE_NONE &&
          treeOwnsLeafAt(&trees[index], x, y, z, &bit)) {
        return TRUE;
      }
    }
  }
  return FALSE;
}

/* Version 4 worlds did not save tree ownership.  Recover only intact trees
   with the generator's distinctive dirt base, 3-5 log trunk, and substantial
   canopy.  This avoids treating ordinary player-built log columns as trees.
   Scanning in the original x/z generation order also gives overlapping
   canopy cells to the tree that would have placed them first. */
void recoverTreesFromWorld() {
  u8 x, z, base_y;

  initTrees();
  for (x = 0; x < MAX_X; x++) {
    for (z = 0; z < MAX_Z; z++) {
      for (base_y = 0; base_y + 1 < MAX_Y; base_y++) {
        u8 height = 0;
        u8 leaves = 0;
        u8 tree_index;
        u8 local_x, local_y, local_z;

        if (blockGet(x, base_y, z) != DIRT ||
            blockGet(x, base_y + 1, z) != WOOD) {
          continue;
        }
        while (height < TREE_TRUNK_PARTS && base_y + 1 + height < MAX_Y &&
            blockGet(x, base_y + 1 + height, z) == WOOD) {
          height++;
        }
        if (height < 3 || (height == TREE_TRUNK_PARTS &&
            base_y + 1 + height < MAX_Y &&
            blockGet(x, base_y + 1 + height, z) == WOOD)) {
          continue;
        }

        for (local_x = 0; local_x < 5; local_x++) {
          for (local_z = 0; local_z < 5; local_z++) {
            int leaf_x = x + local_x - 2;
            int leaf_z = z + local_z - 2;
            if (leaf_x < 0 || leaf_x >= MAX_X || leaf_z < 0 ||
                leaf_z >= MAX_Z) {
              continue;
            }
            for (local_y = 0; local_y < TREE_LEAF_LAYERS; local_y++) {
              u8 leaf_y = base_y + height - 1 + local_y;
              if (leaf_y < MAX_Y &&
                  blockGet(leaf_x, leaf_y, leaf_z) == LEAVES) {
                leaves++;
              }
            }
          }
        }
        if (leaves < 12) {
          continue;
        }

        tree_index = createTree(x, z, base_y, height);
        if (tree_index == TREE_NONE) {
          return;
        }
        for (local_x = 0; local_x < 5; local_x++) {
          for (local_z = 0; local_z < 5; local_z++) {
            int leaf_x = x + local_x - 2;
            int leaf_z = z + local_z - 2;
            if (leaf_x < 0 || leaf_x >= MAX_X || leaf_z < 0 ||
                leaf_z >= MAX_Z) {
              continue;
            }
            for (local_y = 0; local_y < TREE_LEAF_LAYERS; local_y++) {
              u8 leaf_y = base_y + height - 1 + local_y;
              if (leaf_y < MAX_Y &&
                  blockGet(leaf_x, leaf_y, leaf_z) == LEAVES &&
                  !leafOwnedByExistingTree(leaf_x, leaf_y, leaf_z)) {
                treeAddLeaf(tree_index, leaf_x, leaf_y, leaf_z);
              }
            }
          }
        }
        break;
      }
    }
  }
}

static void discardMissingParts(TreeRecord *tree) {
  u8 part, bit;
  int root_x, root_z;

  if (!treeAbsoluteRoot(tree, &root_x, &root_z)) {
    /* No column, no blocks: nothing this record describes still exists. */
    tree->trunk_mask = 0;
    return;
  }
  for (part = 0; part < TREE_TRUNK_PARTS; part++) {
    if ((tree->trunk_mask & (1 << part)) &&
        blockGet(root_x, tree->base_y + 1 + part, root_z) != WOOD) {
      tree->trunk_mask &= ~(1 << part);
    }
  }
  for (bit = 0; bit < TREE_LEAF_BITS; bit++) {
    if (leafBitSet(tree, bit)) {
      u8 local_x = (bit % 25) / 5;
      u8 local_z = bit % 5;
      u8 local_y = bit / 25;
      int x = root_x + local_x - 2;
      u8 y = tree->canopy_y + local_y;
      int z = root_z + local_z - 2;
      if (blockGet(x, y, z) != LEAVES) {
        setLeafBit(tree, bit, FALSE);
      }
    }
  }
}

static u8 treeHasLogAbove(TreeRecord *tree, u8 part) {
  u8 above_mask;

  if (part + 1 >= TREE_TRUNK_PARTS) {
    return FALSE;
  }
  above_mask = tree->trunk_mask & ~((1 << (part + 1)) - 1);
  return above_mask != 0;
}

/*
 * The fixed-extent changed-column array this used to fill was indexed with
 * the record's raw u8 coordinates -- 0..255 against 196 entries, a stack
 * overflow for any root past block 111, and its remesh loop then walked the
 * old 14x14 world.  Per-block dirty marking replaces the array outright:
 * makeDisplayListsAt is a couple of flag stores, is idempotent, and already
 * knows which neighbouring column shares each seam.
 */
static void removeTreeBlocks(TreeRecord *tree) {
  u8 part, bit;
  int root_x, root_z;

  if (!treeAbsoluteRoot(tree, &root_x, &root_z)) {
    return;
  }
  for (part = 0; part < TREE_TRUNK_PARTS; part++) {
    if (tree->trunk_mask & (1 << part)) {
      u8 y = tree->base_y + 1 + part;
      blockSet(root_x, y, root_z, AIR);
      makeDisplayListsAt(root_x, root_z);
    }
  }
  for (bit = 0; bit < TREE_LEAF_BITS; bit++) {
    if (leafBitSet(tree, bit)) {
      u8 local_x = (bit % 25) / 5;
      u8 local_z = bit % 5;
      u8 local_y = bit / 25;
      int x = root_x + local_x - 2;
      u8 y = tree->canopy_y + local_y;
      int z = root_z + local_z - 2;
      blockSet(x, y, z, AIR);
      makeDisplayListsAt(x, z);
    }
  }
}

u8 beginTreeFelling(u8 x, u8 y, u8 z) {
  u8 index = treeIndexAtRoot(x, z);
  TreeRecord *tree;
  u8 part;

  if (index == TREE_NONE) {
    return FALSE;
  }
  tree = &trees[index];
  if (tree->state != TREE_STATE_STANDING || y <= tree->base_y ||
      y > tree->base_y + TREE_TRUNK_PARTS) {
    return FALSE;
  }
  part = y - tree->base_y - 1;
  if (!(tree->trunk_mask & (1 << part))) {
    return FALSE;
  }
  discardMissingParts(tree);
  if (!treeHasLogAbove(tree, part)) {
    return FALSE;
  }

  tree->state = TREE_STATE_FALLING;
  tree->fall_direction = random(4);
  tree->fall_progress = 0;
  tree->debris_cursor = 0;
  removeTreeBlocks(tree);
  return TRUE;
}

void treeBlockDestroyed(u8 x, u8 y, u8 z) {
  u8 index = treeIndexAtRoot(x, z);
  u8 dx, dz;
  int root_x, root_z;
  u8 leaf_bit;

  if (index != TREE_NONE) {
    TreeRecord *tree = &trees[index];
    if (tree->state == TREE_STATE_STANDING && y > tree->base_y &&
        y <= tree->base_y + TREE_TRUNK_PARTS) {
      u8 part = y - tree->base_y - 1;
      tree->trunk_mask &= ~(1 << part);
      return;
    }
  }

  /* Only tree roots within two horizontal blocks can own this leaf. */
  for (dx = 0; dx < 5; dx++) {
    /* Any coordinate is a safe lookup because the sparse index folds roots
       into the window's coordinate wrap, so far trees are still found. */
    root_x = x - 2 + dx;
    for (dz = 0; dz < 5; dz++) {
      root_z = z - 2 + dz;
      index = treeIndexAtRoot(root_x, root_z);
      if (index != TREE_NONE && trees[index].state == TREE_STATE_STANDING &&
          treeOwnsLeafAt(&trees[index], x, y, z, &leaf_bit)) {
        setLeafBit(&trees[index], leaf_bit, FALSE);
        return;
      }
    }
  }
}

static u8 emitDebris(TreeRecord *tree) {
  int root_x, root_z;

  /* Absolute coordinates, or the pickups spawn at the wrapped u8 position --
     which past the first 256 blocks is somewhere else entirely. */
  if (!treeAbsoluteRoot(tree, &root_x, &root_z)) {
    tree->debris_cursor = TREE_DEBRIS_PARTS;
    return FALSE;
  }
  while (tree->debris_cursor < TREE_DEBRIS_PARTS) {
    u8 cursor = tree->debris_cursor++;
    int x, z;
    u8 y, item;

    if (cursor < TREE_TRUNK_PARTS) {
      if (!(tree->trunk_mask & (1 << cursor))) {
        continue;
      }
      x = root_x;
      y = tree->base_y + 1 + cursor;
      z = root_z;
      item = WOOD;
    } else {
      u8 bit = cursor - TREE_TRUNK_PARTS;
      u8 local_x, local_y, local_z;
      if (!leafBitSet(tree, bit)) {
        continue;
      }
      local_x = (bit % 25) / 5;
      local_z = bit % 5;
      local_y = bit / 25;
      x = root_x + local_x - 2;
      y = tree->canopy_y + local_y;
      z = root_z + local_z - 2;
      if (!rollLeafDrop(&item)) {
        continue;
      }
    }
    if (!spawnDroppedItem(item, 1, x, y, z)) {
      tree->debris_cursor--;
      return FALSE;
    }
    return TRUE;
  }
  return FALSE;
}

void updateTrees(float delta) {
  u8 i;

  /* Falling transforms are tiny, so advance every active animation.  The
     expensive part remains capped: only one physical pickup is emitted per
     frame across all fallen trees. */
  for (i = 0; i < MAX_TREES; i++) {
    TreeRecord *tree = &trees[i];
    if (tree->state == TREE_STATE_FALLING) {
      tree->fall_progress += delta / TREE_FALL_FRAMES;
      if (tree->fall_progress >= 1.f) {
        tree->fall_progress = 1.f;
        tree->state = TREE_STATE_DEBRIS;
      }
    }
  }

  for (i = 0; i < MAX_TREES; i++) {
    TreeRecord *tree = &trees[i];
    if (tree->state != TREE_STATE_DEBRIS) {
      continue;
    }
    if (tree->debris_cursor >= TREE_DEBRIS_PARTS) {
      retireTree(i);
      continue;
    }
    emitDebris(tree);
    break;
  }
}

u8 rebuildTreeLookup() {
  u8 i;
  u16 slot;

  for (slot = 0; slot < TREE_ROOT_HASH_SLOTS; slot++) {
    tree_root_values[slot] = 0;
  }
  for (i = 0; i < MAX_TREES; i++) {
    TreeRecord *tree = &trees[i];
    u16 root;
    if (tree->base_y == TREE_INACTIVE_Y) {
      continue;
    }
    if (tree->x >= MAX_X || tree->z >= MAX_Z || tree->base_y >= MAX_Y ||
        tree->canopy_y >= MAX_Y ||
        tree->state > TREE_STATE_DEBRIS || tree->fall_direction > 3 ||
        tree->fall_progress != tree->fall_progress || tree->fall_progress < 0 ||
        tree->fall_progress > 1 || tree->debris_cursor > TREE_DEBRIS_PARTS ||
        (tree->leaf_mask[TREE_LEAF_MASK_BYTES - 1] & 0xF0)) {
      return FALSE;
    }
    root = TREE_ROOT_KEY(tree->x, tree->z);
    if (treeIndexAtRoot(tree->x, tree->z) != TREE_NONE ||
        !treeRootInsert(root, i + 1)) {
      return FALSE;
    }
  }
  return TRUE;
}

u8 treesValid() {
  return rebuildTreeLookup();
}
