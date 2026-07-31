#ifndef TREES_H
#define TREES_H

#include <nusys.h>

#include "world.h"

/* A tree owns only the blocks it placed while generating.  The canopy mask is
   a packed 5 x 5 x 4 volume, so overlapping canopies and player-built logs
   never become part of the same tree. */
#define MAX_TREES 96
#define TREE_INACTIVE_Y 255
#define TREE_LEAF_MASK_BYTES 13

#define TREE_STATE_STANDING 0
#define TREE_STATE_FALLING  1
#define TREE_STATE_DEBRIS   2

typedef struct {
  u8 x;
  u8 z;
  u8 base_y;
  u8 trunk_mask;
  u8 canopy_y;
  u8 state;
  u8 fall_direction;
  u8 debris_cursor;
  float fall_progress;
  u8 leaf_mask[TREE_LEAF_MASK_BYTES];
} TreeRecord;

extern TreeRecord trees[MAX_TREES];

void initTrees();
/* Release the trees rooted in a column the window is about to rebind.  The
   record pool is small and fixed, so a walk that never gave trees back would
   exhaust it and quietly stop growing new ones. */
void treesEvictColumn(int cx, int cz);
/* Retire records rooted outside the fixed save extent before writing the pool
   to disk; treesValid rejects their coordinates on load, so keeping one would
   make the whole save read as corrupt on the next boot. */
void treesDropOutsideFixedExtent();
void recoverTreesFromWorld();
u8 createTree(u8 x, u8 z, u8 base_y, u8 height);
void treeAddLeaf(u8 tree_index, u8 x, u8 y, u8 z);

/* Call before the normal one-block break.  TRUE means the tree took over the
   break and has queued all of its remaining individual block drops. */
u8 beginTreeFelling(u8 x, u8 y, u8 z);

/* Keep the ownership record current when a single trunk or leaf is mined. */
void treeBlockDestroyed(u8 x, u8 y, u8 z);

/* Animates the cheap rigid fall, then streams one queued cube per frame into
   the bounded pickup pool. */
void updateTrees(float delta);

/* Save/load helpers.  Saved records include partially harvested trees and
   in-progress debris queues; the root lookup table is rebuilt after loading. */
u8 rebuildTreeLookup();
u8 treesValid();

#endif /* TREES_H */
