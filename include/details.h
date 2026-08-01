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
  DETAIL_FENCE,
  DETAIL_FENCE_GATE,
  DETAIL_KIND_COUNT
};

#define DETAIL_STATE_OPEN 0x01
#define DETAIL_FLAG_GENERATED 0x01

/*
 * Which neighbouring cells a fence has run a rail to, cached in the record.
 *
 * The renderer cannot work this out for itself.  Asking detailAt about four
 * neighbours per visible fence per frame is up to 24 x 4 pool scans a frame
 * per viewport -- the pool walk that docs/streaming-world-plan.md warns is
 * how the mesher once lost hundreds of millions of iterations.  A fence line
 * only changes when one is placed or broken, so the mask is computed there
 * and the four neighbours are updated with it: O(1) per edit, nothing per
 * frame.
 */
#define DETAIL_LINK_NEG_X 0x01
#define DETAIL_LINK_POS_X 0x02
#define DETAIL_LINK_NEG_Z 0x04
#define DETAIL_LINK_POS_Z 0x08

typedef struct {
  s32 x;
  s32 z;
  u8 y;
  u8 kind;
  u8 orientation;
  u8 state;
  u8 flags;
  u8 active;
  /* DETAIL_LINK_* mask; meaningful for DETAIL_FENCE only. */
  u8 links;
  u8 reserved;
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

/*
 * TRUE for the kinds that claim the cell above their root as well as the root
 * itself, which a door has always done.
 *
 * A fence is the reason this is worth naming.  Its upper cell is never drawn,
 * but it is genuinely occupied, and that one fact is what makes a fence a
 * fence: BLOCK_IS_SOLID stops the player's box at head height so a jump
 * cannot clear it, tryVault's raised test lands inside it so L+R cannot
 * mantle it, and mobCanStandAt's `blockGet(x, ground_y + 1, z) == AIR`
 * requirement fails so no animal can step into or over it.  None of those
 * three rules needed a line of fence-specific code.
 */
u8 detailKindIsTall(u8 kind);
u8 detailItemIsTall(u8 item);
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
