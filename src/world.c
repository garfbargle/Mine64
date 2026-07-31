#include "world.h"
#include "day_cycle.h"
#include "blocks.h"
#include "noise.h"
#include "math.h"
#include "trees.h"
#include "graphics.h"
#include "details.h"
#include "edits.h"

u8 window_blocks[WINDOW_SLOTS][COLUMN_BLOCK_BYTES];
u32 window_keys[WINDOW_SLOTS];

/* See world.h.  Zero -- no ceiling -- until the gameplay callback arms it. */
OSTime stream_work_deadline;

/*
 * Window-key corruption forensics.  Run 6's SD post-mortem proved a freeze
 * family: an FPU unimplemented-operation fault inside guTranslate, fed by a
 * resident window key whose z field decoded hundreds of chunks away from a
 * player who had never been there -- with the player position provably sane.
 * Some stray store is landing inside window_keys.  The audit walks the keys
 * every streaming step, latches the first corrupt one it sees (its bit
 * pattern is the best clue to the writer), repairs the slot by evicting it,
 * and counts on the K row.  The column regenerates from the seed, so the
 * repair costs a moment of pop-in instead of a dead console.
 */
u32 window_key_faults;
u32 window_key_fault_value;
u32 window_key_fault_slot;

/*
 * How far a column has been built, per window slot.  The stages themselves are
 * declared in world.h, next to the functions that advance them; the reason
 * they exist is that decoration reaches across chunk boundaries, so a column
 * can only move on once its neighbours are far enough along to be written into
 * and read from.
 */
static u8 column_state[WINDOW_SLOTS];
/* Whether the deferred underground carve (caves, ores) has run.  Orthogonal
   to the build stages: decoration and the shell mesh only touch the surface,
   so a column can be fully decorated and still shallow until the player
   comes near enough for the underground to matter. */
static u8 column_deep[WINDOW_SLOTS];

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

void worldMarkFixedExtentBuilt() {
  int cx, cz;

  /*
   * A loaded world arrives as finished blocks with no generation history, so
   * every resident column has to be declared complete.  Without this the
   * states left by claiming the slots read as EMPTY, and streaming would
   * cheerfully generate fresh terrain straight over the save.
   */
  for (cx = 0; cx < CHUNKS_X; cx++) {
    for (cz = 0; cz < CHUNKS_Z; cz++) {
      if (windowColumnResident(cx, cz)) {
        column_state[WINDOW_SLOT(cx, cz)] = COLUMN_DECORATED;
        /* A loaded save carries its full underground. */
        column_deep[WINDOW_SLOT(cx, cz)] = TRUE;
      }
    }
  }
}

u8 worldFixedExtentResident() {
  int cx, cz;

  for (cx = 0; cx < CHUNKS_X; cx++) {
    for (cz = 0; cz < CHUNKS_Z; cz++) {
      if (!windowColumnResident(cx, cz)) {
        return FALSE;
      }
    }
  }
  return TRUE;
}

/* Legit play is within a few hundred chunks of origin; the 15-bit key fields
   reach +-16384.  Anything past this is not somewhere the player walked. */
#define WINDOW_KEY_SANE_CHUNKS 2000

