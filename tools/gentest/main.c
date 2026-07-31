/*
 * Does a world generate the same way twice, regardless of what else the game
 * was doing at the time?
 *
 * Streaming rests entirely on that: an unmodified chunk is not saved, it is
 * dropped and regenerated later from its coordinates and the world seed.  If
 * generation can be nudged by anything else -- the order columns are visited,
 * how many blocks were mined first, how far the gameplay RNG has advanced --
 * then walking away from terrain and coming back rewrites it.
 *
 * Build and run:  tools/gentest/run.sh
 */
#include <nusys.h>

#include "blocks.h"
#include "math.h"
#include "world.h"

/*
 * Declared by hand rather than included: the game defines random(), div() and
 * floor() with its own signatures, so pulling in <stdlib.h> next to math.h is
 * a pile of conflicting declarations.  These three are all the harness needs.
 */
int printf(const char *format, ...);
void *malloc(unsigned long size);
void free(void *pointer);

#define SNAPSHOT_BYTES ((unsigned long) MAX_X * MAX_Y * MAX_Z)

static int failures;

static void generateWorld(u64 time, u32 slice, int perturb_rng) {
  gentestSetTime(time);
  beginWorldGeneration();
  while (!stepWorldGeneration(slice)) {
    /* Stand in for a game running alongside generation: mob AI, item drops and
       tree felling all pull on the gameplay RNG.  Terrain must not notice. */
    if (perturb_rng) {
      int i;
      for (i = 0; i < 97; i++) {
        random(1000);
      }
    }
  }
}

static void snapshot(u8 *out) {
  int x, z, y;
  unsigned long i = 0;

  for (x = 0; x < MAX_X; x++) {
    for (z = 0; z < MAX_Z; z++) {
      for (y = 0; y < MAX_Y; y++) {
        out[i++] = blockGet(x, y, z);
      }
    }
  }
}

static unsigned long countDifferencesIn(const u8 *a, const u8 *b,
    unsigned long bytes) {
  unsigned long i, differences = 0;

  for (i = 0; i < bytes; i++) {
    if (a[i] != b[i]) {
      differences++;
    }
  }
  return differences;
}

static unsigned long countDifferences(const u8 *a, const u8 *b) {
  return countDifferencesIn(a, b, SNAPSHOT_BYTES);
}

static void expectIdenticalIn(const char *name, const u8 *a, const u8 *b,
    unsigned long bytes) {
  unsigned long differences = countDifferencesIn(a, b, bytes);

  if (differences == 0) {
    printf("  PASS  %s\n", name);
    return;
  }
  printf("  FAIL  %s: %lu of %lu blocks differ\n", name, differences, bytes);
  failures++;
}

static void expectIdentical(const char *name, const u8 *a, const u8 *b) {
  expectIdenticalIn(name, a, b, SNAPSHOT_BYTES);
}

static void expectDifferent(const char *name, const u8 *a, const u8 *b) {
  unsigned long differences = countDifferences(a, b);

  if (differences > 0) {
    printf("  PASS  %s (%lu blocks differ)\n", name, differences);
    return;
  }
  printf("  FAIL  %s: worlds are identical\n", name);
  failures++;
}

/* Structure macrocells are intentionally aligned to 64 blocks.  Planks occur
   in cottages but not ruins; bricks occur in both and nowhere in untouched
   terrain, so the generated blocks themselves let the black-box harness count
   each kind without exposing generator internals to the game API. */
#define TEST_STRUCTURE_CELL_SIZE 64
#define TEST_STRUCTURE_CELLS_X \
  ((MAX_X + TEST_STRUCTURE_CELL_SIZE - 1) / TEST_STRUCTURE_CELL_SIZE)
#define TEST_STRUCTURE_CELLS_Z \
  ((MAX_Z + TEST_STRUCTURE_CELL_SIZE - 1) / TEST_STRUCTURE_CELL_SIZE)

