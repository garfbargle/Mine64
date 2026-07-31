/*
 * Host stand-ins for the parts of the game world.c talks to but that say
 * nothing about whether generation is reproducible.  Tree records track
 * ownership for felling; the blocks a tree places go through blockSet either
 * way, and blocks are what this harness compares.
 */
#include <nusys.h>

#include "math.h"
#include "trees.h"

TreeRecord trees[MAX_TREES];

static u64 gentest_time;

void gentestSetTime(u64 time) {
  gentest_time = time;
}

u64 osGetTime(void) {
  return gentest_time;
}

void initTrees() {
}

/* Eviction only releases tree records, which this harness does not keep, so
   doing nothing here leaves the blocks it does compare untouched. */
void treesEvictColumn(int cx, int cz) {
  (void) cx; (void) cz;
}

u8 createTree(u8 x, u8 z, u8 base_y, u8 height) {
  (void) x; (void) z; (void) base_y; (void) height;
  return 0;
}

void treeAddLeaf(u8 tree_index, u8 x, u8 y, u8 z) {
  (void) tree_index; (void) x; (void) y; (void) z;
}

void setDayCycleWorldTicks(u32 ticks) {
  (void) ticks;
}

/* Display lists do not exist here, so residency bookkeeping has nothing to
   invalidate -- the blocks this harness compares are unaffected. */
void graphicsInvalidateColumnSlot(unsigned int slot) {
  (void) slot;
}

void graphicsMarkColumnDirty(int cx, int cz) {
  (void) cx; (void) cz;
}

/* Nothing is ever compiled here, so no column is ever waiting on geometry. */
unsigned char graphicsColumnNeedsMesh(int cx, int cz) {
  (void) cx; (void) cz;
  return 0;
}

/* The render origin only affects matrices, which this harness has none of. */
void graphicsSetRenderOrigin(int block_x, int block_z) {
  (void) block_x; (void) block_z;
}
