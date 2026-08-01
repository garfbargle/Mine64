#ifndef EDITS_H
#define EDITS_H

#include <nusys.h>

#include "world.h"

/*
 * Where a player's change to the world goes.
 *
 * This used to be a pool of deviation records, because the block window is a
 * scrolling cache and a column that falls out of it is regenerated from its
 * coordinates rather than remembered.  The pool had a ceiling, and the ceiling
 * was low enough to reach by building a house.
 *
 * Inside the fixed save extent there is no pool any more: the extent is kept
 * whole in the home store (see home.h), so a change is just a block write and
 * every one of its 401,408 cells can differ from what the generator produces.
 * Nothing to run out of, and nothing to count.
 *
 * Outside the extent a change is transient by design.  It is applied to the
 * window and stands until the column is recycled.  The save format packs
 * exactly the extent, so it could never have been carried across a save
 * anyway, and pretending otherwise would only mean losing it later instead.
 */

void initWorldEdits(void);
/* TRUE when a cell is inside the fixed save extent, and so is retained rather
   than transient. */
u8 worldEditInExtent(int x, int y, int z);
/* TRUE when the cell can be written at all: its column has to be resident and
   fully decorated, or the write races the generator still filling it in. */
u8 worldEditCanSet(int x, int y, int z);
u8 worldEditSet(int x, int y, int z, u8 block);

#endif /* EDITS_H */
