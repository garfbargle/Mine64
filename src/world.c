#include "world.h"
#include "day_cycle.h"
#include "blocks.h"
#include "noise.h"
#include "math.h"
#include "trees.h"

u8 window_blocks[WINDOW_SLOTS][COLUMN_BLOCK_BYTES];
u32 window_keys[WINDOW_SLOTS];

/*
 * How far a column has been built, per window slot.  The stages themselves are
 * declared in world.h, next to the functions that advance them; the reason
 * they exist is that decoration reaches across chunk boundaries, so a column
 * can only move on once its neighbours are far enough along to be written into
 * and read from.
 */
static u8 column_state[WINDOW_SLOTS];

void windowReset() {
  u32 slot;

  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    window_keys[slot] = COLUMN_KEY_EMPTY;
  }
}

u8 windowColumnResident(int cx, int cz) {
  return window_keys[WINDOW_SLOT(cx, cz)] == COLUMN_KEY(cx, cz);
}

void windowClaimFixedExtent() {
  int cx, cz;

  /* Whole-world generation and whole-world loading both write every column of
     the fixed extent, so bind all of them up front.  Streaming will claim
     columns individually as they enter the residency ring instead. */
  windowReset();
  for (cx = 0; cx < CHUNKS_X; cx++) {
    for (cz = 0; cz < CHUNKS_Z; cz++) {
      windowClaimColumn(cx, cz);
    }
  }
}

u8 *windowClaimColumn(int cx, int cz) {
  u32 slot = WINDOW_SLOT(cx, cz);
  u32 key = COLUMN_KEY(cx, cz);

  /* Rebinding a slot is the eviction: whatever column occupied it is simply
     forgotten.  Callers that need to preserve player edits must have written
     the outgoing column's diff before claiming over it. */
  if (window_keys[slot] != key) {
    if (window_keys[slot] != COLUMN_KEY_EMPTY) {
      /* Derived state keyed to the outgoing column has to go with it.  Tree
         records are a small fixed pool, so a walk that never released them
         would exhaust it and quietly stop growing trees. */
      treesEvictColumn(windowSlotChunkX(slot), windowSlotChunkZ(slot));
    }
    window_keys[slot] = key;
    column_state[slot] = COLUMN_EMPTY;
  }
  return window_blocks[slot];
}

#define SEA_LEVEL 8
#define MIN_SURFACE_HEIGHT 6
#define MAX_SURFACE_HEIGHT (MAX_Y - 2)

static float absolute(float value) {
  return value < 0.0f ? -value : value;
}

static int clampHeight(int height) {
  if (height < MIN_SURFACE_HEIGHT) {
    return MIN_SURFACE_HEIGHT;
  }
  if (height > MAX_SURFACE_HEIGHT) {
    return MAX_SURFACE_HEIGHT;
  }
  return height;
}

/*
 * Every decision generation makes about a coordinate comes from here rather
 * than from the random() stream, so it can be re-asked in isolation and give
 * the same answer -- the property that lets a clean chunk be dropped and
 * regenerated instead of saved.
 *
 * The salt separates independent questions asked about the same block.  Two
 * hashes of one coordinate would otherwise be the same number, so a tree's
 * height and its canopy shape would move together; a salt per question is
 * cheaper and better behaved than slicing bit fields out of a single value.
 */
#define HASH_SALT_ORE 0x00000000UL
#define HASH_SALT_TREE 0x9E3779B9UL
#define HASH_SALT_TREE_HEIGHT 0x7F4A7C15UL
#define HASH_SALT_TREE_CANOPY 0x2545F491UL
#define HASH_SALT_WAYSTONE 0x165667B1UL
#define HASH_SALT_WAYSTONE_HEIGHT 0x27D4EB2FUL

static u32 coordinateHash(int x, int y, int z, u32 salt) {
  u32 value = (u32) x * 0x8DA6B343UL;
  value ^= (u32) y * 0xD8163841UL;
  value ^= (u32) z * 0xCB1AB31FUL;
  value ^= world_seed + salt;
  value ^= value >> 13;
  value *= 0x85EBCA6BUL;
  return value ^ (value >> 16);
}

