/* math.c pulls graphics.h in for BLOCK_SIZE alone.  The real header declares
   Gfx display lists, which mean nothing on a host. */
#ifndef GENTEST_GRAPHICS_H
#define GENTEST_GRAPHICS_H

#define BLOCK_SIZE 64

#endif /* GENTEST_GRAPHICS_H */