static void countStructureCells(const u8 *world, int *hamlets, int *ruins) {
  int cell_x, cell_z;

  *hamlets = 0;
  *ruins = 0;
  for (cell_x = 0; cell_x < TEST_STRUCTURE_CELLS_X; cell_x++) {
    for (cell_z = 0; cell_z < TEST_STRUCTURE_CELLS_Z; cell_z++) {
      int start_x = cell_x * TEST_STRUCTURE_CELL_SIZE;
      int start_z = cell_z * TEST_STRUCTURE_CELL_SIZE;
      int end_x = min(start_x + TEST_STRUCTURE_CELL_SIZE, MAX_X);
      int end_z = min(start_z + TEST_STRUCTURE_CELL_SIZE, MAX_Z);
      int x, z, y;
      u8 has_planks = FALSE;
      u8 has_bricks = FALSE;

      for (x = start_x; x < end_x; x++) {
        for (z = start_z; z < end_z; z++) {
          for (y = 0; y < MAX_Y; y++) {
            u8 block = world[((unsigned long) x * MAX_Z + z) * MAX_Y + y];

            if (block == PLANKS) {
              has_planks = TRUE;
            } else if (block == BRICKS) {
              has_bricks = TRUE;
            }
          }
        }
      }
      if (has_planks) {
        (*hamlets)++;
      } else if (has_bricks) {
        (*ruins)++;
      }
    }
  }
}

/*
 * The property streaming actually needs: a column built on its own, in
 * whatever order the player happened to walk, is the column the whole-world
 * passes would have produced.  Terrain goes in shuffled, then decoration is
 * driven to a fixpoint in a freshly shuffled order each round, so nothing can
 * quietly depend on columns being reached in a sweep.
 */
static u32 shuffle_state = 0x2F6E2B1u;

static u32 nextShuffle(u32 limit) {
  shuffle_state ^= shuffle_state << 13;
  shuffle_state ^= shuffle_state >> 17;
  shuffle_state ^= shuffle_state << 5;
  return shuffle_state % limit;
}

static void shuffleOrder(int *order, int count) {
  int i;

  for (i = count - 1; i > 0; i--) {
    int j = (int) nextShuffle((u32) (i + 1));
    int swap = order[i];
    order[i] = order[j];
    order[j] = swap;
  }
}

static void generateColumnByColumn(u64 time) {
  static int order[CHUNKS_X * CHUNKS_Z];
  const int count = CHUNKS_X * CHUNKS_Z;
  int i, round, pending;

  /* Claims the extent and clears the per-column states, then the passes are
     bypassed entirely in favour of driving single columns. */
  gentestSetTime(time);
  beginWorldGeneration();

  for (i = 0; i < count; i++) {
    order[i] = i;
  }

  shuffleOrder(order, count);
  for (i = 0; i < count; i++) {
    worldGenerateColumnTerrain(order[i] / CHUNKS_Z, order[i] % CHUNKS_Z);
  }

  /* Each round re-shuffles, so a column blocked on a neighbour is retried from
     a different direction.  Three stages cannot need more rounds than that. */
  for (round = 0; round < COLUMN_DECORATED + 2; round++) {
    shuffleOrder(order, count);
    pending = 0;
    for (i = 0; i < count; i++) {
      if (!worldAdvanceColumnDecoration(order[i] / CHUNKS_Z,
          order[i] % CHUNKS_Z)) {
        pending++;
      }
    }
    if (pending == 0) {
      return;
    }
  }
  printf("  note: %d columns never finished decorating\n", pending);
}

/* Structures are allowed to span many chunks conceptually, but advancing one
   target must write only that target's bytes.  This is stronger than the final
   shuffled-world comparison: a carefully gated cross-column writer could be
   deterministic and still make eviction regenerate clipped buildings. */
static void checkStructureColumnOwnership(void) {
  static int order[CHUNKS_X * CHUNKS_Z];
  u8 *before = malloc(SNAPSHOT_BYTES);
  int count = CHUNKS_X * CHUNKS_Z;
  int i;
  int violations = 0;

  if (before == 0) {
    printf("  FAIL  out of memory checking structure column ownership\n");
    failures++;
    return;
  }
  gentestSetTime(0x5E771EULL);
  beginWorldGeneration();
  for (i = 0; i < count; i++) {
    order[i] = i;
    worldGenerateColumnTerrain(i / CHUNKS_Z, i % CHUNKS_Z);
  }
  shuffleOrder(order, count);
  for (i = 0; i < count; i++) {
    int cx = order[i] / CHUNKS_Z;
    int cz = order[i] % CHUNKS_Z;
    int x, z, y;

    snapshot(before);
    worldAdvanceColumnDecoration(cx, cz);
    for (x = 0; x < MAX_X; x++) {
      for (z = 0; z < MAX_Z; z++) {
        unsigned long base;

        if ((x >> CHUNK_SHIFT) == cx && (z >> CHUNK_SHIFT) == cz) {
          continue;
        }
        base = ((unsigned long) x * MAX_Z + z) * MAX_Y;
        for (y = 0; y < MAX_Y; y++) {
          if (blockGet(x, y, z) != before[base + y]) {
            violations++;
            x = MAX_X;
            z = MAX_Z;
            break;
          }
        }
      }
    }
  }
  if (violations == 0) {
    printf("  PASS  each structure stage writes only its target column\n");
  } else {
    printf("  FAIL  %d structure stages wrote outside their target column\n",
      violations);
    failures++;
  }
  free(before);
}

