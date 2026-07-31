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
