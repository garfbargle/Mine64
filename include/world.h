#ifndef WORLD_H
#define WORLD_H

#include <nusys.h>

#define MAX_X 96
#define MAX_Y 32
#define MAX_Z 96

#define CHUNK_SIZE 8

#define CHUNKS_X (MAX_X / CHUNK_SIZE)
#define CHUNKS_Y (MAX_Y / CHUNK_SIZE)
#define CHUNKS_Z (MAX_Z / CHUNK_SIZE)

#define NUM_BLOCKS (MAX_X * MAX_Y * MAX_Z)
#define NUM_CHUNKS (CHUNKS_X * CHUNKS_Y * CHUNKS_Z)

extern u8 blocks[NUM_BLOCKS];

void initWorld();
u8 tryPlantTree(u8 x, u8 y, u8 z);

#endif /* WORLD_H */