/*
 * One hash per small cell yields compact, recognizable ore veins without
 * paying for another octave-noise sample for every underground block.
 */
static u8 oreInCell(int x, int y, int z, int cell_size, u8 rarity) {
  int cell_x = x / cell_size;
  int cell_y = y / cell_size;
  int cell_z = z / cell_size;
  u32 hash = coordinateHash(cell_x, cell_y, cell_z, HASH_SALT_ORE);
  int center_x;
  int center_y;
  int center_z;

  if (hash % rarity != 0) {
    return FALSE;
  }
  center_x = (hash >> 8) % cell_size;
  center_y = (hash >> 13) % cell_size;
  center_z = (hash >> 18) % cell_size;
  return absolute((float) (x % cell_size - center_x)) +
    absolute((float) (y % cell_size - center_y)) +
    absolute((float) (z % cell_size - center_z)) <= 1.f;
}

static int terrainHeight(int x, int z) {
  float plains = perlin2d(x + 97, z + 211, 0.010f, 4);
  float lowlands = perlin2d(x + 557, z + 877, 0.030f, 3);
  float gentle_hills = perlin2d(x + 419, z + 131, 0.014f, 3);
  float mountain_mask = perlin2d(x + 701, z + 337, 0.022f, 3);
  float mountain_shape = perlin2d(x + 109, z + 593, 0.010f, 3);
  int height;

  /* Keep the normal route through the world broad and mostly level.  Small,
   * scattered lowlands form shore-level basins, while a slow contribution
   * adds walkable grass hills. */
  height = 9 + (int)(plains * 2.0f);
  if (lowlands < 0.28f) {
    height -= 1 + (int)((0.28f - lowlands) * 8.0f);
  }
  if (gentle_hills > 0.56f) {
    height += (int)((gentle_hills - 0.56f) * 9.0f);
  }

  /* Mountains occupy only the highest islands of a separate field.  Their
   * rounded shape and capped boost leave them as rare destinations rather
   * than a wall of sharp ridges between players. */
  if (mountain_mask > 0.74f) {
    height += (int)((mountain_mask - 0.74f) *
      (45.0f + mountain_shape * 55.0f));
  }

  return clampHeight(height);
}

static int isLake(int x, int z, int natural_height) {
  float basin = perlin2d(x + 719, z + 337, 0.018f, 3);
  float moisture = perlin2d(x + 281, z + 647, 0.033f, 2);

  /* Large, low-frequency islands in the basin signal become lakes only in
   * low-to-mid terrain.  That avoids flat mountaintop puddles. */
  return natural_height <= SEA_LEVEL + 3 &&
    basin > 0.66f && moisture > 0.55f;
}

static int isRiver(int x, int z, int natural_height) {
  float watershed = perlin2d(x + 157, z + 491, 0.012f, 3);
  float channel = absolute(perlin2d(x + 383, z + 73, 0.024f, 2) - 0.5f);

  /* An iso-line in a second noise field provides long, gently winding river
   * courses.  The watershed mask breaks it into distinct drainages instead
   * of a regular grid of channels. */
  return natural_height > SEA_LEVEL && natural_height <= SEA_LEVEL + 3 &&
    watershed > 0.58f && channel < 0.022f;
}

static int shapedSurfaceHeight(int x, int z, int natural_height,
    int *water_level) {
  *water_level = -1;
  if (natural_height <= SEA_LEVEL) {
    *water_level = SEA_LEVEL;
    return natural_height;
  }
  if (isLake(x, z, natural_height)) {
    *water_level = SEA_LEVEL;
    return SEA_LEVEL - 2;
  }
  if (isRiver(x, z, natural_height)) {
    *water_level = SEA_LEVEL;
    return SEA_LEVEL - 1;
  }
  return natural_height;
}

/*
 * The height field for one chunk plus a one-block halo.  The slope test in
 * generateTerrainColumn reads each block column's four neighbours, so a chunk
 * can never be built from its own 8x8 alone.
 *
 * This replaces three rotating world-width rows.  Those amortised terrain
 * sampling down to one multi-octave sample per block column, but only by
 * walking the whole world in x order and keeping the two adjacent rows alive
 * -- precisely the assumption a streaming world cannot make.  A 10x10 patch
 * costs about 1.6 samples per block column instead, and buys the ability to
 * generate a chunk alone, in any order.
 */
