#include "world.h"
#include "day_cycle.h"
#include "blocks.h"
#include "noise.h"
#include "math.h"
#include "trees.h"

u8 blocks[NUM_BLOCKS];

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

static float ridgeNoise(float x, float z, float frequency) {
  float value = perlin2d(x, z, frequency, 3);
  return 1.0f - absolute(value * 2.0f - 1.0f);
}

static int terrainHeight(int x, int z) {
  float continents = perlin2d(x, z, 0.010f, 4);
  float rolling = perlin2d(x + 213, z - 97, 0.045f, 3);
  float mountain_mask = perlin2d(x - 401, z + 179, 0.017f, 3);
  float ridges = ridgeNoise(x + 67, z - 311, 0.034f);
  int height;

  /* Broad continents supply the silhouette while ridges only become tall in
   * mountain regions.  This leaves room for beaches and low grasslands. */
  height = 5 + (int)(continents * 11.0f) + (int)((rolling - 0.5f) * 5.0f);
  if (mountain_mask > 0.51f) {
    height += (int)((mountain_mask - 0.51f) * ridges * 40.0f);
  }

  return clampHeight(height);
}

static int isLake(int x, int z, int natural_height) {
  float basin = perlin2d(x - 719, z + 337, 0.018f, 3);
  float moisture = perlin2d(x + 281, z - 647, 0.033f, 2);

  /* Large, low-frequency islands in the basin signal become lakes only in
   * low-to-mid terrain.  That avoids flat mountaintop puddles. */
  return natural_height <= SEA_LEVEL + 5 &&
    basin > 0.66f && moisture > 0.55f;
}

static int isRiver(int x, int z, int natural_height) {
  float watershed = perlin2d(x + 157, z - 491, 0.012f, 3);
  float channel = absolute(perlin2d(x - 383, z + 73, 0.024f, 2) - 0.5f);

  /* An iso-line in a second noise field provides long, gently winding river
   * courses.  The watershed mask breaks it into distinct drainages instead
   * of a regular grid of channels. */
  return natural_height > SEA_LEVEL && natural_height <= SEA_LEVEL + 5 &&
    watershed > 0.58f && channel < 0.022f;
}

static int shapedSurfaceHeight(int x, int z, int *water_level) {
  int natural_height = terrainHeight(x, z);

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

  blocks[tx * MAX_Z * MAX_Y + ty * MAX_Z + tz] = DIRT;

  height = random(3) + 3;
  tree_index = createTree(tx, tz, ty, height);
  for (y = ty + 1; y < min(ty + height + 1, MAX_Y); y++) {
    blocks[tx * MAX_Z * MAX_Y + y * MAX_Z + tz] = WOOD;
  }

  generateLeafHeights(leaf_heightmap);

  for (x = max(tx - 2, 0); x < min(tx + 3, MAX_X); x++) {
    for (z = max(tz - 2, 0); z < min(tz + 3, MAX_Z); z++) {
      for (y = ty + height - 1; y < min(ty + height + leaf_heightmap[(x - tx + 2) * 5 + (z - tz + 2)] + 1, MAX_Y); y++) {
        if (!blocks[x * MAX_Z * MAX_Y + y * MAX_Z + z]) {
          blocks[x * MAX_Z * MAX_Y + y * MAX_Z + z] = LEAVES;
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
    block = blocks[tx * MAX_Z * MAX_Y + ty * MAX_Z + tz];
    if (block == GRASS) {
      spawnTree(tx, ty, tz);
      return;
    } else if (block != AIR) {
      return;
    }
  }
}

u8 tryPlantTree(u8 x, u8 y, u8 z) {
  u8 scan_y;

  if (x >= MAX_X || y == 0 || y >= MAX_Y || z >= MAX_Z ||
      blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z] != AIR ||
      blocks[x * MAX_Y * MAX_Z + (y - 1) * MAX_Z + z] != GRASS) {
    return FALSE;
  }
  for (scan_y = y; scan_y < MAX_Y; scan_y++) {
    if (blocks[x * MAX_Y * MAX_Z + scan_y * MAX_Z + z] != AIR) {
      return FALSE;
    }
  }
  spawnTree(x, y - 1, z);
  return TRUE;
}

void initWorld() {
  int x, y, z;
  int height, dirt_depth, water_level;
  float biome, slope;
  u8 block, do_sand, exposed_stone;

  seed = (u32) osGetTime();
  setDayCycleWorldTicks(DAY_CYCLE_START_TICK);
  initTrees();

  for (x = 0; x < MAX_X; x++) {
    for (z = 0; z < MAX_Z; z++) {
      height = shapedSurfaceHeight(x, z, &water_level);
      biome = perlin2d(x + 883, z - 521, 0.025f, 3);
      slope = absolute((float)(terrainHeight(x + 1, z) - terrainHeight(x - 1, z))) +
              absolute((float)(terrainHeight(x, z + 1) - terrainHeight(x, z - 1)));
      do_sand = water_level >= 0 || height <= SEA_LEVEL + 2 || biome < 0.38f;
      exposed_stone = height > 21 || slope >= 5;
      dirt_depth = 3 + (int)(biome * 2.0f);

      for (y = 0; y < MAX_Y; y++) {
        if (y >= height && y <= water_level) {
          block = WATER;
        } else if (y >= height)  {
          block = AIR;
        } else if (isCave(x, y, z, height)) {
          block = AIR;
        } else if (y < height - dirt_depth) {
          block = STONE;
        } else if (do_sand) {
          block = SAND;
        } else if (exposed_stone && y >= height - 2) {
          block = STONE;
        } else if (y == height - 1) {
          block = GRASS;
        } else {
          block = DIRT;
        }

        blocks[x * MAX_Z * MAX_Y + y * MAX_Z + z] = block;
      }
    }
  }
  
  for (x = 0; x < MAX_X; x++) {
    for (z = 0; z < MAX_Z; z++) {
      if (random(1000) < perlin2d(x, z, 0.02, 2) * 8 - 2) {
        trySpawnTree(x, z);
      }
    }
  }
}
