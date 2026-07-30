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

/* The creative hotbar exposes every placeable block.  Block IDs intentionally
   match their one-based hotbar positions so a selected slot can be saved and
   used directly when placing blocks. */
#define FIRST_PLACEABLE_BLOCK DIRT
#define BLOCK_TYPE_COUNT      BRICKS

#endif /* BLOCKS_H */
