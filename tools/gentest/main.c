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

static unsigned long countDifferences(const u8 *a, const u8 *b) {
  unsigned long i, differences = 0;

  for (i = 0; i < SNAPSHOT_BYTES; i++) {
    if (a[i] != b[i]) {
      differences++;
    }
  }
  return differences;
}

static void expectIdentical(const char *name, const u8 *a, const u8 *b) {
  unsigned long differences = countDifferences(a, b);

  if (differences == 0) {
    printf("  PASS  %s\n", name);
    return;
  }
  printf("  FAIL  %s: %lu of %lu blocks differ\n", name, differences,
    SNAPSHOT_BYTES);
  failures++;
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

/* Columns holding either block, counted once each.  A waystone pillar contains
   both kinds of cobblestone, so testing them separately and adding would count
   most pillars twice. */
static int countColumnsContaining(const u8 *world, u8 block, u8 or_block) {
  int x, z, y, columns = 0;

  for (x = 0; x < MAX_X; x++) {
    for (z = 0; z < MAX_Z; z++) {
      for (y = 0; y < MAX_Y; y++) {
        u8 found = world[((unsigned long) x * MAX_Z + z) * MAX_Y + y];
        if (found == block || found == or_block) {
          columns++;
          break;
        }
      }
    }
  }
  return columns;
}

#define WAYSTONE_TRIALS 12

int main(void) {
  u8 *base = malloc(SNAPSHOT_BYTES);
  u8 *other = malloc(SNAPSHOT_BYTES);
  int trial;
  int total_waystones;

  if (base == 0 || other == 0) {
    printf("out of memory\n");
    return 1;
  }

  printf("Generation determinism (%dx%dx%d world)\n", MAX_X, MAX_Y, MAX_Z);

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

  generateWorld(0x1234ABCEULL, 64, 0);
  snapshot(other);
  expectDifferent("a different seed is a different world", base, other);

  /*
   * Density is the one thing converting waystones to a per-column hash could
   * quietly change, and it is invisible until someone walks a whole world.  A
   * waystone is always three columns -- the pillar and its two outriggers --
   * and no other generator places cobblestone of either kind.
   *
   * Expect more spread than the old code gave: drawing exactly ten landmarks
   * produced exactly ten every time.  A density per unit area is a Poisson
   * process, which is both the honest model for a world with no fixed size and
   * the reason the counts below vary.
   */
  printf("\nFeature density across seeds (old code: 10 waystones per world)\n");
  total_waystones = 0;
  for (trial = 0; trial < WAYSTONE_TRIALS; trial++) {
    int stone_columns, waystones, trunks;

    generateWorld(0xC0FFEE00ULL + trial * 7919ULL, MAX_X * MAX_Z, 0);
    snapshot(other);
    stone_columns = countColumnsContaining(other, COBBLESTONE,
      MOSSY_COBBLESTONE);
    waystones = stone_columns / 3;
    trunks = countColumnsContaining(other, WOOD, WOOD);
    total_waystones += waystones;
    printf("  seed %2d: %2d waystones, %4d trees\n", trial, waystones, trunks);
  }
  printf("  mean: %d.%d waystones per world\n",
    total_waystones / WAYSTONE_TRIALS,
    (total_waystones * 10 / WAYSTONE_TRIALS) % 10);

  free(base);
  free(other);

  printf("\n%s\n", failures == 0 ? "all checks passed" : "CHECKS FAILED");
  return failures == 0 ? 0 : 1;
}
