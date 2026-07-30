#ifndef WORLD_H
#define WORLD_H

#include <nusys.h>

#define MAX_X 160
#define MAX_Y 32
#define MAX_Z 160

#define CHUNK_SIZE 8

#define CHUNKS_X (MAX_X / CHUNK_SIZE)
#define CHUNKS_Y (MAX_Y / CHUNK_SIZE)
#define CHUNKS_Z (MAX_Z / CHUNK_SIZE)

#define NUM_BLOCKS (MAX_X * MAX_Y * MAX_Z)
#define NUM_BLOCK_BYTES ((NUM_BLOCKS + 1) / 2)
#define NUM_CHUNKS (CHUNKS_X * CHUNKS_Y * CHUNKS_Z)

/*
 * The world's 16 block types fit in four bits.  Keeping that representation
 * live, rather than expanding it only while saving, doubles the explorable
 * footprint without spending another 400 KiB of the N64's RDRAM.
 */
extern u8 block_data[NUM_BLOCK_BYTES];

#define BLOCK_INDEX(x, y, z) \
  ((u32) (x) * MAX_Y * MAX_Z + (u32) (y) * MAX_Z + (u32) (z))

static __inline__ __attribute__((unused)) u8 blockGetIndex(u32 index) {
  u8 packed = block_data[index >> 1];
  return (index & 1) ? packed & 0x0F : packed >> 4;
}

static __inline__ __attribute__((unused)) void blockSetIndex(u32 index,
    u8 block) {
  u8 *packed = &block_data[index >> 1];
  if (index & 1) {
    *packed = (*packed & 0xF0) | (block & 0x0F);
  } else {
    *packed = (*packed & 0x0F) | (block << 4);
  }
}

#define blockGet(x, y, z) blockGetIndex(BLOCK_INDEX((x), (y), (z)))
#define blockSet(x, y, z, block) \
  blockSetIndex(BLOCK_INDEX((x), (y), (z)), (block))

void initWorld();
u8 tryPlantTree(u8 x, u8 y, u8 z);

#endif /* WORLD_H */