#define STRUCTURE_TRIALS 12

/*
 * The window slot is now the only name a column has: rendering indexes every
 * per-column table by it, and recovers the column's coordinates back out of
 * the key.  Both directions have to hold for negative chunks, which the fixed
 * world never produced and streaming reaches on the first step west or north.
 */
static void checkSlotMapping(void) {
  int cx, cz;
  int collisions = 0, round_trip_failures = 0;
  static int seen[WINDOW_SLOTS];

  for (cx = 0; cx < CHUNKS_X; cx++) {
    for (cz = 0; cz < CHUNKS_Z; cz++) {
      u32 slot = WINDOW_SLOT(cx, cz);
      if (seen[slot]++) {
        collisions++;
      }
    }
  }
  if (collisions == 0) {
    printf("  PASS  the fixed world's columns occupy distinct slots\n");
  } else {
    printf("  FAIL  %d slot collisions inside the fixed extent\n", collisions);
    failures++;
  }

  /* Well past the fixed world in both directions, including negatives. */
  windowReset();
  for (cx = -40; cx <= 40; cx++) {
    for (cz = -40; cz <= 40; cz++) {
      u32 slot = WINDOW_SLOT(cx, cz);
      windowClaimColumn(cx, cz);
      if (!windowSlotResident(slot) || windowSlotChunkX(slot) != cx ||
          windowSlotChunkZ(slot) != cz) {
        round_trip_failures++;
      }
    }
  }
  if (round_trip_failures == 0) {
    printf("  PASS  slot decode round-trips, negative chunks included\n");
  } else {
    printf("  FAIL  %d slots did not decode back to their column\n",
      round_trip_failures);
    failures++;
  }
  windowReset();
}

/*
 * Far lands: everything the fixed world never exercised at once.  Chunk
 * (-52, 63) is around block (-416, 504) -- negative x beyond every noise
 * sample offset, z well past the fixed extent -- so this walks the actual
 * streaming loop out there and asserts the three properties an unbounded
 * world rests on:
 *
 *   1. what terrain you find cannot depend on the walk that found it,
 *   2. terrain evicted behind you regenerates byte-identical when you return,
 *   3. decoration really happens out there -- trees root *and* keep their
 *      canopies, which is exactly what the old 0..MAX clamps broke.
 *
 * Snapshots cover Chebyshev radius 4 around the centre: those columns'
 * entire decoration neighbourhoods (radius 5) are inside the settled tree
 * ring, so both builds must agree on every byte.
 */
#define FAR_CX (-52)
#define FAR_CZ 63
#define FAR_SEED 0xFA9C0FFEEULL
#define FAR_RADIUS 4
#define FAR_SPAN ((FAR_RADIUS * 2 + 1) * CHUNK_SIZE)
#define FAR_SNAPSHOT_BYTES ((unsigned long) FAR_SPAN * FAR_SPAN * MAX_Y)

/* The per-frame budgets the game itself streams with (main.c). */
#define FAR_TERRAIN_BUDGET 3
#define FAR_DECORATE_BUDGET 4

static void settleStreaming(int pcx, int pcz) {
  int i;

  /* The terrain ring is 225 columns and each stage needs its own passes, so
     give the loop far more frames than any settle can need. */
  for (i = 0; i < 500; i++) {
    stepWorldStreaming(pcx, pcz, FAR_TERRAIN_BUDGET, FAR_DECORATE_BUDGET);
  }
}

/* One chunk of movement per "frame burst", the way a sprinting player crosses
   boundaries with the streamer trickling along behind. */
static void walkStreamingTo(int *cur_cx, int *cur_cz, int to_cx, int to_cz) {
  while (*cur_cx != to_cx || *cur_cz != to_cz) {
    int i;

    if (*cur_cx != to_cx) {
      *cur_cx += to_cx > *cur_cx ? 1 : -1;
    }
    if (*cur_cz != to_cz) {
      *cur_cz += to_cz > *cur_cz ? 1 : -1;
    }
    for (i = 0; i < 8; i++) {
      stepWorldStreaming(*cur_cx, *cur_cz, FAR_TERRAIN_BUDGET,
        FAR_DECORATE_BUDGET);
    }
  }
}