#define HEIGHT_PATCH_SIZE (CHUNK_SIZE + 2)
static u8 height_patch[HEIGHT_PATCH_SIZE][HEIGHT_PATCH_SIZE];
static int height_patch_base_x;
static int height_patch_base_z;

static void fillHeightPatch(int base_x, int base_z) {
  int px, pz;

  height_patch_base_x = base_x;
  height_patch_base_z = base_z;
  for (px = 0; px < HEIGHT_PATCH_SIZE; px++) {
    for (pz = 0; pz < HEIGHT_PATCH_SIZE; pz++) {
      height_patch[px][pz] =
        (u8) terrainHeight(base_x + px - 1, base_z + pz - 1);
    }
  }
}

/* Only valid for the chunk the patch was filled for, plus its halo. */
static int patchHeight(int x, int z) {
  return height_patch[x - height_patch_base_x + 1]
                     [z - height_patch_base_z + 1];
}

static int isCave(int x, int y, int z, int surface_height) {
  float chambers;
  float passages;

  /* Preserve a solid floor and several blocks of roof so caves feel like
   * underground networks rather than random holes in the landscape. */
  if (y < 2 || y >= surface_height - 3) {
    return FALSE;
  }

  chambers = perlin3d(x + 71, y, z - 191, 0.105f, 3);
  passages = perlin3d(x - 389, y + 53, z + 127, 0.165f, 2);
  return chambers > 0.68f && passages > 0.58f;
}

/* One canopy corner's draw.  The corner index rides in the hash's y slot, so a
   single salt yields eight independent values for one tree. */
static int canopyDraw(int tx, int corner, int tz) {
  return (int) (coordinateHash(tx, corner, tz, HASH_SALT_TREE_CANOPY) % 3u);
}

static void generateLeafHeights(int *heights, int tx, int tz) {
  int i;
  for (i = 0; i < 25; i++) {
    heights[i] = 0;
  }

  heights[2 * 5 + 2] = 2;
  heights[1 * 5 + 2] = 2;
  heights[3 * 5 + 2] = 2;
  heights[2 * 5 + 1] = 2;
  heights[2 * 5 + 3] = 2;

  heights[1 * 5 + 1] = canopyDraw(tx, 0, tz);
  heights[1 * 5 + 3] = canopyDraw(tx, 1, tz);
  heights[3 * 5 + 1] = canopyDraw(tx, 2, tz);
  heights[3 * 5 + 3] = canopyDraw(tx, 3, tz);

  heights[0 * 5 + 0] = -canopyDraw(tx, 4, tz);
  heights[0 * 5 + 4] = -canopyDraw(tx, 5, tz);
  heights[4 * 5 + 0] = -canopyDraw(tx, 6, tz);
  heights[4 * 5 + 4] = -canopyDraw(tx, 7, tz);
}

static void spawnTree(int tx, int ty, int tz) {
  int height;
  int x, y, z;
  u8 tree_index;
  int leaf_heightmap[25];

  blockSet(tx, ty, tz, DIRT);

  height = 3 + (int) (coordinateHash(tx, 0, tz, HASH_SALT_TREE_HEIGHT) % 3u);
  tree_index = createTree(tx, tz, ty, height);
  for (y = ty + 1; y < min(ty + height + 1, MAX_Y); y++) {
    blockSet(tx, y, tz, WOOD);
  }

  generateLeafHeights(leaf_heightmap, tx, tz);

  for (x = max(tx - 2, 0); x < min(tx + 3, MAX_X); x++) {
    for (z = max(tz - 2, 0); z < min(tz + 3, MAX_Z); z++) {
      for (y = ty + height - 1; y < min(ty + height + leaf_heightmap[(x - tx + 2) * 5 + (z - tz + 2)] + 1, MAX_Y); y++) {
        if (!blockGet(x, y, z)) {
          blockSet(x, y, z, LEAVES);
          treeAddLeaf(tree_index, x, y, z);
        }
      }
    }
  }
}

