#include "world.h"
#include "day_cycle.h"
#include "blocks.h"
#include "noise.h"
#include "math.h"
#include "trees.h"

u8 window_blocks[WINDOW_SLOTS][COLUMN_BLOCK_BYTES];
u32 window_keys[WINDOW_SLOTS];

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

  /* Rebinding a slot is the eviction: whatever column occupied it is simply
     forgotten.  Callers that need to preserve player edits must have written
     the outgoing column's diff before claiming over it. */
  window_keys[slot] = COLUMN_KEY(cx, cz);
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

static u32 coordinateHash(int x, int y, int z) {
  u32 value = (u32) x * 0x8DA6B343UL;
  value ^= (u32) y * 0xD8163841UL;
  value ^= (u32) z * 0xCB1AB31FUL;
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
  u32 hash = coordinateHash(cell_x, cell_y, cell_z);
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

static void fillTerrainHeightRow(u8 *row, int x) {
  int z;

  /*
   * Keep a one-column halo at each edge.  Three rotating rows are enough for
   * both axes of the slope test, avoiding four redundant multi-octave terrain
   * samples per column without retaining a full second world-sized map.
   */
  for (z = -1; z <= MAX_Z; z++) {
    row[z + 1] = terrainHeight(x, z);
  }
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

void generateLeafHeights(int *heights) {
  int i;
  for (i = 0; i < 25; i++) {
    heights[i] = 0;
  }

  heights[2 * 5 + 2] = 2;
  heights[1 * 5 + 2] = 2;
  heights[3 * 5 + 2] = 2;
  heights[2 * 5 + 1] = 2;
  heights[2 * 5 + 3] = 2;

  heights[1 * 5 + 1] = random(3);
  heights[1 * 5 + 3] = random(3);
  heights[3 * 5 + 1] = random(3);
  heights[3 * 5 + 3] = random(3);
  
  heights[0 * 5 + 0] = -random(3);
  heights[0 * 5 + 4] = -random(3);
  heights[4 * 5 + 0] = -random(3);
  heights[4 * 5 + 4] = -random(3);
}

static void spawnTree(int tx, int ty, int tz) {
  int height;
  int x, y, z;
  u8 tree_index;
  int leaf_heightmap[25];

  blockSet(tx, ty, tz, DIRT);

  height = random(3) + 3;
  tree_index = createTree(tx, tz, ty, height);
  for (y = ty + 1; y < min(ty + height + 1, MAX_Y); y++) {
    blockSet(tx, y, tz, WOOD);
  }

  generateLeafHeights(leaf_heightmap);

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

static void generateWaystones() {
  u8 landmark;

  /*
   * Sparse mossy waystones give a large procedural world memorable bearings
   * and imply history without storing a village simulation or structure map.
   */
  for (landmark = 0; landmark < 10; landmark++) {
    u8 attempt;
    for (attempt = 0; attempt < 32; attempt++) {
      int x = 8 + random(MAX_X - 16);
      int z = 8 + random(MAX_Z - 16);
      int y = exposedGrassY(x, z);
      int east_y = exposedGrassY(x + 2, z);
      int south_y = exposedGrassY(x, z + 2);
      int height;
      int part;

      if (y < SEA_LEVEL + 1 || east_y < 0 || south_y < 0 ||
          absolute((float) (east_y - y)) > 1.f ||
          absolute((float) (south_y - y)) > 1.f) {
        continue;
      }
      height = 2 + random(4);
      for (part = 1; part <= height && y + part < MAX_Y; part++) {
        blockSet(x, y + part, z,
          part == height || (part & 1) ? MOSSY_COBBLESTONE : COBBLESTONE);
      }
      blockSet(x + 2, east_y + 1, z, MOSSY_COBBLESTONE);
      blockSet(x, south_y + 1, z + 2, MOSSY_COBBLESTONE);
      break;
    }
  }
}

void initWorld() {
  static u8 terrain_height_rows[3][MAX_Z + 2];
  u8 *previous_heights = terrain_height_rows[0];
  u8 *current_heights = terrain_height_rows[1];
  u8 *next_heights = terrain_height_rows[2];
  int x, y, z;
  int height, dirt_depth, water_level;
  float biome, slope;
  u8 block, do_sand, exposed_stone;

  seed = (u32) osGetTime();
  setDayCycleWorldTicks(DAY_CYCLE_START_TICK);
  initTrees();

  windowClaimFixedExtent();

  fillTerrainHeightRow(previous_heights, -1);
  fillTerrainHeightRow(current_heights, 0);
  fillTerrainHeightRow(next_heights, 1);

  for (x = 0; x < MAX_X; x++) {
    for (z = 0; z < MAX_Z; z++) {
      height = shapedSurfaceHeight(x, z, current_heights[z + 1],
        &water_level);
      biome = perlin2d(x + 883, z + 521, 0.025f, 3);
      slope = absolute((float)(next_heights[z + 1] -
        previous_heights[z + 1])) +
        absolute((float)(current_heights[z + 2] - current_heights[z]));
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

    if (x + 1 < MAX_X) {
      u8 *old_previous = previous_heights;
      previous_heights = current_heights;
      current_heights = next_heights;
      next_heights = old_previous;
      fillTerrainHeightRow(next_heights, x + 2);
    }
  }

  generateWaystones();

  for (x = 0; x < MAX_X; x++) {
    for (z = 0; z < MAX_Z; z++) {
      if (random(1000) < perlin2d(x, z, 0.02, 2) * 8 - 2) {
        trySpawnTree(x, z);
      }
    }
  }
}
