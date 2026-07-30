#ifndef GEOMETRY_H
#define GEOMETRY_H

#include <nusys.h>
#include "world.h"

#define ZXY 0
#define XZY 1
#define YXZ 2

#define FRONT  0
#define LEFT   1
#define BACK   2
#define RIGHT  3
#define TOP    4
#define BOTTOM 5

#define NONE 255
#define ALL  254

typedef struct {
  u16 bs:3;
  u16 bt:3;
  u16 width:3;
  u16 height:3;
  u16 block:4;
} Quad;

typedef struct {
  u8 n;
  Quad quads[CHUNK_SIZE * CHUNK_SIZE];
} QuadList;

typedef struct {
  u8 n_front;
  u8 n_back;
  Quad quads[CHUNK_SIZE * CHUNK_SIZE];
} DualQuadList;

typedef struct {
  DualQuadList x_quads[CHUNK_SIZE];
  DualQuadList y_quads[CHUNK_SIZE];
  DualQuadList z_quads[CHUNK_SIZE];
} ChunkQuads;

/* Geometry is generated into one reusable vertical-column scratch buffer
   immediately before that column's display lists are compiled. */
extern ChunkQuads column_quads[CHUNKS_Y];
/* The scenic preview mesh does not need seam refinement; disabling it removes
   the dominant cost of compiling a world. */
void geometrySetTjunctionRefinement(u8 enabled);
/* Planes that hit the refinement cap.  Large values mean terrain is
   fragmented enough that the cap is doing real work. */
extern u32 tjunction_refinement_caps;

void initGeometry();
void makeColumnGeometry(u8 cx, u8 cz);

#endif /* GEOMETRY_H */