void trySpawnTree(int tx, int tz) {
  u8 block;
  int ty;

  for (ty = MAX_Y - 1; ty >= 0; ty--) {
    block = blockGet(tx, ty, tz);
    if (block == GRASS) {
      spawnTree(tx, ty, tz);
      return;
    } else if (block != AIR) {
      return;
    }
  }
}

u8 tryPlantTree(int x, int y, int z) {
  u8 scan_y;

  if (x >= MAX_X || y == 0 || y >= MAX_Y || z >= MAX_Z ||
      blockGet(x, y, z) != AIR ||
      blockGet(x, y - 1, z) != GRASS) {
    return FALSE;
  }
  for (scan_y = y; scan_y < MAX_Y; scan_y++) {
    if (blockGet(x, scan_y, z) != AIR) {
      return FALSE;
    }
  }
  spawnTree(x, y - 1, z);
  return TRUE;
}

static int exposedGrassY(int x, int z) {
  int y;

  if (x < 0 || z < 0 || x >= MAX_X || z >= MAX_Z) {
    return -1;
  }
  for (y = MAX_Y - 2; y >= 0; y--) {
    if (blockGet(x, y, z) == GRASS && blockGet(x, y + 1, z) == AIR) {
      return y;
    }
    if (blockGet(x, y, z) != AIR) {
      return -1;
    }
  }
  return -1;
}

/*
 * Sparse mossy waystones give a large procedural world memorable bearings and
 * imply history without storing a village simulation or structure map.
 *
 * This used to draw ten landmarks from the RNG and retry each up to 32 times
 * until it hit suitable ground, which asks a question -- "where are the ten
 * waystones" -- that a single column cannot answer about itself.  Density is
 * now a property of area rather than a count, which is also the only form that
 * means anything once the world stops having a fixed size.
 *
 * One candidate per this many columns.  Most candidates are rejected by the
 * ground test below, so the surviving density is a good deal sparser: this is
 * tuned to land near the ten-per-112x112-world the fixed size used to give.
 */
#define WAYSTONE_COLUMN_ODDS 500

static void tryPlaceWaystone(int x, int z) {
  int y, east_y, south_y;
  int height, part;

  if (coordinateHash(x, 0, z, HASH_SALT_WAYSTONE) % WAYSTONE_COLUMN_ODDS != 0) {
    return;
  }

  /* exposedGrassY rejects out-of-range columns, which is what keeps a waystone
     and its two outriggers from running off the edge of a fixed world. */
  y = exposedGrassY(x, z);
  east_y = exposedGrassY(x + 2, z);
  south_y = exposedGrassY(x, z + 2);
  if (y < SEA_LEVEL + 1 || east_y < 0 || south_y < 0 ||
      absolute((float) (east_y - y)) > 1.f ||
      absolute((float) (south_y - y)) > 1.f) {
    return;
  }

  height = 2 + (int) (coordinateHash(x, 0, z, HASH_SALT_WAYSTONE_HEIGHT) % 4u);
  for (part = 1; part <= height && y + part < MAX_Y; part++) {
    blockSet(x, y + part, z,
      part == height || (part & 1) ? MOSSY_COBBLESTONE : COBBLESTONE);
  }
  blockSet(x + 2, east_y + 1, z, MOSSY_COBBLESTONE);
  blockSet(x, south_y + 1, z + 2, MOSSY_COBBLESTONE);
}

/* TRUE when a tree roots at this column.  The perlin field sets the local
   density and the hash decides whether this particular column draws one. */
static u8 treeSeededAt(int x, int z) {
  float density = perlin2d(x, z, 0.02f, 2) * 8.f - 2.f;

  return (float) (coordinateHash(x, 0, z, HASH_SALT_TREE) % 1000u) < density;
}

/*
 * Generating a world is roughly two seconds of noise sampling -- far longer
 * than a retrace -- and nothing else on the graphics thread submits frames.
 * Run it in slices so the picker keeps orbiting the outgoing world and keeps
 * reading the controller instead of going dead for the duration.
 *
 * The loop state that used to live on initWorld's stack therefore has to
 * persist between slices.
 */
#define WORLD_GEN_IDLE 0
#define WORLD_GEN_TERRAIN 1
#define WORLD_GEN_WAYSTONES 2
#define WORLD_GEN_TREES 3

