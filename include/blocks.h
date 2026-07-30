#ifndef BLOCKS_H
#define BLOCKS_H

#define AIR         0
#define DIRT        1
#define STONE       2
#define GRASS       3
#define COBBLESTONE 4
#define SAND        5
#define WOOD        6
#define LEAVES      7
#define PLANKS      8
#define BRICKS      9
#define CRAFTING_TABLE 10
#define COAL_ORE    11
#define IRON_ORE    12
#define BEDROCK     13
#define MOSSY_COBBLESTONE 14
#define WATER       15

/* The packed world has exactly sixteen block IDs.  Inventory-only items live
   above that namespace so all four remaining terrain IDs can deepen mining. */
#define FIRST_PLACEABLE_BLOCK DIRT
#define BLOCK_TYPE_COUNT      CRAFTING_TABLE
#define MAX_BLOCK_ID          WATER

#define BLOCK_IS_VALID(block) ((block) <= MAX_BLOCK_ID)
#define BLOCK_IS_SOLID(block) ((block) != AIR && (block) != WATER)

#endif /* BLOCKS_H */
