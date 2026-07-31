#ifndef EDITS_H
#define EDITS_H

#include <nusys.h>

/* The terrain itself remains procedural and nibble-packed.  Only deviations
   are retained across window eviction and saved, so an effectively infinite
   world does not imply effectively infinite RDRAM. */
#define MAX_WORLD_EDITS 2048

typedef struct {
  s32 x;
  s32 z;
  u8 y;
  u8 block;
  u8 active;
  u8 reserved;
} WorldEdit;

extern WorldEdit world_edits[MAX_WORLD_EDITS];
extern u16 world_edit_count;
extern u32 world_edit_overflows;

void initWorldEdits(void);
u8 worldEditCanSet(int x, int y, int z);
u8 worldEditSet(int x, int y, int z, u8 block);
void worldApplyEditsToColumn(int cx, int cz);

#endif /* EDITS_H */
