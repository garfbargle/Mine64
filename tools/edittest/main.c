/*
 * Does the player-edit pool keep its promises?
 *
 * The pool is a compact array sorted by column bucket, with a prefix index so
 * that replaying one column does not walk the whole thing.  That layout is
 * what lets the ceiling rise, and it is also the kind of structure that goes
 * wrong quietly: a bucket offset that drifts by one does not crash, it just
 * replays somebody else's edits into a column, and the damage only shows up
 * after the player walks away and comes back.
 *
 * So every check here ends the same way -- regenerate the column from scratch
 * and ask whether the world still looks the way the player left it.
 *
 * Build and run:  tools/edittest/run.sh
 */
#include <nusys.h>

#include "blocks.h"
#include "edits.h"
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

/*
 * Everything the pool's layout claims, tested against the pool itself.
 *
 * Records are sorted by bucket and then by key, the prefix index agrees with
 * where records actually sit, and every record is filed under the column its
 * own coordinates put it in.
 */
static int poolConsistent(void) {
  u16 bucket;
  u16 index;

  if (world_edit_column_first[0] != 0) {
    printf("    bucket 0 does not start at 0\n");
    return 0;
  }
  if (world_edit_column_first[EDIT_COLUMNS] != world_edit_count) {
    printf("    terminator %u != count %u\n",
      world_edit_column_first[EDIT_COLUMNS], world_edit_count);
    return 0;
  }
  for (bucket = 0; bucket < EDIT_COLUMNS; bucket++) {
    u16 start = world_edit_column_first[bucket];
    u16 end = world_edit_column_first[bucket + 1];

    if (start > end) {
      printf("    bucket %u runs backwards (%u..%u)\n", bucket, start, end);
      return 0;
    }
    for (index = start; index < end; index++) {
      WorldEdit record = world_edits[index];
      u16 owner = (u16) (((int) EDIT_X(record) >> CHUNK_SHIFT) * CHUNKS_Z +
        ((int) EDIT_Z(record) >> CHUNK_SHIFT));

      if (owner != bucket) {
        printf("    record %u sits in bucket %u but belongs to %u\n",
          index, bucket, owner);
        return 0;
      }
      if (index > start &&
          EDIT_KEY_OF(world_edits[index - 1]) >= EDIT_KEY_OF(record)) {
        printf("    bucket %u is not sorted at %u\n", bucket, index);
        return 0;
      }
    }
  }
  return 1;
}

/* Throw the column away and rebuild it from its coordinates and the seed, the
   way streaming does when the player walks out of range and returns. */
