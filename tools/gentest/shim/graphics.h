/* math.c pulls graphics.h in for BLOCK_SIZE alone.  The real header declares
   Gfx display lists, which mean nothing on a host. */
#ifndef GENTEST_GRAPHICS_H
#define GENTEST_GRAPHICS_H

#define BLOCK_SIZE 64

/* world.c calls these when the window rebinds or finishes a column.  They only
   affect display lists, which this harness has none of. */
void graphicsInvalidateColumnSlot(unsigned int slot);
void graphicsMarkColumnDirty(int cx, int cz);
unsigned char graphicsColumnNeedsMesh(int cx, int cz);
void graphicsSetRenderOrigin(int block_x, int block_z);

#endif /* GENTEST_GRAPHICS_H */
