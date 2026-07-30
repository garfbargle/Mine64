#include "trees.h"

#include "blocks.h"
#include "geometry.h"
#include "graphics.h"
#include "items.h"

#define TREE_NONE 255
#define TREE_TRUNK_PARTS 5
#define TREE_LEAF_LAYERS 4
#define TREE_LEAF_BITS (25 * TREE_LEAF_LAYERS)
#define TREE_DEBRIS_PARTS (TREE_TRUNK_PARTS + TREE_LEAF_BITS)
#define TREE_FALL_FRAMES 42.f

TreeRecord trees[MAX_TREES];

/* This small table makes trunk lookup constant time.  It is derived data and
   is never written to save files. */
static u8 tree_at_root[MAX_X * MAX_Z];

static u32 blockIndex(u8 x, u8 y, u8 z) {
  return x * MAX_Y * MAX_Z + y * MAX_Z + z;
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

static u8 treeIndexAtRoot(u8 x, u8 z) {
  u8 index = tree_at_root[x * MAX_Z + z];
  return index == 0 ? TREE_NONE : index - 1;
}

static void retireTree(u8 index) {
  TreeRecord *tree = &trees[index];
  tree_at_root[tree->x * MAX_Z + tree->z] = 0;
  tree->base_y = TREE_INACTIVE_Y;
  tree->state = TREE_STATE_STANDING;
}

void initTrees() {
  u8 i;
  u32 root;

  for (i = 0; i < MAX_TREES; i++) {
    trees[i].base_y = TREE_INACTIVE_Y;
    trees[i].state = TREE_STATE_STANDING;
  }
  for (root = 0; root < MAX_X * MAX_Z; root++) {
    tree_at_root[root] = 0;
  }
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
  tree_at_root[x * MAX_Z + z] = i + 1;
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
  local_x = x - tree->x + 2;
  local_y = y - tree->canopy_y;
  local_z = z - tree->z + 2;
  if (local_x < 0 || local_x >= 5 || local_y < 0 ||
      local_y >= TREE_LEAF_LAYERS || local_z < 0 || local_z >= 5) {
    return;
  }
  bit = local_y * 25 + local_x * 5 + local_z;
  setLeafBit(tree, bit, TRUE);
}

static u8 treeOwnsLeafAt(TreeRecord *tree, u8 x, u8 y, u8 z, u8 *bit_out) {
  int local_x = x - tree->x + 2;
  int local_y = y - tree->canopy_y;
  int local_z = z - tree->z + 2;
  u8 bit;

  if (local_x < 0 || local_x >= 5 || local_y < 0 ||
      local_y >= TREE_LEAF_LAYERS || local_z < 0 || local_z >= 5) {
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
    root_x = x - 2 + dx;
    if (root_x < 0 || root_x >= MAX_X) {
      continue;
    }
    for (dz = 0; dz < 5; dz++) {
      u8 index;
      u8 bit;
      root_z = z - 2 + dz;
      if (root_z < 0 || root_z >= MAX_Z) {
        continue;
      }
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

        if (blocks[blockIndex(x, base_y, z)] != DIRT ||
            blocks[blockIndex(x, base_y + 1, z)] != WOOD) {
          continue;
        }
        while (height < TREE_TRUNK_PARTS && base_y + 1 + height < MAX_Y &&
            blocks[blockIndex(x, base_y + 1 + height, z)] == WOOD) {
          height++;
        }
        if (height < 3 || (height == TREE_TRUNK_PARTS &&
            base_y + 1 + height < MAX_Y &&
            blocks[blockIndex(x, base_y + 1 + height, z)] == WOOD)) {
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
                  blocks[blockIndex(leaf_x, leaf_y, leaf_z)] == LEAVES) {
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
                  blocks[blockIndex(leaf_x, leaf_y, leaf_z)] == LEAVES &&
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

  for (part = 0; part < TREE_TRUNK_PARTS; part++) {
    if ((tree->trunk_mask & (1 << part)) &&
        blocks[blockIndex(tree->x, tree->base_y + 1 + part, tree->z)] != WOOD) {
      tree->trunk_mask &= ~(1 << part);
    }
  }
  for (bit = 0; bit < TREE_LEAF_BITS; bit++) {
    if (leafBitSet(tree, bit)) {
      u8 local_x = (bit % 25) / 5;
      u8 local_z = bit % 5;
      u8 local_y = bit / 25;
      u8 x = tree->x + local_x - 2;
      u8 y = tree->canopy_y + local_y;
      u8 z = tree->z + local_z - 2;
      if (blocks[blockIndex(x, y, z)] != LEAVES) {
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

static void removeTreeBlocks(TreeRecord *tree) {
  u8 changed_columns[CHUNKS_X * CHUNKS_Z];
  u8 part, bit, cx, cz;

  for (cx = 0; cx < CHUNKS_X * CHUNKS_Z; cx++) {
    changed_columns[cx] = FALSE;
  }

  for (part = 0; part < TREE_TRUNK_PARTS; part++) {
    if (tree->trunk_mask & (1 << part)) {
      u8 y = tree->base_y + 1 + part;
      blocks[blockIndex(tree->x, y, tree->z)] = AIR;
      regenerateBlock(tree->x, y, tree->z);
      changed_columns[(tree->x / CHUNK_SIZE) * CHUNKS_Z + tree->z / CHUNK_SIZE] = TRUE;
    }
  }
  for (bit = 0; bit < TREE_LEAF_BITS; bit++) {
    if (leafBitSet(tree, bit)) {
      u8 local_x = (bit % 25) / 5;
      u8 local_z = bit % 5;
      u8 local_y = bit / 25;
      u8 x = tree->x + local_x - 2;
      u8 y = tree->canopy_y + local_y;
      u8 z = tree->z + local_z - 2;
      blocks[blockIndex(x, y, z)] = AIR;
      regenerateBlock(x, y, z);
      changed_columns[(x / CHUNK_SIZE) * CHUNKS_Z + z / CHUNK_SIZE] = TRUE;
    }
  }
  for (cx = 0; cx < CHUNKS_X; cx++) {
    for (cz = 0; cz < CHUNKS_Z; cz++) {
      if (changed_columns[cx * CHUNKS_Z + cz]) {
        makeDisplayListsAt(cx * CHUNK_SIZE, cz * CHUNK_SIZE);
      }
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
    root_x = x - 2 + dx;
    if (root_x < 0 || root_x >= MAX_X) {
      continue;
    }
    for (dz = 0; dz < 5; dz++) {
      root_z = z - 2 + dz;
      if (root_z < 0 || root_z >= MAX_Z) {
        continue;
      }
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
  while (tree->debris_cursor < TREE_DEBRIS_PARTS) {
    u8 cursor = tree->debris_cursor++;
    u8 x, y, z, item;

    if (cursor < TREE_TRUNK_PARTS) {
      if (!(tree->trunk_mask & (1 << cursor))) {
        continue;
      }
      x = tree->x;
      y = tree->base_y + 1 + cursor;
      z = tree->z;
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
      x = tree->x + local_x - 2;
      y = tree->canopy_y + local_y;
      z = tree->z + local_z - 2;
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
  u32 table_index;

  for (table_index = 0; table_index < MAX_X * MAX_Z; table_index++) {
    tree_at_root[table_index] = 0;
  }
  for (i = 0; i < MAX_TREES; i++) {
    TreeRecord *tree = &trees[i];
    u32 root;
    if (tree->base_y == TREE_INACTIVE_Y) {
      continue;
    }
    if (tree->x >= MAX_X || tree->z >= MAX_Z || tree->base_y >= MAX_Y ||
        tree->canopy_y >= MAX_Y || tree->trunk_mask & ~0x1F ||
        tree->state > TREE_STATE_DEBRIS || tree->fall_direction > 3 ||
        tree->fall_progress != tree->fall_progress || tree->fall_progress < 0 ||
        tree->fall_progress > 1 || tree->debris_cursor > TREE_DEBRIS_PARTS ||
        (tree->leaf_mask[TREE_LEAF_MASK_BYTES - 1] & 0xF0)) {
      return FALSE;
    }
    root = tree->x * MAX_Z + tree->z;
    if (tree_at_root[root] != 0) {
      return FALSE;
    }
    tree_at_root[root] = i + 1;
  }
  return TRUE;
}

u8 treesValid() {
  return rebuildTreeLookup();
}