static void regenerateColumn(int cx, int cz) {
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

static void generateWorld(u32 seed) {
  gentestSetTime(seed);
  beginWorldGeneration(seed);
  while (!stepWorldGeneration(64)) {
  }
}

/* A cell inside the extent that currently holds something other than `block`,
   so an edit to `block` is always a real deviation. */
static int findCell(int cx, int cz, u8 block, int *out_x, int *out_y,
    int *out_z) {
  int bx, by, bz;

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

static void testBasicPersistence(void) {
  int x, y, z;

  printf("\nAn edit survives regeneration\n");
  check("a target cell was found", findCell(3, 4, BRICKS, &x, &y, &z));
  check("the edit is accepted", worldEditSet(x, y, z, BRICKS));
  check("the world shows it immediately", blockGet(x, y, z) == BRICKS);
  check("it took exactly one slot", world_edit_count == 1);
  check("the pool is consistent", poolConsistent());

  regenerateColumn(3, 4);
  check("it is still there after regeneration", blockGet(x, y, z) == BRICKS);
}

/*
 * The reclamation rule: an edit that puts the original block back is not a
 * deviation any more.  Without this, mining a block and replacing it spends a
 * slot forever, which is most of what a player does while building.
 */
static void testReclamation(void) {
  int x, y, z;
  u8 original;
  u16 before;

  printf("\nUndoing an edit returns its slot\n");
  check("a target cell was found", findCell(5, 5, BRICKS, &x, &y, &z));
  original = blockGet(x, y, z);
  before = world_edit_count;

  worldEditSet(x, y, z, BRICKS);
  check("the edit took a slot", world_edit_count == before + 1);
  worldEditSet(x, y, z, AIR);
  check("changing it again reuses the slot", world_edit_count == before + 1);
  worldEditSet(x, y, z, original);
  check("putting the original back frees the slot",
    world_edit_count == before);
  check("the pool is consistent", poolConsistent());

  regenerateColumn(5, 5);
  check("regeneration restores the original block",
    blockGet(x, y, z) == original);
}

/* Setting a cell to what it already holds is not a deviation and must not
   consume a slot -- otherwise placing a stone block against stone terrain
   quietly bills the player for it. */
static void testNoOpEdit(void) {
  int x, y, z;
  u16 before;

  printf("\nA no-op edit costs nothing\n");
  check("a target cell was found", findCell(6, 6, BRICKS, &x, &y, &z));
  before = world_edit_count;
  check("setting a cell to its own block is accepted",
    worldEditSet(x, y, z, blockGet(x, y, z)));
  check("the pool did not grow", world_edit_count == before);
}

/*
 * Many edits scattered across many columns, then every column rebuilt.  This
 * is where a drifting bucket offset shows itself: the pool stays the right
 * size and every individual insert looks fine, but a column replays the wrong
 * records.
 */
static void testManyColumns(void) {
#define SAMPLE_COUNT 900
  static int xs[SAMPLE_COUNT];
  static int ys[SAMPLE_COUNT];
  static int zs[SAMPLE_COUNT];
  static u8 blocks[SAMPLE_COUNT];
  static const u8 palette[4] = {BRICKS, PLANKS, COBBLESTONE, AIR};
  int taken = 0;
  int attempt;
  int wrong = 0;
  int cx, cz;
  int i;

  printf("\nScattered edits across the whole extent\n");
  for (attempt = 0; attempt < SAMPLE_COUNT * 12 && taken < SAMPLE_COUNT;
      attempt++) {
    int x = random(MAX_X);
    int y = 1 + random(MAX_Y - 1);
    int z = random(MAX_Z);
    u8 block = palette[random(4)];

    if (blockGet(x, y, z) == block || !worldEditCanSet(x, y, z)) {
      continue;
    }
    /* Later writes to a cell would make the expected value ambiguous. */
    for (i = 0; i < taken; i++) {
      if (xs[i] == x && ys[i] == y && zs[i] == z) {
        break;
      }
    }
    if (i < taken) {
      continue;
    }
    if (!worldEditSet(x, y, z, block)) {
      continue;
    }
    xs[taken] = x;
    ys[taken] = y;
    zs[taken] = z;
    blocks[taken] = block;
    taken++;
  }
  check("enough edits were placed", taken == SAMPLE_COUNT);
  check("the pool is consistent", poolConsistent());

  for (cx = 0; cx < CHUNKS_X; cx++) {
    for (cz = 0; cz < CHUNKS_Z; cz++) {
      regenerateColumn(cx, cz);
    }
  }
  for (i = 0; i < taken; i++) {
    if (blockGet(xs[i], ys[i], zs[i]) != blocks[i]) {
      if (wrong == 0) {
        printf("    first mismatch at %d,%d,%d: %u, expected %u\n",
          xs[i], ys[i], zs[i], blockGet(xs[i], ys[i], zs[i]), blocks[i]);
      }
      wrong++;
    }
  }
  check("every edit survives a full rebuild of the extent", wrong == 0);
#undef SAMPLE_COUNT
}

/*
 * Exploration must not spend the building budget.  Outside the fixed extent a
 * change still lands in the block window -- the player sees it happen -- but
 * it takes no slot, because the save format could never have carried it.
 */
static void testOutsideExtent(void) {
  int cx = CHUNKS_X + 3;
  int cz = 2;
  int x = cx * CHUNK_SIZE + 1;
  int z = cz * CHUNK_SIZE + 1;
  u16 before = world_edit_count;
  int y;
  int placed = 0;

  printf("\nEdits outside the extent are transient, not refused\n");
  worldGenerateColumnTerrain(cx, cz);
  for (y = 0; y < 8; y++) {
    if (worldAdvanceColumnDecoration(cx, cz)) {
      break;
    }
  }
  for (y = 1; y < MAX_Y; y++) {
    if (blockGet(x, y, z) != BRICKS && worldEditSet(x, y, z, BRICKS)) {
      placed++;
    }
  }
  check("the edits are accepted", placed > 0);
  check("the world shows them", blockGet(x, MAX_Y - 1, z) == BRICKS ||
    placed == 0);
  check("no pool slot was spent", world_edit_count == before);
  check("the pool is consistent", poolConsistent());
}

/*
 * What a full pool looks like from the outside.  The old failure was silent:
 * worldEditSet returned FALSE and the caller returned, so a refused placement
 * and a dropped controller read looked identical.
 */
static void testOverflow(void) {
  int x, y, z;
  int refusals = 0;
  int accepted = 0;

  printf("\nA full pool refuses loudly and stays intact\n");
  world_edit_full_message = 0;
  for (x = 0; x < MAX_X; x++) {
    for (z = 0; z < MAX_Z; z++) {
      for (y = 1; y < MAX_Y; y++) {
        if (blockGet(x, y, z) == BRICKS || !worldEditInExtent(x, y, z)) {
          continue;
        }
        if (worldEditSet(x, y, z, BRICKS)) {
          accepted++;
        } else {
          refusals++;
        }
      }
    }
  }
  check("the pool filled to its ceiling",
    world_edit_count == MAX_WORLD_EDITS);
  check("some edits were accepted", accepted > 0);
  check("the rest were refused", refusals > 0);
  check("the refusal was counted", world_edit_overflows > 0);
  check("the player is told", world_edit_full_message > 0);
  check("the pool is consistent", poolConsistent());
  check("canSet agrees the pool is full",
    !worldEditCanSet(MAX_X - 1, MAX_Y - 1, MAX_Z - 1) ||
    blockGet(MAX_X - 1, MAX_Y - 1, MAX_Z - 1) == BRICKS);
}

int main(void) {
  printf("Mine64 edit pool checks\n");
  printf("  extent %dx%dx%d, %d columns, %d slots\n",
    MAX_X, MAX_Y, MAX_Z, EDIT_COLUMNS, MAX_WORLD_EDITS);

  generateWorld(20250801u);
  initWorldEdits();
  check("a fresh pool is consistent", poolConsistent());

  testBasicPersistence();
  testReclamation();
  testNoOpEdit();
  testManyColumns();
  testOutsideExtent();

  /* Last: it fills the pool and never empties it. */
  initWorldEdits();
  generateWorld(20250801u);
  testOverflow();

  if (failures != 0) {
    printf("\n%d check(s) failed\n", failures);
    return 1;
  }
  printf("\nall checks passed\n");
  return 0;
}