static void snapshotFar(u8 *out) {
  int x, z, y;
  unsigned long i = 0;
  int base_x = (FAR_CX - FAR_RADIUS) * CHUNK_SIZE;
  int base_z = (FAR_CZ - FAR_RADIUS) * CHUNK_SIZE;

  for (x = 0; x < FAR_SPAN; x++) {
    for (z = 0; z < FAR_SPAN; z++) {
      for (y = 0; y < MAX_Y; y++) {
        out[i++] = blockGet(base_x + x, y, base_z + z);
      }
    }
  }
}

static void expectFarRingDecorated(const char *name) {
  int dx, dz, undecorated = 0;

  for (dx = -STREAM_TREE_RADIUS; dx <= STREAM_TREE_RADIUS; dx++) {
    for (dz = -STREAM_TREE_RADIUS; dz <= STREAM_TREE_RADIUS; dz++) {
      if (worldColumnState(FAR_CX + dx, FAR_CZ + dz) != COLUMN_DECORATED) {
        undecorated++;
      }
    }
  }
  if (undecorated == 0) {
    printf("  PASS  %s\n", name);
  } else {
    printf("  FAIL  %s: %d columns not decorated\n", name, undecorated);
    failures++;
  }
}

static void checkFarLands(void) {
  u8 *walked = malloc(FAR_SNAPSHOT_BYTES);
  u8 *direct = malloc(FAR_SNAPSHOT_BYTES);
  int cur_cx, cur_cz;
  unsigned long i, trunks = 0, leaves = 0, not_resident = 0;

  if (walked == 0 || direct == 0) {
    printf("out of memory\n");
    failures++;
    return;
  }

  printf("\nFar lands (streaming to chunk %d,%d)\n", FAR_CX, FAR_CZ);

  /* A fresh world at spawn, then the long walk out. */
  gentestSetTime(FAR_SEED);
  generateWorld(FAR_SEED, 64, 0);
  cur_cx = CHUNKS_X / 2;
  cur_cz = CHUNKS_Z / 2;
  walkStreamingTo(&cur_cx, &cur_cz, FAR_CX, FAR_CZ);
  settleStreaming(FAR_CX, FAR_CZ);
  expectFarRingDecorated("the walked-to ring fully decorates");
  snapshotFar(walked);

  /* Same seed, no walk history: the ring lands straight on the far point. */
  generateWorld(FAR_SEED, 64, 0);
  settleStreaming(FAR_CX, FAR_CZ);
  snapshotFar(direct);
  expectIdenticalIn("far terrain does not depend on the walk that found it",
    walked, direct, FAR_SNAPSHOT_BYTES);

  /* Walk far enough away that every column of the patch is evicted, then
     come back.  Clean columns carry no bytes; they must come back anyway. */
  walkStreamingTo(&cur_cx, &cur_cz, FAR_CX + 3 * STREAM_TERRAIN_RADIUS,
    FAR_CZ);
  settleStreaming(cur_cx, cur_cz);
  walkStreamingTo(&cur_cx, &cur_cz, FAR_CX, FAR_CZ);
  settleStreaming(FAR_CX, FAR_CZ);
  snapshotFar(direct);
  expectIdenticalIn("evicted terrain regenerates identically on return",
    walked, direct, FAR_SNAPSHOT_BYTES);

  for (i = 0; i < FAR_SNAPSHOT_BYTES; i++) {
    if (walked[i] == BLOCK_NOT_RESIDENT) {
      not_resident++;
    }
  }

  /*
   * The decoration the fixed-extent clamps used to strip out here.
   *
   * Counted across the whole decorated ring rather than the snapshot patch.
   * The patch is nine chunks of one spot, and with biomes a spot is allowed
   * to be a desert -- which is treeless by design, because trySpawnTree
   * plants only on grass.  This asks the question the check is actually for
   * ("does anything decorate this far out?") rather than betting the answer
   * on one 72-block square of a particular seed's climate.
   */
  {
    int dx, dz, bx, bz, y;

    for (dx = -STREAM_TREE_RADIUS; dx <= STREAM_TREE_RADIUS; dx++) {
      for (dz = -STREAM_TREE_RADIUS; dz <= STREAM_TREE_RADIUS; dz++) {
        for (bx = 0; bx < CHUNK_SIZE; bx++) {
          for (bz = 0; bz < CHUNK_SIZE; bz++) {
            int x = (FAR_CX + dx) * CHUNK_SIZE + bx;
            int z = (FAR_CZ + dz) * CHUNK_SIZE + bz;

            for (y = 0; y < MAX_Y; y++) {
              u8 block = blockGet(x, y, z);

              if (block == WOOD) {
                trunks++;
              } else if (block == LEAVES) {
                leaves++;
              }
            }
          }
        }
      }
    }
  }
  if (trunks > 0 && leaves > trunks) {
    printf("  PASS  far trees decorate with canopies "
      "(%lu trunk, %lu leaf blocks)\n", trunks, leaves);
  } else {
    printf("  FAIL  far decoration missing: %lu trunk, %lu leaf blocks\n",
      trunks, leaves);
    failures++;
  }
  if (not_resident == 0) {
    printf("  PASS  the settled snapshot region is fully resident\n");
  } else {
    printf("  FAIL  %lu blocks in the snapshot were not resident\n",
      not_resident);
    failures++;
  }

  free(walked);
  free(direct);
}

