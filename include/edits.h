#ifndef EDITS_H
#define EDITS_H

#include <nusys.h>

#include "world.h"

/*
 * The terrain itself remains procedural and nibble-packed.  Only deviations
 * are retained across window eviction and saved, so an effectively infinite
 * world does not imply effectively infinite RDRAM.
 *
 * Retained edits are confined to the fixed save extent.  That is not a new
 * restriction on where the player may build -- outside the extent a change is
 * still written straight into the block window and stands until the column is
 * evicted -- it is a statement about what survives.  The save format packs
 * exactly the fixed extent, so an edit past it could never have reached the
 * cartridge anyway; denying it a pool slot only stops exploration from
 * consuming the budget that building near spawn depends on.
 *
 * Being extent-bound is what shrinks a record from twelve bytes to four.  The
 * two s32 coordinates existed to address the infinite world; inside the extent
 * x and z span 0..111 and y spans 0..31, so the whole cell address is nineteen
 * bits and the record fits in a u32 with room for two block IDs.
 */

/* Live records occupy [0, world_edit_count) with no holes, sorted by column
   bucket and then by cell key.  There is no active flag and no high-water
   mark: insertion and removal keep the array compact. */
typedef u32 WorldEdit;

#define MAX_WORLD_EDITS 6144
#define EDIT_COLUMNS (CHUNKS_X * CHUNKS_Z)

/*
 * Cell key: x(7) | y(5) | z(7), laid out as bitfields rather than a flat
 * (x * MAX_Y + y) * MAX_Z + z index so that decoding costs shifts instead of
 * two divisions.  worldApplyEditsToColumn decodes every record of a column
 * each time that column regenerates, and MAX_Z is not a power of two.
 */
#define EDIT_KEY(x, y, z)          \
  (((((u32) (x)) & 0x7Fu) << 12) | \
   ((((u32) (y)) & 0x1Fu) << 7) |  \
    (((u32) (z)) & 0x7Fu))

/* record: key(19) | original(4) | block(4).  Bits 27..31 are unused. */
#define EDIT_MAKE(key, original, block)                  \
  (((u32) (key) << 8) | (((u32) (original) & 0x0Fu) << 4) | \
    ((u32) (block) & 0x0Fu))
#define EDIT_KEY_OF(record) ((record) >> 8)
#define EDIT_X(record) (((record) >> 20) & 0x7Fu)
#define EDIT_Y(record) (((record) >> 15) & 0x1Fu)
#define EDIT_Z(record) (((record) >> 8) & 0x7Fu)
/* What the generator put in the cell before the player first touched it.
   Costs four bits and buys slot reclamation: an edit that puts the original
   block back is not a deviation any more, so its slot is released rather than
   spent forever on a no-op. */
#define EDIT_ORIGINAL(record) (((record) >> 4) & 0x0Fu)
#define EDIT_BLOCK(record) ((record) & 0x0Fu)

extern WorldEdit world_edits[MAX_WORLD_EDITS];
/* Prefix offsets into world_edits, one per extent column plus a terminator,
   so replaying a column visits only that column's records.  This is what
   removes the pool ceiling: worldApplyEditsToColumn runs for every column the
   streamer decorates and used to walk the whole pool to do it. */
extern u16 world_edit_column_first[EDIT_COLUMNS + 1];
extern u16 world_edit_count;
extern u32 world_edit_overflows;
/* Frames left to show the "build limit" notice.  A refused edit was otherwise
   indistinguishable from a dropped controller input. */
extern u8 world_edit_full_message;

void initWorldEdits(void);
/* TRUE when a cell is inside the fixed save extent, and so can hold a
   retained edit rather than a transient one. */
u8 worldEditInExtent(int x, int y, int z);
u8 worldEditCanSet(int x, int y, int z);
u8 worldEditSet(int x, int y, int z, u8 block);
void worldApplyEditsToColumn(int cx, int cz);
void worldEditTickMessage(void);

#endif /* EDITS_H */