void windowAuditKeys(void) {
  u32 slot;

  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    int cx, cz;

    if (!windowSlotResident(slot)) {
      continue;
    }
    cx = windowSlotChunkX(slot);
    cz = windowSlotChunkZ(slot);
    if (cx > WINDOW_KEY_SANE_CHUNKS || cx < -WINDOW_KEY_SANE_CHUNKS ||
        cz > WINDOW_KEY_SANE_CHUNKS || cz < -WINDOW_KEY_SANE_CHUNKS) {
      if (window_key_faults == 0) {
        window_key_fault_value = window_keys[slot];
        window_key_fault_slot = slot;
      }
      window_key_faults++;
      graphicsInvalidateColumnSlot(slot);
      window_keys[slot] = COLUMN_KEY_EMPTY;
      column_state[slot] = COLUMN_EMPTY;
      column_deep[slot] = FALSE;
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
      detailsEvictGeneratedColumn(windowSlotChunkX(slot),
        windowSlotChunkZ(slot));
      graphicsInvalidateColumnSlot(slot);
    }
    window_keys[slot] = key;
    column_state[slot] = COLUMN_EMPTY;
    column_deep[slot] = FALSE;
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
#define HASH_SALT_STRUCTURE_KIND 0xA24BAED5UL
#define HASH_SALT_STRUCTURE_X 0x9FB21C65UL
#define HASH_SALT_STRUCTURE_Z 0xC2B2AE35UL
#define HASH_SALT_STRUCTURE_VARIANT 0xD1B54A35UL
#define HASH_SALT_STRUCTURE_BLOCK 0x94D049BBUL

static u32 coordinateHash(int x, int y, int z, u32 salt) {
  u32 value = (u32) x * 0x8DA6B343UL;
  value ^= (u32) y * 0xD8163841UL;
  value ^= (u32) z * 0xCB1AB31FUL;
  value ^= world_seed + salt;
  value ^= value >> 13;
  value *= 0x85EBCA6BUL;
  return value ^ (value >> 16);
}

/* C's / and % truncate toward zero, so below zero a "cell" straddling the
   origin would be twice as wide and its in-cell offsets negative.  Flooring
   keeps the grid uniform on both sides; positive coordinates are untouched. */
static int floorDiv(int value, int divisor) {
  int quotient = value / divisor;
  return value % divisor < 0 ? quotient - 1 : quotient;
}

/*
 * One hash per small cell yields compact, recognizable ore veins without
 * paying for another octave-noise sample for every underground block.
 */
static u8 oreInCell(int x, int y, int z, int cell_size, u8 rarity) {
  int cell_x = floorDiv(x, cell_size);
  int cell_y = floorDiv(y, cell_size);
  int cell_z = floorDiv(z, cell_size);
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
  return absolute((float) (x - cell_x * cell_size - center_x)) +
    absolute((float) (y - cell_y * cell_size - center_y)) +
    absolute((float) (z - cell_z * cell_size - center_z)) <= 1.f;
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

  /* No world-edge clamp: past the canopy's reach the only boundary that
     matters is residency, and blockGet/blockSet already answer for that -- a
     non-resident sample reads BLOCK_NOT_RESIDENT (never AIR), so no leaf is
     placed there, exactly as the old 0..MAX clamp left the fixed world. */
  for (x = tx - 2; x < tx + 3; x++) {
    for (z = tz - 2; z < tz + 3; z++) {
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

  /* Residency is the only horizontal bound: a non-resident column reads
     BLOCK_NOT_RESIDENT, which is neither AIR nor GRASS, so planting outside
     the streamed world already fails here. */
  if (y == 0 || y >= MAX_Y ||
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

/*
 * Structures use a deliberately coarse 64x64 macrocell grid.  A cell has at
 * most one candidate, kept well inside its bounds, so a target chunk only has
 * one plan to consider.  More importantly, a chunk stamps only its own 8x8
 * intersection of that plan.  No cottage reaches over and writes a neighbour:
 * shuffled generation and eviction therefore reproduce the same bytes without
 * an oversized resident margin or a stored structure map.
 */
#define STRUCTURE_CELL_SHIFT 6
#define STRUCTURE_CELL_SIZE (1 << STRUCTURE_CELL_SHIFT)

#define STRUCTURE_NONE 0
#define STRUCTURE_HAMLET 1
#define STRUCTURE_RUIN 2

#define HAMLET_RADIUS 18
#define RUIN_RADIUS 10
#define TREE_CANOPY_RADIUS 2

typedef struct {
  int cell_x;
  int cell_z;
  int anchor_x;
  int anchor_z;
  int ground_y;
  u32 seed;
  u8 kind;
  u8 variant;
} StructurePlan;

/* A tiny direct-mapped cache avoids repeating the site's noise samples for
   every chunk intersecting a hamlet.  It is only an arithmetic memo: cache
   hits and misses produce the same plan. */
#define STRUCTURE_PLAN_CACHE_SIZE 8
static StructurePlan structure_plan_cache[STRUCTURE_PLAN_CACHE_SIZE];
static u8 structure_plan_valid[STRUCTURE_PLAN_CACHE_SIZE];

static const int hamlet_house_centers[4][2] = {
  {-11, -10}, {11, -10}, {-11, 11}, {11, 11}
};
static const int ruin_site_samples[5][2] = {
  {0, 0}, {-6, -6}, {6, -6}, {-6, 6}, {6, 6}
};

static int intAbsolute(int value) {
  return value < 0 ? -value : value;
}

static void hamletHouseCenter(const StructurePlan *plan, int house,
    int *house_x, int *house_z) {
  u32 hash = coordinateHash(plan->anchor_x, house, plan->anchor_z,
    HASH_SALT_STRUCTURE_VARIANT);

  *house_x = hamlet_house_centers[house][0] + (int) (hash % 3u) - 1;
  *house_z = hamlet_house_centers[house][1] +
    (int) ((hash >> 8) % 3u) - 1;
}

static void resetStructurePlanCache(void) {
  int i;

  for (i = 0; i < STRUCTURE_PLAN_CACHE_SIZE; i++) {
    structure_plan_valid[i] = FALSE;
  }
}

/* Return the unmodified terrain's exposed surface, rejecting water and peaks
   too high to leave vertical room for a roof.  This never reads resident block
   data, so deciding whether a town exists cannot depend on streaming order. */
static u8 structureSurfaceSample(int x, int z, int *surface_y) {
  int natural_height = terrainHeight(x, z);
  int water_level;
  int height = shapedSurfaceHeight(x, z, natural_height, &water_level);

  if (water_level >= 0 || height < SEA_LEVEL + 1 || height > MAX_Y - 7) {
    return FALSE;
  }
  *surface_y = height - 1;
  return TRUE;
}

static u8 structureSiteSuitable(StructurePlan *plan) {
  int allowed_relief = plan->kind == STRUCTURE_HAMLET ? 3 : 4;
  int sample;
  int min_y = MAX_Y;
  int max_y = -1;
  int sum_y = 0;

  /* The centre and four actual cottage sites catch bad ground without nine
     conservative edge samples rejecting an otherwise lovely riverside town. */
  for (sample = 0; sample < 5; sample++) {
    int y;
    int sample_x;
    int sample_z;

    if (sample == 0) {
      sample_x = 0;
      sample_z = 0;
    } else if (plan->kind == STRUCTURE_HAMLET) {
      hamletHouseCenter(plan, sample - 1, &sample_x, &sample_z);
    } else {
      sample_x = ruin_site_samples[sample][0];
      sample_z = ruin_site_samples[sample][1];
    }

    if (!structureSurfaceSample(plan->anchor_x + sample_x,
        plan->anchor_z + sample_z, &y)) {
      return FALSE;
    }
    if (y < min_y) {
      min_y = y;
    }
    if (y > max_y) {
      max_y = y;
    }
    sum_y += y;
  }
  if (max_y - min_y > allowed_relief) {
    return FALSE;
  }

  /* The average minimizes both cut and fill, while the relief test above
     keeps every foundation at most a couple of blocks deep. */
  plan->ground_y = (sum_y + 2) / 5;
  return TRUE;
}

static const StructurePlan *structurePlanForCell(int cell_x, int cell_z) {
  u32 cache_hash = (u32) cell_x * 0x9E3779B9UL ^
    (u32) cell_z * 0x85EBCA6BUL;
  u32 slot = cache_hash & (STRUCTURE_PLAN_CACHE_SIZE - 1);
  StructurePlan *plan = &structure_plan_cache[slot];
  u32 kind_hash;
  u32 offset_hash;
  u32 roll;

  if (structure_plan_valid[slot] && plan->cell_x == cell_x &&
      plan->cell_z == cell_z && plan->seed == world_seed) {
    return plan;
  }

  plan->cell_x = cell_x;
  plan->cell_z = cell_z;
  plan->seed = world_seed;
  plan->kind = STRUCTURE_NONE;
  plan->ground_y = 0;

  kind_hash = coordinateHash(cell_x, 0, cell_z,
    HASH_SALT_STRUCTURE_KIND);
  roll = kind_hash & 15u;
  if (roll < 7u) {
    plan->kind = STRUCTURE_HAMLET;
  } else if (roll < 9u) {
    plan->kind = STRUCTURE_RUIN;
  }

  offset_hash = coordinateHash(cell_x, 1, cell_z,
    HASH_SALT_STRUCTURE_X);
  plan->anchor_x = cell_x * STRUCTURE_CELL_SIZE +
    STRUCTURE_CELL_SIZE / 2 + (int) (offset_hash % 11u) - 5;
  offset_hash = coordinateHash(cell_x, 2, cell_z,
    HASH_SALT_STRUCTURE_Z);
  plan->anchor_z = cell_z * STRUCTURE_CELL_SIZE +
    STRUCTURE_CELL_SIZE / 2 + (int) (offset_hash % 11u) - 5;
  plan->variant = (u8) (coordinateHash(cell_x, 3, cell_z,
    HASH_SALT_STRUCTURE_VARIANT) & 7u);

  if (plan->kind != STRUCTURE_NONE && !structureSiteSuitable(plan)) {
    plan->kind = STRUCTURE_NONE;
  }
  structure_plan_valid[slot] = TRUE;
  return plan;
}

static int structureTopSolidY(int x, int z) {
  int y;

  for (y = MAX_Y - 1; y >= 0; y--) {
    u8 block = blockGet(x, y, z);

    if (block == BLOCK_NOT_RESIDENT) {
      return -1;
    }
    if (block != AIR && block != WATER) {
      return y;
    }
  }
  return -1;
}

static u8 agedStoneAt(int x, int y, int z) {
  return coordinateHash(x, y, z, HASH_SALT_STRUCTURE_BLOCK) % 5u == 0u ?
    MOSSY_COBBLESTONE : COBBLESTONE;
}

/* Flatten just a building's footprint and clear only the headroom it owns.
   The site test limits relief, so the capped three-block footing always joins
   the natural ground without producing costly cliff-sized foundation walls. */
static void prepareStructureColumn(int x, int z, int floor_y, int clear_top) {
  int surface_y = structureTopSolidY(x, z);
  int y;

  if (surface_y < 0) {
    return;
  }
  for (y = max(surface_y + 1, floor_y - 3); y <= floor_y; y++) {
    blockSet(x, y, z, COBBLESTONE);
  }
  for (y = floor_y + 1; y <= clear_top && y < MAX_Y; y++) {
    blockSet(x, y, z, AIR);
  }
}

static u8 hamletHousePresent(const StructurePlan *plan, int house) {
  /* Most plans trade one cottage for a little breathing room, making the
     silhouettes less stamp-like while retaining three homes.  The four-home
     plan is an uncommon, pleasantly bustling variation. */
  return plan->variant == 7u || house != (plan->variant & 3u);
}

/* Returns TRUE when this x/z belongs to the cottage, including its overhang. */
static u8 stampCottageColumn(const StructurePlan *plan, int house,
    int rx, int rz, int x, int z) {
  int house_x;
  int house_z;
  int dx;
  int dz;
  int adx;
  int adz;
  int floor_y = plan->ground_y;
  int y;
  int roof_y;
  int door_dx;
  int chimney_dx;
  int roof_cross;
  u8 is_wall;

  hamletHouseCenter(plan, house, &house_x, &house_z);
  dx = rx - house_x;
  dz = rz - house_z;
  adx = intAbsolute(dx);
  adz = intAbsolute(dz);

  if (adx > 4 || adz > 4) {
    return FALSE;
  }
  if (adx == 4 && adz == 4) {
    return TRUE;
  }

  if (adx <= 3 && adz <= 3) {
    prepareStructureColumn(x, z, floor_y, floor_y + 7);
    blockSet(x, floor_y, z,
      adx == 3 || adz == 3 ? COBBLESTONE : PLANKS);

    is_wall = adx == 3 || adz == 3;
    door_dx = house_x < 0 ? 3 : -3;
    if (is_wall) {
      for (y = 1; y <= 3; y++) {
        u8 doorway = dx == door_dx && dz == 0 && y <= 2;
        u8 window = y == 2 &&
          ((adz == 3 && dx == 0) || (adx == 3 && dz == 0));

        if (!doorway && !window) {
          blockSet(x, floor_y + y, z,
            adx == 3 && adz == 3 ? WOOD : PLANKS);
        }
      }
    }
  }

  /* A filled, stepped gable is cheap for the greedy mesher and reads much
     better at N64 distance than a flat box roof. */
  roof_cross = ((house + plan->variant) & 1) ? adx : adz;
  roof_y = floor_y + 4 + (4 - roof_cross) / 2;
  for (y = floor_y + 4; y <= roof_y; y++) {
    blockSet(x, y, z, BRICKS);
  }

  /* Alternating chimneys keep the rooflines lively without another model or
     texture.  They intentionally overwrite the roof at their intersection. */
  chimney_dx = ((house + plan->variant) & 1) ? -2 : 2;
  if (dx == chimney_dx && dz == 2) {
    for (y = floor_y + 1; y <= floor_y + 7 && y < MAX_Y; y++) {
      blockSet(x, y, z, COBBLESTONE);
    }
  }
  return TRUE;
}

static u8 stampWellColumn(const StructurePlan *plan, int rx, int rz,
    int x, int z) {
  int arx = intAbsolute(rx);
  int arz = intAbsolute(rz);
  int floor_y = plan->ground_y;

  if (arx > 2 || arz > 2) {
    return FALSE;
  }
  prepareStructureColumn(x, z, floor_y, floor_y + 5);
  blockSet(x, floor_y, z,
    arx == 2 || arz == 2 ? COBBLESTONE : WATER);
  if (arx == 2 || arz == 2) {
    blockSet(x, floor_y + 1, z, agedStoneAt(x, floor_y + 1, z));
  }
  if (arx == 2 && arz == 2) {
    blockSet(x, floor_y + 2, z, WOOD);
    blockSet(x, floor_y + 3, z, WOOD);
  }
  blockSet(x, floor_y + 4, z, PLANKS);
  if (rx == 0) {
    blockSet(x, floor_y + 5, z, WOOD);
  }
  return TRUE;
}

static u8 hamletPathAt(const StructurePlan *plan, int rx, int rz) {
  int house;

  if ((intAbsolute(rx) <= 1 && intAbsolute(rz) <= HAMLET_RADIUS) ||
      (intAbsolute(rz) <= 1 && intAbsolute(rx) <= HAMLET_RADIUS) ||
      (intAbsolute(rx) <= 4 && intAbsolute(rz) <= 4)) {
    return TRUE;
  }
  for (house = 0; house < 4; house++) {
    int hx;
    int hz;

    if (!hamletHousePresent(plan, house)) {
      continue;
    }
    hamletHouseCenter(plan, house, &hx, &hz);
    if (rz != hz) {
      continue;
    }
    if ((hx < 0 && rx >= hx + 4 && rx <= -2) ||
        (hx > 0 && rx <= hx - 4 && rx >= 2)) {
      return TRUE;
    }
  }
  return FALSE;
}

static void stampPathColumn(int x, int z) {
  int y = structureTopSolidY(x, z);

  if (y < 0 || y + 1 >= MAX_Y || blockGet(x, y + 1, z) == WATER) {
    return;
  }
  blockSet(x, y, z, agedStoneAt(x, y, z));
}

static void stampHamletColumn(const StructurePlan *plan, int cx, int cz) {
  int base_x = cx * CHUNK_SIZE;
  int base_z = cz * CHUNK_SIZE;
  int bx, bz;

  for (bx = 0; bx < CHUNK_SIZE; bx++) {
    for (bz = 0; bz < CHUNK_SIZE; bz++) {
      int x = base_x + bx;
      int z = base_z + bz;
      int rx = x - plan->anchor_x;
      int rz = z - plan->anchor_z;
      int house;
      u8 handled;

      if (intAbsolute(rx) > HAMLET_RADIUS ||
          intAbsolute(rz) > HAMLET_RADIUS) {
        continue;
      }
      handled = stampWellColumn(plan, rx, rz, x, z);
      for (house = 0; !handled && house < 4; house++) {
        if (hamletHousePresent(plan, house)) {
          handled = stampCottageColumn(plan, house, rx, rz, x, z);
        }
      }
      if (!handled && hamletPathAt(plan, rx, rz)) {
        stampPathColumn(x, z);
      }
    }
  }
}

static void stampRuinColumn(const StructurePlan *plan, int cx, int cz) {
  int base_x = cx * CHUNK_SIZE;
  int base_z = cz * CHUNK_SIZE;
  int bx, bz;

  for (bx = 0; bx < CHUNK_SIZE; bx++) {
    for (bz = 0; bz < CHUNK_SIZE; bz++) {
      int x = base_x + bx;
      int z = base_z + bz;
      int rx = x - plan->anchor_x;
      int rz = z - plan->anchor_z;
      int arx = intAbsolute(rx);
      int arz = intAbsolute(rz);
      u32 rubble_hash;

      if (arx > RUIN_RADIUS || arz > RUIN_RADIUS) {
        continue;
      }
      if (arx <= 4 && arz <= 4) {
        int y;

        prepareStructureColumn(x, z, plan->ground_y, plan->ground_y + 8);
        blockSet(x, plan->ground_y, z,
          (arx <= 1 && arz <= 1) ? BRICKS : agedStoneAt(x,
            plan->ground_y, z));
        if (arx == 4 || arz == 4) {
          int wall_height = 3 + (int) (coordinateHash(x, 0, z,
            HASH_SALT_STRUCTURE_BLOCK) % 5u);
          u8 doorway = rz == 4 && arx <= 1;

          if (arx == 4 && arz == 4) {
            wall_height = min(wall_height + 1, 8);
          }
          for (y = 1; y <= wall_height; y++) {
            if (!(doorway && y <= 3)) {
              blockSet(x, plan->ground_y + y, z,
                agedStoneAt(x, plan->ground_y + y, z));
            }
          }
        }
        continue;
      }

      /* A short approach and a sparse collapse field make the landmark read
         as a ruin instead of an isolated procedural box. */
      if (arx <= 1 && rz >= 4 && rz <= RUIN_RADIUS) {
        stampPathColumn(x, z);
        continue;
      }
      rubble_hash = coordinateHash(x, 0, z, HASH_SALT_STRUCTURE_BLOCK);
      if (rubble_hash % 13u == 0u) {
        int y = structureTopSolidY(x, z);

        if (y >= 0 && y + 1 < MAX_Y && blockGet(x, y + 1, z) != WATER) {
          blockSet(x, y, z, agedStoneAt(x, y, z));
          if ((rubble_hash & 32u) != 0u && y + 2 < MAX_Y) {
            blockSet(x, y + 1, z, MOSSY_COBBLESTONE);
          }
        }
      }
    }
  }
}

static void stampStructureColumn(int cx, int cz) {
  int cell_x = floorDiv(cx * CHUNK_SIZE, STRUCTURE_CELL_SIZE);
  int cell_z = floorDiv(cz * CHUNK_SIZE, STRUCTURE_CELL_SIZE);
  const StructurePlan *plan = structurePlanForCell(cell_x, cell_z);

  if (plan->kind == STRUCTURE_HAMLET) {
    stampHamletColumn(plan, cx, cz);
  } else if (plan->kind == STRUCTURE_RUIN) {
    stampRuinColumn(plan, cx, cz);
  }
}

/* Pure exclusion, expanded by the tree's full two-block canopy reach.  A tree
   can never be admitted merely because its destination chunk has not stamped
   the town yet, nor can leaves intrude from a root just outside its road. */
static u8 structureExcludesTreeAt(int x, int z) {
  int cell_x = floorDiv(x, STRUCTURE_CELL_SIZE);
  int cell_z = floorDiv(z, STRUCTURE_CELL_SIZE);
  const StructurePlan *plan = structurePlanForCell(cell_x, cell_z);
  int radius;

  if (plan->kind == STRUCTURE_NONE) {
    return FALSE;
  }
  radius = (plan->kind == STRUCTURE_HAMLET ? HAMLET_RADIUS : RUIN_RADIUS) +
    TREE_CANOPY_RADIUS;
  return intAbsolute(x - plan->anchor_x) <= radius &&
    intAbsolute(z - plan->anchor_z) <= radius;
}

/* Keep the original small waystones as a second scale of discovery between
   towns.  Their plan is coordinate-pure too; each destination x/z asks only
   whether it is a pillar, east stone or south stone and writes that one local
   stack.  That preserves the old three-part silhouette without cross-column
   stores. */
#define WAYSTONE_COLUMN_ODDS 500

static u8 waystoneSurfaceY(int x, int z, int *surface_y) {
  int natural_height = terrainHeight(x, z);
  int water_level;
  int height = shapedSurfaceHeight(x, z, natural_height, &water_level);

  if (water_level >= 0 || height <= SEA_LEVEL) {
    return FALSE;
  }
  *surface_y = height - 1;
  return TRUE;
}

static u8 waystoneCandidateAt(int x, int z, int *root_y, int *east_y,
    int *south_y) {
  if (coordinateHash(x, 0, z, HASH_SALT_WAYSTONE) %
      WAYSTONE_COLUMN_ODDS != 0u || structureExcludesTreeAt(x, z)) {
    return FALSE;
  }
  if (!waystoneSurfaceY(x, z, root_y) ||
      !waystoneSurfaceY(x + 2, z, east_y) ||
      !waystoneSurfaceY(x, z + 2, south_y) ||
      *root_y < SEA_LEVEL + 1 ||
      intAbsolute(*east_y - *root_y) > 1 ||
      intAbsolute(*south_y - *root_y) > 1) {
    return FALSE;
  }
  return TRUE;
}

static void stampWaystonesColumn(int cx, int cz) {
  int base_x = cx * CHUNK_SIZE;
  int base_z = cz * CHUNK_SIZE;
  int bx, bz;

  for (bx = 0; bx < CHUNK_SIZE; bx++) {
    for (bz = 0; bz < CHUNK_SIZE; bz++) {
      int x = base_x + bx;
      int z = base_z + bz;
      int root_y, east_y, south_y;

      if (waystoneCandidateAt(x, z, &root_y, &east_y, &south_y)) {
        int height = 2 + (int) (coordinateHash(x, 0, z,
          HASH_SALT_WAYSTONE_HEIGHT) % 4u);
        int part;

        for (part = 1; part <= height && root_y + part < MAX_Y; part++) {
          blockSet(x, root_y + part, z,
            part == height || (part & 1) ? MOSSY_COBBLESTONE : COBBLESTONE);
        }
      }
      if (waystoneCandidateAt(x - 2, z, &root_y, &east_y, &south_y) &&
          east_y + 1 < MAX_Y) {
        blockSet(x, east_y + 1, z, MOSSY_COBBLESTONE);
      }
      if (waystoneCandidateAt(x, z - 2, &root_y, &east_y, &south_y) &&
          south_y + 1 < MAX_Y) {
        blockSet(x, south_y + 1, z, MOSSY_COBBLESTONE);
      }
    }
  }
}

/*
 * TRUE when a tree roots at this column.  The perlin field sets the local
 * density and the hash decides whether this particular column draws one.
 *
 * Biomes deliberately do not appear here.  trySpawnTree plants only on GRASS,
 * so desert, highland and scree refuse trees on their own -- for free, and
 * without this stage needing a height patch it does not have.  Forest and
 * plains share a palette and are separated by this field's own variation,
 * which is why the biome does not scale it either.
 */
static u8 treeSeededAt(int x, int z) {
  float density = perlin2d(x, z, 0.02f, 2) * 8.f - 2.f;

  return (float) (coordinateHash(x, 0, z, HASH_SALT_TREE) % 1000u) < density &&
    !structureExcludesTreeAt(x, z);
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
#define WORLD_GEN_STRUCTURES 2
#define WORLD_GEN_TREES 3

static u8 world_gen_stage = WORLD_GEN_IDLE;
static int world_gen_x;
static int world_gen_z;


/*
 * Biomes.
 *
 * A biome is never stored.  Like every other decision generation makes it is
 * re-derived from the coordinate and the seed, so an evicted column comes back
 * identical -- which is the property the whole streaming design rests on.
 *
 * It is also derived entirely from signals the column has already computed:
 * the climate field that used to set nothing but the dirt depth, the shaped
 * surface height, and the local slope.  Classifying therefore costs no noise
 * sample that was not already being paid for.
 *
 * With all sixteen block IDs spoken for there is no room for biome-specific
 * blocks, so a biome is a palette drawn from what already exists -- which
 * block the surface shows, what lies under it, and how thick that band is.
 * Four of the five read differently at draw distance (grass, sand, stone,
 * cobblestone); plains and forest share a palette and are told apart by tree
 * density, which the tree field already varies on its own.
 */
#define BIOME_PLAINS   0
#define BIOME_FOREST   1
#define BIOME_DESERT   2
#define BIOME_HIGHLAND 3
#define BIOME_SCREE    4
#define BIOME_COUNT    5

typedef struct {
  u8 surface;    /* the one exposed top block */
  u8 subsurface; /* the band between that surface and the stone beneath it */
  u8 depth;      /* how thick the band is */
} BiomePalette;

static const BiomePalette biome_palettes[BIOME_COUNT] = {
  {GRASS,       DIRT,  3},  /* plains                            */
  {GRASS,       DIRT,  5},  /* forest: deeper soil under the trees */
  {SAND,        SAND,  4},  /* desert                            */
  {STONE,       STONE, 3},  /* highland: bare rock               */
  {COBBLESTONE, STONE, 2}   /* scree: weathered, broken rock     */
};

/*
 * The climate field.  This is the sample that used to be called `biome` and
 * fed only terrainDirtDepth.  Widening it from 0.025 to 0.006 turns its
 * ~40-block patches into ~170-block regions, which is the scale a biome has
 * to be for walking out of one to mean anything.  Measured over a 800x800
 * block area the field spans 0.14 to 0.87 about a mean of 0.505, which is
 * where the thresholds below come from.
 *
 * Widening it also broadens what it already governed -- soil depth, and how
 * far sand climbs a dry bank -- which is the intended trade: those were
 * per-patch details and are now per-region ones.
 */
static float climateAt(int x, int z) {
  return perlin2d(x + 883, z + 521, 0.006f, 3);
}

/*
 * Bare rock outranks climate: a slope too steep or a summit too high holds no
 * soil whatever the weather, which is the rule the old `exposed_stone` test
 * encoded and this keeps.  Climate then splits that rock into clean stone and
 * weathered scree, and splits the ground that does hold soil into desert,
 * plains and forest.
 *
 * The cuts fall at roughly 14% desert, 59% plains and 27% forest, so plains
 * stays the broad and mostly level route through the world that terrainHeight
 * is shaped to provide.
 */
static u8 biomeAt(int height, float slope, float climate) {
  if (height > 21 || slope >= 5.f) {
    return climate < 0.45f ? BIOME_SCREE : BIOME_HIGHLAND;
  }
  if (climate < 0.32f) {
    return BIOME_DESERT;
  }
  return climate > 0.62f ? BIOME_FOREST : BIOME_PLAINS;
}

/*
 * Everything the surface fill and the deferred underground carve have to
 * agree on for one block column.
 *
 * They used to sample the climate field independently, and stayed consistent
 * only because the two copies happened to be spelled the same way.  The dirt
 * depth it yields decides where carveTerrainColumn is allowed to place ore,
 * so any divergence would make a column that was deepened later differ from
 * one generated deep.  Nothing tests that: the harness compares streamed
 * worlds against streamed worlds, and every column takes the same route
 * through shallow-then-deepen, so a systematic disagreement between the two
 * halves would cancel out and pass.  Structure is the guarantee instead --
 * one function answers for both callers, so they cannot drift apart.
 *
 * Only valid while the height patch covering x/z is filled: the slope test
 * reads the four neighbouring block columns.
 */
typedef struct {
  int height;
  int water_level;
  int dirt_depth;
  u8 surface;
  u8 subsurface;
} ColumnProfile;

static void columnProfile(int x, int z, ColumnProfile *profile) {
  const BiomePalette *palette;
  float slope;
  u8 biome;

  profile->height = shapedSurfaceHeight(x, z, patchHeight(x, z),
    &profile->water_level);
  slope = absolute((float) (patchHeight(x + 1, z) - patchHeight(x - 1, z))) +
    absolute((float) (patchHeight(x, z + 1) - patchHeight(x, z - 1)));
  biome = biomeAt(profile->height, slope, climateAt(x, z));
  palette = &biome_palettes[biome];
  profile->surface = palette->surface;
  profile->subsurface = palette->subsurface;
  profile->dirt_depth = palette->depth;

  /* Shorelines are sand in every biome, and a desert carries its sand a
     little further up the bank.  This is the one place the palette does not
     get the last word: at the waterline it is the water the eye reads, not
     the climate. */
  if (profile->height <= SEA_LEVEL + 1 ||
      (biome == BIOME_DESERT && profile->height <= SEA_LEVEL + 3)) {
    profile->surface = SAND;
    profile->subsurface = SAND;
  }
}

/*
 * The underground: carve the cave fields and place the ore veins into a
 * block column whose solid fill is already written.  Both the deep generator
 * and the deferred deepen pass go through this one function, which is what
 * makes "generate deep directly" and "generate shallow now, deepen on
 * approach" byte-identical by construction -- the invariant the host harness
 * checks across every ordering.
 *
 * This is also the expensive half of terrain: two 3D noise samples per
 * underground block.  Deferring it for far columns is what lets streaming
 * outrun a sprinting player.
 */
static void carveTerrainColumn(int x, int z, int height, int dirt_depth) {
  int y;

  for (y = 1; y < height; y++) {
    if (isCave(x, y, z, height)) {
      blockSet(x, y, z, AIR);
    } else if (y < height - dirt_depth) {
      if (y < 13 && oreInCell(x, y, z, 5, 7)) {
        blockSet(x, y, z, IRON_ORE);
      } else if (y < 23 && oreInCell(x, y, z, 4, 5)) {
        blockSet(x, y, z, COAL_ORE);
      }
    }
  }
}

/*
 * The surface fill: everything a column needs to be walked on, decorated and
 * shown as a distant shell.  Caves never breach the top three blocks under
 * the surface (isCave keeps a roof), so a shallow column is walkably
 * identical to a deep one -- the underground stays unknown until the player
 * is close enough for it to matter.
 */
static void generateTerrainColumn(int x, int z, u8 deep) {
  ColumnProfile profile;
  int y;
  u8 block;

  columnProfile(x, z, &profile);

  /*
   * The biome resolves to three block IDs and a depth before the loop starts,
   * so what used to be a seven-way chain of terrain special cases is now five
   * comparisons against values already in registers.  Bare rock reads as
   * stone through the whole band rather than as the old two-block cap over
   * dirt, which is both simpler and what an exposed summit should look like.
   */
  for (y = 0; y < MAX_Y; y++) {
    if (y >= profile.height) {
      block = y <= profile.water_level ? WATER : AIR;
    } else if (y == 0) {
      block = BEDROCK;
    } else if (y < profile.height - profile.dirt_depth) {
      block = STONE;
    } else if (y == profile.height - 1) {
      block = profile.surface;
    } else {
      block = profile.subsurface;
    }

    blockSet(x, y, z, block);
  }

  if (deep) {
    carveTerrainColumn(x, z, profile.height, profile.dirt_depth);
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

/* The fixed save extent always generates deep: the v10 format packs every
   block of it, so a shallow column there would write uncarved stone into a
   player's save. */
static u8 columnInFixedExtent(int cx, int cz) {
  return cx >= 0 && cx < CHUNKS_X && cz >= 0 && cz < CHUNKS_Z;
}

u8 worldColumnDeep(int cx, int cz) {
  return windowColumnResident(cx, cz) && column_deep[WINDOW_SLOT(cx, cz)];
}

/* Claim a slot and fill it with bare terrain.  Nothing here reads a
   neighbour, so a column can always take this step alone. */
void worldGenerateColumnTerrain(int cx, int cz) {
  int base_x = cx * CHUNK_SIZE;
  int base_z = cz * CHUNK_SIZE;
  int bx, bz;
  u8 deep = columnInFixedExtent(cx, cz);

  windowClaimColumn(cx, cz);
  fillHeightPatch(base_x, base_z);
  for (bx = 0; bx < CHUNK_SIZE; bx++) {
    for (bz = 0; bz < CHUNK_SIZE; bz++) {
      generateTerrainColumn(base_x + bx, base_z + bz, deep);
    }
  }
  column_state[WINDOW_SLOT(cx, cz)] = COLUMN_TERRAIN;
  column_deep[WINDOW_SLOT(cx, cz)] = deep;
}

/* Run the deferred underground carve on a shallow column that the player has
   come near.  The surface is untouched -- caves keep a roof -- so nothing
   the player can currently see or stand on changes. */
static void worldDeepenColumn(int cx, int cz) {
  int base_x = cx * CHUNK_SIZE;
  int base_z = cz * CHUNK_SIZE;
  int bx, bz;

  fillHeightPatch(base_x, base_z);
  for (bx = 0; bx < CHUNK_SIZE; bx++) {
    for (bz = 0; bz < CHUNK_SIZE; bz++) {
      ColumnProfile profile;

      /* Same profile the surface fill used, from the same function, so the
         dirt depth the ore placement keys off cannot disagree with the one
         the blocks above it were written from. */
      columnProfile(base_x + bx, base_z + bz, &profile);
      carveTerrainColumn(base_x + bx, base_z + bz, profile.height,
        profile.dirt_depth);
    }
  }
  column_deep[WINDOW_SLOT(cx, cz)] = TRUE;
}

/* Nearest shallow column in the deepen ring, or FALSE when they are all
   deep.  Same nearest-first shape as the stage scans. */
static u8 nearestShallowColumn(int pcx, int pcz, int *out_cx, int *out_cz) {
  int dx, dz;
  int best_distance = 0;
  u8 found = FALSE;

  for (dx = -STREAM_DEEPEN_RADIUS; dx <= STREAM_DEEPEN_RADIUS; dx++) {
    for (dz = -STREAM_DEEPEN_RADIUS; dz <= STREAM_DEEPEN_RADIUS; dz++) {
      int cx = pcx + dx;
      int cz = pcz + dz;
      int distance = dx * dx + dz * dz;

      if (worldColumnState(cx, cz) < COLUMN_TERRAIN ||
          column_deep[WINDOW_SLOT(cx, cz)]) {
        continue;
      }
      if (!found || distance < best_distance) {
        best_distance = distance;
        *out_cx = cx;
        *out_cz = cz;
        found = TRUE;
      }
    }
  }
  return found;
}

/* Returns FALSE when the column's neighbours are not built far enough yet and
   the caller should come back to it. */
u8 worldAdvanceColumnDecoration(int cx, int cz) {
  int base_x = cx * CHUNK_SIZE;
  int base_z = cz * CHUNK_SIZE;
  int bx, bz;
  u32 slot = WINDOW_SLOT(cx, cz);

  if (column_state[slot] == COLUMN_TERRAIN) {
    /* Structure plans are coordinate-pure, and the stamper writes only this
       target column.  It therefore needs no neighbour gate of its own. */
    stampStructureColumn(cx, cz);
    stampWaystonesColumn(cx, cz);
    column_state[slot] = COLUMN_STRUCTURED;
    return FALSE;
  }

  if (column_state[slot] == COLUMN_STRUCTURED) {
    /* Writes canopy up to two blocks out, and must not root in ground a
       neighbour's structure is about to take. */
    if (!neighboursReached(cx, cz, COLUMN_STRUCTURED)) {
      return FALSE;
    }
    for (bx = 0; bx < CHUNK_SIZE; bx++) {
      for (bz = 0; bz < CHUNK_SIZE; bz++) {
        if (treeSeededAt(base_x + bx, base_z + bz)) {
          trySpawnTree(base_x + bx, base_z + bz);
        }
      }
    }
    /* Regenerated terrain is only the base layer.  Saved player edits and
       persistent detail proxies are overlays, so they win over both the
       deterministic town and any tree that would otherwise grow through it. */
    worldApplyEditsToColumn(cx, cz);
    detailsApplyColumn(cx, cz);
    column_state[slot] = COLUMN_DECORATED;
  }
  return column_state[slot] == COLUMN_DECORATED;
}

/*
 * Residency is two discs, not one, and not a frustum cone.  Walking exposes a
 * single new row of columns, but turning around invalidates a whole cone at
 * once, so anything shaped by facing stalls every time the player spins.
 *
 * The terrain disc is deliberately one column wider than the decorated disc,
 * which is the invariant neighboursReached depends on: a column only decorates
 * when everything it might write a canopy into is already claimed.
 *
 * Radius 7 spans 15 columns, which is the most a 16-slot window can hold
 * without two live columns aliasing onto one slot.
 */
/*
 * Square rings, not Euclidean discs.  Both are omnidirectional -- the thing
 * that matters, since spinning must not invalidate residency -- but the margin
 * each stage needs behaves very differently.  A column at Euclidean distance d
 * has neighbours out at d + sqrt(2), and with three chained stages that margin
 * is paid twice, collapsing a radius 7 terrain ring into a radius 3 decorated
 * ring: 31 columns, a patch of ground barely wider than the player.
 *
 * Under Chebyshev distance a neighbour is simply one ring further out, so each
 * stage costs exactly one column of radius and the decorated ring stays wide.
 */
static int chebyshev(int dx, int dz) {
  int ax = dx < 0 ? -dx : dx;
  int az = dz < 0 ? -dz : dz;

  return ax > az ? ax : az;
}

/*
 * Prefetch bias, in chunks.  Ring membership never moves -- it stays a disc
 * around the player, so spinning cannot invalidate residency -- but the
 * *order* candidates are ranked in leans toward where the player is heading.
 * Without it the ring edge directly ahead is by definition the last thing
 * nearest-first builds, which is exactly where a walking player is about to
 * arrive.
 */
static int stream_bias_cx;
static int stream_bias_cz;

void worldSetStreamBias(int bias_cx, int bias_cz) {
  stream_bias_cx = bias_cx;
  stream_bias_cz = bias_cz;
}

/* Nearest-first around a point slightly ahead of the player, so the ground
   being walked toward fills before the fringe being walked away from. */
static u8 nearestColumnNeeding(int pcx, int pcz, int radius, u8 state,
    int *out_cx, int *out_cz) {
  int dx, dz;
  int best_distance = 0;
  u8 found = FALSE;

  for (dx = -radius; dx <= radius; dx++) {
    for (dz = -radius; dz <= radius; dz++) {
      int bdx = dx - stream_bias_cx;
      int bdz = dz - stream_bias_cz;
      int distance = bdx * bdx + bdz * bdz;
      int cx, cz;

      /* Ring membership is Chebyshev from the player; only the ranking is
         biased.  The residency invariants never depend on order. */
      if (chebyshev(dx, dz) > radius) {
        continue;
      }
      cx = pcx + dx;
      cz = pcz + dz;
      if (worldColumnState(cx, cz) >= state) {
        continue;
      }
      if (!found || distance < best_distance) {
        best_distance = distance;
        *out_cx = cx;
        *out_cz = cz;
        found = TRUE;
      }
    }
  }
  return found;
}

/*
 * Let go of everything outside the ring.
 *
 * Without this a column is only ever displaced when some other column needs
 * its exact slot, so walking away from spawn leaves the whole starting extent
 * resident and finished on top of everything newly streamed in.  Compaction
 * rebuilds every resident finished column, so that set growing without bound
 * means the mesh arena crosses its reserve, compacts, comes out just as full,
 * and compacts again -- and while it is compacting no newly streamed column
 * gets compiled at all.  Terrain exists and is walkable, and none of it is
 * ever drawn.
 *
 * Released columns cost nothing to lose: generation is a pure function of the
 * coordinate and the seed, so an unmodified column comes back identical.
 *
 * The radius has to match the terrain ring exactly rather than allow a margin
 * of hysteresis -- a disc of radius 7 spans 15 columns, and anything wider
 * would put two live columns on one slot.
 */
static void releaseColumnsOutsideRing(int pcx, int pcz) {
  u32 slot;

  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    int cx, cz, dx, dz;

    if (!windowSlotResident(slot)) {
      continue;
    }
    cx = windowSlotChunkX(slot);
    cz = windowSlotChunkZ(slot);
    dx = cx - pcx;
    dz = cz - pcz;
    if (chebyshev(dx, dz) <= STREAM_TERRAIN_RADIUS) {
      continue;
    }
    treesEvictColumn(cx, cz);
    detailsEvictGeneratedColumn(cx, cz);
    graphicsInvalidateColumnSlot(slot);
    window_keys[slot] = COLUMN_KEY_EMPTY;
    column_state[slot] = COLUMN_EMPTY;
    column_deep[slot] = FALSE;
  }
}

/*
 * A neighbour that has not been compiled yet will read this column correctly
 * whenever it does get compiled, so only an already-finished one needs
 * redoing.  Marking all four unconditionally re-meshes most columns several
 * times over as the ring advances, and every rebuild orphans its predecessor
 * in the arena, which is what drags compaction forward.
 */
static void remeshDecoratedNeighbour(int cx, int cz) {
  if (worldColumnState(cx, cz) == COLUMN_DECORATED) {
    graphicsMarkColumnDirty(cx, cz);
  }
}

static u8 stream_guarantee_stage;

u8 worldStreamStageGuaranteed(u8 stage) {
  return stream_guarantee_stage == stage;
}

void stepWorldStreaming(int pcx, int pcz, u32 terrain_budget,
    u32 decorate_budget) {
  int cx = pcx, cz = pcz;
  u8 stage_did_one;

  windowAuditKeys();
  releaseColumnsOutsideRing(pcx, pcz);

  /*
   * Whose turn it is to run past the deadline (see the stage table in
   * world.h).  Advanced here, before any stage consults it, so this frame's
   * world stages and its mesh queue -- which draw() processes after this
   * returns -- agree on the turn.
   *
   * The exemption itself exists because a walking player generates pending
   * terrain continuously, and a terrain column alone can exhaust the whole
   * time budget -- run purely on the deadline, terrain starved decoration
   * for as long as the player kept moving, and meshing waits on decoration:
   * blocks existed (mobs walked on them) while the mesh sat seconds behind.
   * When the deadline has room, none of this matters: every stage runs to
   * its column budget exactly as before.
   */
  stream_guarantee_stage++;
  if (stream_guarantee_stage >= STREAM_STAGE_COUNT) {
    stream_guarantee_stage = 0;
  }

  stage_did_one = FALSE;
  while (terrain_budget > 0 &&
      ((worldStreamStageGuaranteed(STREAM_STAGE_TERRAIN) && !stage_did_one) ||
        !streamWorkExpired()) &&
      nearestColumnNeeding(pcx, pcz, STREAM_TERRAIN_RADIUS, COLUMN_TERRAIN,
        &cx, &cz)) {
    worldGenerateColumnTerrain(cx, cz);
    terrain_budget--;
    stage_did_one = TRUE;
  }

  /*
   * Stage-major, exactly like the three whole-world passes: every column in
   * reach is waystoned before any column grows a tree.  Advancing one column
   * as far as it can go instead would spend the whole budget retrying the
   * nearest column, which cannot finish until neighbours that never get a turn
   * catch up.
   */
  /*
   * Deepen the columns the player is approaching: the deferred underground
   * carve must land before the near ring's full-detail mesh wants to show
   * caves, and before the player can dig.  One guaranteed step keeps the
   * deepen frontier ahead of a sprint without ever stacking a second full
   * carve into one callback.
   */
  stage_did_one = FALSE;
  while (((worldStreamStageGuaranteed(STREAM_STAGE_DEEPEN) && !stage_did_one) ||
        !streamWorkExpired()) &&
      nearestShallowColumn(pcx, pcz, &cx, &cz)) {
    worldDeepenColumn(cx, cz);
    stage_did_one = TRUE;
  }

  stage_did_one = FALSE;
  while (decorate_budget > 0 &&
      ((worldStreamStageGuaranteed(STREAM_STAGE_STRUCTURES) &&
        !stage_did_one) || !streamWorkExpired()) &&
      nearestColumnNeeding(pcx, pcz, STREAM_STRUCTURE_RADIUS,
        COLUMN_STRUCTURED,
        &cx, &cz)) {
    worldAdvanceColumnDecoration(cx, cz);
    decorate_budget--;
    stage_did_one = TRUE;
  }

  /*
   * Anything finished inside the mesh ring must actually have geometry.
   *
   * A dirty mark is only set at the moment a column *becomes* decorated, so a
   * column that lost its mesh afterwards is never asked for again: it is
   * already decorated, no stage transition remains to re-queue it, and it
   * stays invisible permanently.  That happens whenever a compaction runs
   * short before reaching it, and whenever a column leaves the mesh ring and
   * later comes back.  Standing at the edge of it, nothing appears however
   * long you wait -- the queue is empty and the arena has room, because
   * nothing knows the column is missing.
   */
  {
    int dx, dz;

    for (dx = -STREAM_TREE_RADIUS; dx <= STREAM_TREE_RADIUS; dx++) {
      for (dz = -STREAM_TREE_RADIUS; dz <= STREAM_TREE_RADIUS; dz++) {
        int rx = pcx + dx;
        int rz = pcz + dz;

        if (worldColumnState(rx, rz) == COLUMN_DECORATED &&
            graphicsColumnNeedsMesh(rx, rz)) {
          graphicsMarkColumnDirty(rx, rz);
        }
      }
    }
  }

  stage_did_one = FALSE;
  while (decorate_budget > 0 &&
      ((worldStreamStageGuaranteed(STREAM_STAGE_TREES) && !stage_did_one) ||
        !streamWorkExpired()) &&
      nearestColumnNeeding(pcx, pcz, STREAM_TREE_RADIUS, COLUMN_DECORATED,
        &cx, &cz)) {
    stage_did_one = TRUE;
    if (worldAdvanceColumnDecoration(cx, cz)) {
      /* Finished columns are the only ones worth compiling; a half-decorated
         one would have to be thrown away when its trees arrive. */
      graphicsMarkColumnDirty(cx, cz);
      /*
       * The neighbours have to be recompiled too.  Meshing reads one block
       * across each horizontal boundary to decide whether a face is hidden,
       * and when those neighbours were built this column did not exist yet --
       * they culled against BLOCK_NOT_RESIDENT and left a hole at the seam.
       */
      remeshDecoratedNeighbour(cx - 1, cz);
      remeshDecoratedNeighbour(cx + 1, cz);
      remeshDecoratedNeighbour(cx, cz - 1);
      remeshDecoratedNeighbour(cx, cz + 1);
    }
    decorate_budget--;
  }
}

void beginWorldGeneration() {
  u32 slot;

  /* One draw of entropy fixes the world; the gameplay RNG starts from the same
     place but is free to wander, because nothing reproducible reads it. */
  world_seed = (u32) osGetTime();
  seed = world_seed;
  setDayCycleWorldTicks(DAY_CYCLE_START_TICK);
  initTrees();
  resetStructurePlanCache();
  /* A new world starts at spawn, near coordinate zero.  Without this, a menu
     visited after a long walk would build the scenic preview against an
     origin hundreds of blocks away and overflow its matrices. */
  graphicsSetRenderOrigin(0, 0);

  windowClaimFixedExtent();
  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    column_state[slot] = COLUMN_EMPTY;
    column_deep[slot] = FALSE;
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
  if (world_gen_stage == WORLD_GEN_STRUCTURES) {
    return (u8) (90 + (world_gen_x * 5) / CHUNKS_X);
  }
  return (u8) (95 + (world_gen_x * 5) / CHUNKS_X);
}

/*
 * Walks chunk columns rather than block columns now, in three whole-extent
 * passes.  The passes are what satisfy the neighbour gates everywhere at once:
 * by the time the structure pass reaches a column every column has terrain,
 * and by the time the tree pass reaches it every column has its structures. A
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
