/*
 * Do the player's changes to the world survive the block window recycling
 * them, and is there still a budget to run out of?
 *
 * The window is a scrolling cache: walk far enough and a column is reused for
 * somewhere else, and coming back rebuilds it from its coordinates and the
 * seed.  Edits used to live in a sparse pool of deviations with a few thousand
 * slots.  Now the whole save extent is kept in the home store, so the question
 * is no longer how many edits fit -- it is whether the store is written and
 * read back at the right moments.
 *
 * Every check therefore ends the same way: force the column out of the window,
 * bring it back, and ask whether the world still looks the way it was left.
 *
 * Build and run:  tools/edittest/run.sh
 */
#include <nusys.h>

#include "blocks.h"
#include "edits.h"
#include "home.h"
#include "math.h"
#include "mods.h"
#include "world.h"

int printf(const char *format, ...);

static int failures;

static void check(const char *what, int ok) {
  printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    failures++;
  }
}

static void decorate(int cx, int cz) {
  int guard;

  worldGenerateColumnTerrain(cx, cz);
  for (guard = 0; guard < 8; guard++) {
    if (worldAdvanceColumnDecoration(cx, cz)) {
      return;
    }
  }
  printf("    column %d,%d never finished decorating\n", cx, cz);
  failures++;
}

/*
 * Push a column out of the window and pull it back.
 *
 * Slots are addressed by the low bits of the chunk coordinates, so claiming
 * the column WINDOW_COLUMNS away lands on the same slot and evicts this one --
 * the same thing walking that far does, without the walk.
 */
static void evictAndReturn(int cx, int cz) {
  decorate(cx + WINDOW_COLUMNS, cz);
  decorate(cx, cz);
}

static void generateWorld(u32 seed) {
  gentestSetTime(seed);
  beginWorldGeneration(seed);
  while (!stepWorldGeneration(64)) {
  }
}

/* A cell inside the extent holding something other than `block`, so writing
   `block` there is always a real change. */
static int findCell(int cx, int cz, u8 block, int *out_x, int *out_y,
    int *out_z) {
  int bx, bz, by;

  for (bx = 0; bx < CHUNK_SIZE; bx++) {
    for (bz = 0; bz < CHUNK_SIZE; bz++) {
      for (by = 1; by < MAX_Y; by++) {
        int x = cx * CHUNK_SIZE + bx;
        int z = cz * CHUNK_SIZE + bz;

        if (blockGet(x, by, z) != block && worldEditCanSet(x, by, z)) {
          *out_x = x;
          *out_y = by;
          *out_z = z;
          return 1;
        }
      }
    }
  }
  return 0;
}

static void testSurvivesRecycling(void) {
  int x, y, z;

  printf("\nAn edit survives the window recycling its column\n");
  check("a target cell was found", findCell(3, 4, BRICKS, &x, &y, &z));
  check("the edit is accepted", worldEditSet(x, y, z, BRICKS));
  check("the world shows it immediately", blockGet(x, y, z) == BRICKS);

  evictAndReturn(3, 4);
  check("it is still there after the column comes back",
    blockGet(x, y, z) == BRICKS);
}

/* Breaking a block is an edit like any other, and the one most likely to be
   quietly undone by regeneration putting the terrain back. */
static void testRemovalSurvives(void) {
  int x, y, z;

  printf("\nA removed block stays removed\n");
  check("a solid cell was found", findCell(6, 3, AIR, &x, &y, &z));
  check("the removal is accepted", worldEditSet(x, y, z, AIR));
  evictAndReturn(6, 3);
  check("the cell is still empty", blockGet(x, y, z) == AIR);
}

/*
 * The point of the whole change: build far more than the old pool could hold.
 *
 * Sixteen columns filled solid over eleven levels is 11,264 changed cells,
 * against a pool that used to stop at 2,048.  Nothing here is expected to be
 * refused, and all of it has to come back.
 */
static void testNoBudget(void) {
  int cx, cz, bx, bz, y;
  int written = 0;
  int refused = 0;
  int wrong = 0;

  printf("\nBuilding is not rationed\n");
  for (cx = 2; cx < 6; cx++) {
    for (cz = 2; cz < 6; cz++) {
      for (bx = 0; bx < CHUNK_SIZE; bx++) {
        for (bz = 0; bz < CHUNK_SIZE; bz++) {
          for (y = 10; y < 21; y++) {
            int x = cx * CHUNK_SIZE + bx;
            int z = cz * CHUNK_SIZE + bz;

            if (worldEditSet(x, y, z, BRICKS)) {
              written++;
            } else {
              refused++;
            }
          }
        }
      }
    }
  }
  printf("    %d cells written, %d refused\n", written, refused);
  check("every cell was accepted", refused == 0);
  check("far more than the old 2048-slot pool", written == 4 * 4 * 64 * 11);

  for (cx = 2; cx < 6; cx++) {
    for (cz = 2; cz < 6; cz++) {
      evictAndReturn(cx, cz);
    }
  }
  for (cx = 2; cx < 6; cx++) {
    for (cz = 2; cz < 6; cz++) {
      for (bx = 0; bx < CHUNK_SIZE; bx++) {
        for (bz = 0; bz < CHUNK_SIZE; bz++) {
          for (y = 10; y < 21; y++) {
            if (blockGet(cx * CHUNK_SIZE + bx, y, cz * CHUNK_SIZE + bz) !=
                BRICKS) {
              wrong++;
            }
          }
        }
      }
    }
  }
  check("all of it survives recycling", wrong == 0);
}

