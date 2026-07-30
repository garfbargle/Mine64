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
/* Water is terrain-only rather than an inventory item: IDs 11-14 are tools
   and saplings, and the packed save format supports all four-bit IDs. */
#define WATER       15

/* Block IDs are also placeable item IDs.  The nine-slot hotbar starts with
   the base blocks; crafted blocks can be moved into it from storage. */
#define FIRST_PLACEABLE_BLOCK DIRT
#define BLOCK_TYPE_COUNT      CRAFTING_TABLE
#define MAX_BLOCK_ID          WATER

#define BLOCK_IS_VALID(block) ((block) <= BLOCK_TYPE_COUNT || (block) == WATER)
#define BLOCK_IS_SOLID(block) ((block) != AIR && (block) != WATER)

#endif /* BLOCKS_H */