int main(void) {
  u8 *base = malloc(SNAPSHOT_BYTES);
  u8 *other = malloc(SNAPSHOT_BYTES);
  int trial;
  int total_hamlets;
  int total_ruins;

  if (base == 0 || other == 0) {
    printf("out of memory\n");
    return 1;
  }

  printf("Generation determinism (%dx%dx%d world)\n", MAX_X, MAX_Y, MAX_Z);

  checkSlotMapping();
  checkStructureColumnOwnership();

  generateWorld(0x1234ABCDULL, 64, 0);
  snapshot(base);

  /* The slice budget is a scheduling decision -- how much of a world to build
     per frame.  It must not be able to reach the terrain. */
  generateWorld(0x1234ABCDULL, 1, 0);
  snapshot(other);
  expectIdentical("slice size does not change the world", base, other);

  generateWorld(0x1234ABCDULL, MAX_X * MAX_Z, 0);
  snapshot(other);
  expectIdentical("one-shot generation matches sliced", base, other);

  /* The regression this task exists to close: noise and decoration used to
     read the same global the gameplay RNG advances. */
  generateWorld(0x1234ABCDULL, 64, 1);
  snapshot(other);
  expectIdentical("gameplay RNG cannot disturb generation", base, other);

  /*
   * What streaming will actually do, against what the fixed world does.
   *
   * Run over several seeds rather than one.  A tree canopy suppresses a tree
   * that would have rooted under it -- the root scan meets leaves before it
   * meets grass -- so two trees in adjacent columns are an order-dependent
   * pair that a single seed can easily fail to contain.
   */
  for (trial = 0; trial < 8; trial++) {
    u64 seed_time = 0x1234ABCDULL + trial * 104729ULL;

    generateWorld(seed_time, 64, 0);
    snapshot(base);
    generateColumnByColumn(seed_time);
    snapshot(other);
    expectIdentical("columns built one at a time, in shuffled order", base,
      other);
  }

  /* Restore the reference world the remaining checks compare against. */
  generateWorld(0x1234ABCDULL, 64, 0);
  snapshot(base);

  generateWorld(0x1234ABCEULL, 64, 0);
  snapshot(other);
  expectDifferent("a different seed is a different world", base, other);

  /* The macrocell roll and terrain suitability filter should produce both
     inhabited and ruined landmarks across seeds without blanketing the map.
     Counting cells also catches a blueprint accidentally crossing its cell or
     losing the distinctive cottage/ruin material vocabulary. */
  printf("\nStructure density across seeds (four visible macrocells)\n");
  total_hamlets = 0;
  total_ruins = 0;
  for (trial = 0; trial < STRUCTURE_TRIALS; trial++) {
    int hamlets, ruins;

    generateWorld(0xC0FFEE00ULL + trial * 7919ULL, MAX_X * MAX_Z, 0);
    snapshot(other);
    countStructureCells(other, &hamlets, &ruins);
    total_hamlets += hamlets;
    total_ruins += ruins;
    printf("  seed %2d: %d hamlet%s, %d ruin%s\n", trial,
      hamlets, hamlets == 1 ? "" : "s", ruins, ruins == 1 ? "" : "s");
  }
  if (total_hamlets > 0 && total_ruins > 0 &&
      total_hamlets + total_ruins <
        STRUCTURE_TRIALS * TEST_STRUCTURE_CELLS_X *
          TEST_STRUCTURE_CELLS_Z) {
    printf("  PASS  settlements are sparse and both content sets appear "
      "(%d hamlets, %d ruins)\n", total_hamlets, total_ruins);
  } else {
    printf("  FAIL  bad structure spread: %d hamlets, %d ruins\n",
      total_hamlets, total_ruins);
    failures++;
  }

  free(base);
  free(other);

  checkFarLands();

  printf("\n%s\n", failures == 0 ? "all checks passed" : "CHECKS FAILED");
  return failures == 0 ? 0 : 1;
}
