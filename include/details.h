#ifndef DETAILS_H
#define DETAILS_H

#include <nusys.h>
#include "math.h"

/* Full cubes remain nibble-packed.  The few cells whose shape or state cannot
   be represented by a cube carry one record here and use CRAFTING_TABLE (10)
   as their occupied proxy in the terrain.  An old ID-10 cell with no record
   is still an ordinary crafting table, preserving existing saves. */
#define MAX_DETAILS 384

enum DetailKind {
  DETAIL_NONE,
  DETAIL_TORCH,
  DETAIL_WOOD_STAIRS,
  DETAIL_STONE_STAIRS,
  DETAIL_WOOD_DOOR,
  DETAIL_WINDOW,
  DETAIL_KIND_COUNT
};

#define DETAIL_STATE_OPEN 0x01
#define DETAIL_FLAG_GENERATED 0x01

typedef struct {
  s32 x;
  s32 z;
  u8 y;
  u8 kind;
  u8 orientation;
  u8 state;
  u8 flags;
  u8 active;
  u8 reserved[2];
} DetailCell;

extern DetailCell details[MAX_DETAILS];
extern u16 detail_count;
/* One past the highest slot ever handed out since initDetails.  Records are
   allocated from the low end, so every pool walk can stop here instead of at
   MAX_DETAILS -- which is what keeps an empty pool genuinely free. */
extern u16 detail_scan_limit;
extern u32 detail_overflows;

void initDetails(void);
DetailCell *detailAt(int x, int y, int z);
u8 detailKindForItem(u8 item);
u8 detailItemForKind(u8 kind);
u8 detailPlace(u8 item, int x, int y, int z, u8 orientation, u8 flags);
u8 detailRemove(int x, int y, int z, u8 *drop_item);
u8 detailToggle(int x, int y, int z);
u8 detailIsCustomAt(int x, int y, int z);
u8 detailIsStairAt(int x, int y, int z);
u8 worldCellSolid(int x, int y, int z);
u8 detailLightAt(Vector3 position);
void detailsApplyColumn(int cx, int cz);
void detailsEvictGeneratedColumn(int cx, int cz);

#endif /* DETAILS_H */