/*
 * Canopy from a neighbour reaches two blocks past its own column, so a column
 * restored from its snapshot can have leaves stamped back over it by a
 * neighbour rebuilt afterwards.  Deterministic terrain would survive that --
 * the same leaves land in the same places -- but the player's edit would not.
 */
static void testNeighbourCanopy(void) {
  int x, y, z;
  int cx = 8;
  int cz = 8;

  printf("\nA neighbour rebuilding does not overwrite an edit\n");
  check("a target cell was found", findCell(cx, cz, BRICKS, &x, &y, &z));
  check("the edit is accepted", worldEditSet(x, y, z, BRICKS));
  evictAndReturn(cx, cz);
  check("it survives its own column's rebuild", blockGet(x, y, z) == BRICKS);

  /* Now rebuild everything around it, which is what runs the tree pass over
     the edited column's boundary again. */
  {
    int dx, dz;

    for (dx = -1; dx <= 1; dx++) {
      for (dz = -1; dz <= 1; dz++) {
        if (dx != 0 || dz != 0) {
          evictAndReturn(cx + dx, cz + dz);
        }
      }
    }
  }
  check("it survives every neighbour's rebuild too",
    blockGet(x, y, z) == BRICKS);
}

/*
 * The save reads the home store rather than the window, which is what retired
 * the old "too far from spawn to save" refusal.  The store therefore has to
 * agree with the window for a column that is still resident, and has to keep
 * answering for one that is not.
 */
static void testStoreMatchesWindow(void) {
  int x, y, z;
  int mismatches = 0;
  int cx, cz, bx, bz;

  printf("\nThe store is what the save will write\n");
  check("a target cell was found", findCell(9, 9, BRICKS, &x, &y, &z));
  worldEditSet(x, y, z, BRICKS);

  /* An edit made since the column was restored is only in the window until
     this runs; the save calls it for exactly that reason. */
  homeFlushResident();
  check("the store has the fresh edit", homeBlockGet(x, y, z) == BRICKS);

  for (cx = 0; cx < CHUNKS_X; cx++) {
    for (cz = 0; cz < CHUNKS_Z; cz++) {
      for (bx = 0; bx < CHUNK_SIZE; bx += 3) {
        for (bz = 0; bz < CHUNK_SIZE; bz += 3) {
          int wx = cx * CHUNK_SIZE + bx;
          int wz = cz * CHUNK_SIZE + bz;
          int wy;

          for (wy = 0; wy < MAX_Y; wy += 5) {
            if (homeBlockGet(wx, wy, wz) != blockGet(wx, wy, wz)) {
              mismatches++;
            }
          }
        }
      }
    }
  }
  check("store and window agree across the extent", mismatches == 0);

  /* Walk the column out of the window; the store must still answer. */
  decorate(9 + WINDOW_COLUMNS, 9);
  check("the store answers for an evicted column",
    homeBlockGet(x, y, z) == BRICKS);
  decorate(9, 9);
}

/*
 * Outside the extent a change is applied but not retained.  It is not refused
 * -- the player sees it happen -- it simply does not outlive the column, which
 * is all the save format could ever have carried.
 */
static void testOutsideExtentIsTransient(void) {
  int cx = CHUNKS_X + 3;
  int cz = 5;
  int x = cx * CHUNK_SIZE + 1;
  int z = cz * CHUNK_SIZE + 1;
  int y;
  int placed = 0;
  int survived = 0;

  printf("\nOutside the extent, changes are transient by design\n");
  decorate(cx, cz);
  for (y = 1; y < MAX_Y; y++) {
    if (blockGet(x, y, z) != BRICKS && worldEditSet(x, y, z, BRICKS)) {
      placed++;
    }
  }
  check("the edits are accepted", placed > 0);

  evictAndReturn(cx, cz);
  for (y = 1; y < MAX_Y; y++) {
    if (blockGet(x, y, z) == BRICKS) {
      survived++;
    }
  }
  /* The regenerated column may legitimately contain bricks of its own only if
     the generator puts them there, which it does not at these coordinates. */
  check("none of them outlive the column", survived == 0);
}

/* A world is not allowed to inherit the previous one's blocks.  The store is
   the whole extent, so a stale one would hand a rerolled seed the terrain it
   was asked to replace. */
static void testNewWorldClearsStore(void) {
  int x, y, z;
  int differs = 0;
  int bx, by, bz;

  printf("\nA new world does not inherit the old one's store\n");
  check("a target cell was found", findCell(4, 4, BRICKS, &x, &y, &z));
  worldEditSet(x, y, z, BRICKS);
  homeFlushResident();

  generateWorld(999u);
  for (bx = 0; bx < MAX_X; bx += 7) {
    for (bz = 0; bz < MAX_Z; bz += 7) {
      for (by = 0; by < MAX_Y; by += 3) {
        if (homeBlockGet(bx, by, bz) != blockGet(bx, by, bz)) {
          differs++;
        }
      }
    }
  }
  check("the store matches the world that was just generated", differs == 0);
}

int main(void) {
  printf("Mine64 edit persistence checks\n");
  printf("  extent %dx%dx%d, %d columns, store %d KiB\n",
    MAX_X, MAX_Y, MAX_Z, HOME_COLUMNS,
    (int) (HOME_COLUMNS * COLUMN_BLOCK_BYTES / 1024));

  generateWorld(20250801u);
  testSurvivesRecycling();
  testRemovalSurvives();
  testNoBudget();
  testNeighbourCanopy();
  testStoreMatchesWindow();
  testOutsideExtentIsTransient();
  testNewWorldClearsStore();

  if (failures != 0) {
    printf("\n%d check(s) failed\n", failures);
    return 1;
  }
  printf("\nall checks passed\n");
  return 0;
}