static u8 world_gen_stage = WORLD_GEN_IDLE;
static int world_gen_x;
static int world_gen_z;


static void generateTerrainColumn(int x, int z) {
  int y;
  int height, dirt_depth, water_level;
  float biome, slope;
  u8 block, do_sand, exposed_stone;

  height = shapedSurfaceHeight(x, z, patchHeight(x, z), &water_level);
  biome = perlin2d(x + 883, z + 521, 0.025f, 3);
  slope = absolute((float)(patchHeight(x + 1, z) - patchHeight(x - 1, z))) +
    absolute((float)(patchHeight(x, z + 1) - patchHeight(x, z - 1)));
  /* Sand belongs at shorelines and a few shallow lowland patches.  Do
   * not let a broad biome signal turn hills or mountain shoulders into
   * implausible vertical sand stacks. */
  do_sand = height <= SEA_LEVEL + 1 ||
    (height <= SEA_LEVEL + 3 && biome < 0.42f);
  exposed_stone = height > 21 || slope >= 5;
  dirt_depth = 3 + (int)(biome * 2.0f);

  for (y = 0; y < MAX_Y; y++) {
    if (y >= height && y <= water_level) {
      block = WATER;
    } else if (y >= height)  {
      block = AIR;
    } else if (y == 0) {
      block = BEDROCK;
    } else if (isCave(x, y, z, height)) {
      block = AIR;
    } else if (y < height - dirt_depth) {
      if (y < 13 && oreInCell(x, y, z, 5, 7)) {
        block = IRON_ORE;
      } else if (y < 23 && oreInCell(x, y, z, 4, 5)) {
        block = COAL_ORE;
      } else {
        block = STONE;
      }
    } else if (do_sand) {
      block = SAND;
    } else if (exposed_stone && y >= height - 2) {
      block = STONE;
    } else if (y == height - 1) {
      block = GRASS;
    } else {
      block = DIRT;
    }

    blockSet(x, y, z, block);
  }
}

u8 worldColumnState(int cx, int cz) {
  u32 slot = WINDOW_SLOT(cx, cz);

  if (!windowColumnResident(cx, cz)) {
    return COLUMN_EMPTY;
  }
  return column_state[slot];
}

/*
 * TRUE when every neighbour touching this column has reached `state`.
 *
 * A neighbour that is not resident is not counted as behind, because it is not
 * coming: whole-world generation claims exactly the world, so its edge columns
 * decorate exactly as they always did rather than waiting forever on a column
 * outside the map.  The cost of that reading is an invariant streaming has to
 * honour -- **the ring that gets terrain must be one column wider than the
 * ring that gets decorated** -- or a canopy reaching into a column that has
 * not been claimed yet is silently dropped.
 */
static u8 neighboursReached(int cx, int cz, u8 state) {
  int dx, dz;

  for (dx = -1; dx <= 1; dx++) {
    for (dz = -1; dz <= 1; dz++) {
      if (!windowColumnResident(cx + dx, cz + dz)) {
        continue;
      }
      if (column_state[WINDOW_SLOT(cx + dx, cz + dz)] < state) {
        return FALSE;
      }
    }
  }
  return TRUE;
}

/* Claim a slot and fill it with bare terrain.  Nothing here reads a
   neighbour, so a column can always take this step alone. */
void worldGenerateColumnTerrain(int cx, int cz) {
  int base_x = cx * CHUNK_SIZE;
  int base_z = cz * CHUNK_SIZE;
  int bx, bz;

  windowClaimColumn(cx, cz);
  fillHeightPatch(base_x, base_z);
  for (bx = 0; bx < CHUNK_SIZE; bx++) {
    for (bz = 0; bz < CHUNK_SIZE; bz++) {
      generateTerrainColumn(base_x + bx, base_z + bz);
    }
  }
  column_state[WINDOW_SLOT(cx, cz)] = COLUMN_TERRAIN;
}

/* Returns FALSE when the column's neighbours are not built far enough yet and
   the caller should come back to it. */
