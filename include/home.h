#ifndef HOME_H
#define HOME_H

#include <nusys.h>

#include "world.h"

/*
 * A dense, permanent copy of the fixed save extent.
 *
 * The block window is a scrolling cache: walk far enough and a column is
 * recycled, and coming back regenerates it from its coordinates and the seed.
 * That is what makes an endless world affordable, and it is also why player
 * changes needed somewhere else to live.  A sparse list of deviations was the
 * cheap answer, and it bought a ceiling -- a few thousand blocks, after which
 * building stopped working.
 *
 * The extent is small enough to just keep.  112 x 32 x 112 blocks at a nibble
 * each is 196 KiB, so every cell of the saveable world can hold whatever the
 * player put there and no list of exceptions is needed at all.  Editing
 * inside the extent has no budget to run out of.
 *
 * Outside the extent nothing is retained.  The save format packs exactly this
 * region, so a change out there could never have reached the cartridge; it is
 * applied to the window, stands until the column is recycled, and is gone.
 *
 * The store is also what the save reads.  Writing a save used to require the
 * whole extent to be resident at once -- walk away from spawn and saving
 * refused, because blockGet had nothing to answer with.  A column's snapshot
 * is here whether or not it is currently in the window.
 */

#define HOME_COLUMNS (CHUNKS_X * CHUNKS_Z)

extern u8 home_blocks[HOME_COLUMNS][COLUMN_BLOCK_BYTES];
/* A column with no snapshot yet has never finished decorating.  Distinct from
   an all-air column, which is a legitimate snapshot. */
extern u8 home_present[HOME_COLUMNS];

void initHome(void);

/* TRUE and *out set when (cx, cz) is an extent column. */
u8 homeColumnIndex(int cx, int cz, u16 *out);

/*
 * Copy a column's snapshot back over it, where it finishes decorating.  This
 * is what makes a rebuilt column look the way the player left it.  A column
 * with no snapshot yet is left as generated.
 *
 * It has to run after decoration rather than before: trees write canopy into
 * neighbouring columns, so a restore done any earlier would be overwritten by
 * a neighbour's leaves.
 */
void homeRestoreColumn(int cx, int cz);

/*
 * Snapshot the whole extent, once initial generation has finished it.
 *
 * Capture cannot be folded into homeRestoreColumn the way it first appears it
 * should.  A column is decorated before its neighbours are, and their trees
 * still have canopy to write into it, so a snapshot taken at that moment is
 * missing blocks that the column will have moments later.  Restoring it after
 * that would delete them -- and because it depends on which neighbour
 * decorated first, the same world would build differently depending on the
 * order the streamer happened to visit it in.
 */
void homeCaptureExtent(void);

/*
 * The two halves of protecting neighbouring columns from a pass that writes
 * past its own column, which is what the tree canopy does -- leaves reach two
 * blocks out, so a column sitting quietly in the window can have blocks
 * stamped over it by a neighbour being rebuilt.
 *
 * Called either side of that pass:
 *
 *   homeFlushDirtyNeighbours  captures any neighbour holding edits that are
 *                             still only in the window, so its snapshot is
 *                             current before anything can overwrite them.
 *   homeResyncNeighbours      copies the snapshots back afterwards, undoing
 *                             whatever the pass wrote into them.
 *
 * Both are needed.  Restoring without flushing first would push a stale
 * snapshot over a column the player has been building in -- which is worse
 * than the canopy ever was.
 */
void homeFlushDirtyNeighbours(int cx, int cz);
void homeResyncNeighbours(int cx, int cz);

/* Note that a column's window copy now differs from its snapshot.  Called for
   every player edit inside the extent; the flush that clears it is what makes
   the store current again. */
void homeMarkDirty(int x, int z);

/* Called where a column leaves the window, to catch edits made since it was
   restored.  No-op outside the extent. */
void homeFlushColumn(int cx, int cz);

/* Bring every resident extent column's snapshot up to date.  A save reads the
   store, so anything still only in the window has to be folded in first. */
void homeFlushResident(void);

/* Block accessors against the store rather than the window, for the save and
   load paths, which run over the whole extent regardless of residency. */
u8 homeBlockGet(int x, int y, int z);
void homeBlockSet(int x, int y, int z, u8 block);
/* Mark the whole extent as carrying a snapshot, after a load has filled it. */
void homeMarkAllPresent(void);

#endif /* HOME_H */