u8 worldAdvanceColumnDecoration(int cx, int cz) {
  int base_x = cx * CHUNK_SIZE;
  int base_z = cz * CHUNK_SIZE;
  int bx, bz;
  u32 slot = WINDOW_SLOT(cx, cz);

  if (column_state[slot] == COLUMN_TERRAIN) {
    /* Reads the ground two blocks east and south, which can be a neighbour. */
    if (!neighboursReached(cx, cz, COLUMN_TERRAIN)) {
      return FALSE;
    }
    for (bx = 0; bx < CHUNK_SIZE; bx++) {
      for (bz = 0; bz < CHUNK_SIZE; bz++) {
        tryPlaceWaystone(base_x + bx, base_z + bz);
      }
    }
    column_state[slot] = COLUMN_WAYSTONED;
    return FALSE;
  }

  if (column_state[slot] == COLUMN_WAYSTONED) {
    /* Writes canopy up to two blocks out, and must not root in ground a
       neighbour's waystone is about to take. */
    if (!neighboursReached(cx, cz, COLUMN_WAYSTONED)) {
      return FALSE;
    }
    for (bx = 0; bx < CHUNK_SIZE; bx++) {
      for (bz = 0; bz < CHUNK_SIZE; bz++) {
        if (treeSeededAt(base_x + bx, base_z + bz)) {
          trySpawnTree(base_x + bx, base_z + bz);
        }
      }
    }
    column_state[slot] = COLUMN_DECORATED;
  }
  return column_state[slot] == COLUMN_DECORATED;
}

void beginWorldGeneration() {
  u32 slot;

  /* One draw of entropy fixes the world; the gameplay RNG starts from the same
     place but is free to wander, because nothing reproducible reads it. */
  world_seed = (u32) osGetTime();
  seed = world_seed;
  setDayCycleWorldTicks(DAY_CYCLE_START_TICK);
  initTrees();

  windowClaimFixedExtent();
  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    column_state[slot] = COLUMN_EMPTY;
  }

  world_gen_x = 0;
  world_gen_z = 0;
  world_gen_stage = WORLD_GEN_TERRAIN;
}

u8 worldGenerationActive() {
  return world_gen_stage != WORLD_GEN_IDLE;
}

u8 worldGenerationProgress() {
  if (world_gen_stage == WORLD_GEN_IDLE) {
    return 100;
  }
  /* Terrain dominates the cost; treat decoration as the last tenth so the bar
     does not stall visibly at the end.  Both decoration stages walk every
     column now, so they split that tenth between them. */
  if (world_gen_stage == WORLD_GEN_TERRAIN) {
    return (u8) ((world_gen_x * 90) / CHUNKS_X);
  }
  if (world_gen_stage == WORLD_GEN_WAYSTONES) {
    return (u8) (90 + (world_gen_x * 5) / CHUNKS_X);
  }
  return (u8) (95 + (world_gen_x * 5) / CHUNKS_X);
}

/*
 * Walks chunk columns rather than block columns now, in three whole-extent
 * passes.  The passes are what satisfy the neighbour gates everywhere at once:
 * by the time the waystone pass reaches a column every column has terrain, and
 * by the time the tree pass reaches it every column has its waystones.  A
 * streaming world reaches the same states one column at a time instead, and
 * gets the same world out -- which is what the host harness checks.
 */
u8 stepWorldGeneration(u32 columns) {
  while (columns > 0 && world_gen_stage != WORLD_GEN_IDLE) {
    if (world_gen_stage == WORLD_GEN_TERRAIN) {
      worldGenerateColumnTerrain(world_gen_x, world_gen_z);
    } else {
      worldAdvanceColumnDecoration(world_gen_x, world_gen_z);
    }
    columns--;

    if (++world_gen_z >= CHUNKS_Z) {
      world_gen_z = 0;
      if (++world_gen_x >= CHUNKS_X) {
        world_gen_x = 0;
        world_gen_stage++;
        if (world_gen_stage > WORLD_GEN_TREES) {
          world_gen_stage = WORLD_GEN_IDLE;
        }
      }
    }
  }
  return world_gen_stage == WORLD_GEN_IDLE;
}

/* Whole-world generation for callers that cannot yield, such as the storage
   fallback when a save will not open.  Identical output to the sliced path:
   the same RNG stream is consumed in the same order. */
void initWorld() {
  beginWorldGeneration();
  while (!stepWorldGeneration(MAX_X * MAX_Z)) {
  }
}
