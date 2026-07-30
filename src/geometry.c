#include <nusys.h>
#include "blocks.h"
#include "geometry.h"

#define KEEP 0
#define NOT_VISIBLE 1
#define OBSTRUCTED 2

/* The display lists retain their final quad commands, so retaining greedy
   meshes for the entire world only wastes RDRAM.  One column holds the four
   vertical chunks needed while its lists are compiled. */
ChunkQuads column_quads[CHUNKS_Y];

typedef struct {
  u8 lower;
  u8 upper;
  u8 start;
  u8 block;
} Front;

typedef struct {
  Front fronts[CHUNK_SIZE];
  u8 n_fronts;
  u8 lower;
  u8 block;
  u8 fi;
  u8 status;
} Scanline;

void setFront(Front *front, u8 lower, u8 upper, u8 start, u8 block) {
  front->lower = lower;
  front->upper = upper;
  front->start = start;
  front->block = block;
}

void insertFront(Scanline *scan, u8 i, u8 lower, u8 upper, u8 start, u8 block) {
    u8 j;
    if (scan->n_fronts >= CHUNK_SIZE) {
      return;
    }
    for (j = scan->n_fronts; j > i; j--) {
      scan->fronts[j] = scan->fronts[j - 1];
    }

    setFront(&scan->fronts[i], lower, upper, start, block);
    scan->n_fronts++;
}

void removeFront(Scanline *scan, u8 i) {
  u8 j;
  for (j = i; j < scan->n_fronts - 1; j++) {
    scan->fronts[j] = scan->fronts[j + 1];
  }

  scan->n_fronts--;
}

void appendQuad(QuadList *list, u8 bs, u8 bt, u8 width, u8 height, u8 block) {
  if (list->n >= CHUNK_SIZE * CHUNK_SIZE) {
    return;
  }
  list->quads[list->n].bs = bs;
  list->quads[list->n].bt = bt;
  list->quads[list->n].width = width - 1;
  list->quads[list->n].height = height - 1;
  list->quads[list->n].block = block;
  list->n++;
}

u8 blockAt(u8 r, u8 s, u8 t, u8 axes) {
  if (axes == ZXY) {
    return blocks[s * MAX_Y * MAX_Z + t * MAX_Z + r];
  } else if (axes == XZY) {
    return blocks[r * MAX_Y * MAX_Z + t * MAX_Z + s];
  } else if (axes == YXZ) {
    if (r >= MAX_Y) {
      return 0;
    }
    return blocks[s * MAX_Y * MAX_Z + r * MAX_Z + t];
  }
  return AIR;
}

void dropFront(Scanline *scan, QuadList *quads, u8 bs) {
  appendQuad(quads,
    scan->fronts[scan->fi].start,
    scan->fronts[scan->fi].lower,
    bs - scan->fronts[scan->fi].start,
    scan->fronts[scan->fi].upper - scan->fronts[scan->fi].lower,
    scan->fronts[scan->fi].block
  );
  removeFront(scan, scan->fi);
}

void droppingStep(Scanline *scan, QuadList *quads, u8 bs, u8 bt, u8 block, u8 cover_block) {
  if (scan->fi < scan->n_fronts && bt >= scan->fronts[scan->fi].upper) {
    if (scan->status != KEEP) {
      dropFront(scan, quads, bs);
    } else {
      scan->fi++;
    }
    scan->status = NOT_VISIBLE;
  }

  if (scan->fi < scan->n_fronts && bt >= scan->fronts[scan->fi].lower && scan->status != OBSTRUCTED && !cover_block) {
    if (block == scan->fronts[scan->fi].block) {
      scan->status = KEEP;
    } else {
      scan->status = OBSTRUCTED;
    }
  }
}

void droppingFinish(Scanline *scan, QuadList *quads, u8 bs) {
  if (scan->fi < scan->n_fronts && scan->status != KEEP) {
    dropFront(scan, quads, bs);
  }
}

void droppingPhase(Scanline *scan1, Scanline *scan2, QuadList *quads1, QuadList *quads2, u8 ct, u8 bs, u8 r, u8 s, u8 axes) {
  u8 bt, t;
  u8 block, block_f;

  scan1->fi = 0;
  scan1->status = NOT_VISIBLE;
  scan2->fi = 0;
  scan2->status = NOT_VISIBLE;

  for (bt = 0; bt < CHUNK_SIZE; bt++) {
    t = ct * CHUNK_SIZE + bt;

    block = blockAt(r, s, t, axes);
    block_f = blockAt(r + 1, s, t, axes);

    droppingStep(scan1, quads1, bs, bt, block, block_f);
    droppingStep(scan2, quads2, bs, bt, block_f, block);
  }

  droppingFinish(scan1, quads1, bs);
  droppingFinish(scan2, quads2, bs);
}

void endFront(Scanline *scan, u8 upper, u8 start) {
  if (scan-> lower != NONE) {
    insertFront(scan, scan->fi, scan->lower, upper, start, scan->block);
    scan->fi++;
    scan->lower = NONE;
  }
}

void creationStep(Scanline *scan, u8 bs, u8 bt, u8 block, u8 cover_block, u8 split_grass) {
  if (scan->fi < scan->n_fronts && bt >= scan->fronts[scan->fi].upper) {
    scan->fi++;
  }

  if (split_grass && scan->block == GRASS) {
    endFront(scan, bt, bs);
  }

  if (scan->fi < scan->n_fronts && bt >= scan->fronts[scan->fi].lower) {
    endFront(scan, bt, bs);
  } else {
    if (block && !cover_block) {
      if (block != scan->block) {
        endFront(scan, bt, bs);
      }

      if (scan->lower == NONE) {
        scan->lower = bt;
        scan->block = block;
      }
    } else {
      endFront(scan, bt, bs);
    }
  }
}

void creationFinish(Scanline *scan, u8 bs) {
  if (scan->lower != NONE) {
    insertFront(scan, scan->fi, scan->lower, CHUNK_SIZE, bs, scan->block);
    scan->lower = NONE;
  }
}

void creationPhase(Scanline *scan1, Scanline *scan2, u8 ct, u8 bs, u8 r, u8 s, u8 axes, u8 split_grass) {
  u8 bt, t;
  u8 block, block_f;

  scan1->fi = 0;
  scan1->lower = NONE;
  scan2->fi = 0;
  scan2->lower = NONE;

  for (bt = 0; bt < CHUNK_SIZE; bt++) {
    t = ct * CHUNK_SIZE + bt;

    block = blockAt(r, s, t, axes);
    block_f = blockAt(r + 1, s, t, axes);

    creationStep(scan1, bs, bt, block, block_f, split_grass);
    creationStep(scan2, bs, bt, block_f, block, split_grass);
  }

  creationFinish(scan1, bs);
  creationFinish(scan2, bs);
}

void appendAll(Scanline *scan, QuadList *quads) {
  for (scan->fi = 0; scan->fi < scan->n_fronts; scan->fi++) {
    appendQuad(quads,
      scan->fronts[scan->fi].start,
      scan->fronts[scan->fi].lower,
      CHUNK_SIZE - scan->fronts[scan->fi].start,
      scan->fronts[scan->fi].upper - scan->fronts[scan->fi].lower,
      scan->fronts[scan->fi].block
    );
  }
}

void makeChunkPlaneQuads(DualQuadList *axis_quads, u8 cr, u8 cs, u8 ct, u8 br, u8 max_r, u8 axes, u8 split_grass) {
  u8 bs, r, s, i;

  Scanline scan1, scan2;
  QuadList quads1, quads2;

  DualQuadList *both_quads;

  quads1.n = 0;
  quads2.n = 0;

  both_quads = &(axis_quads)[br];

  r = cr * CHUNK_SIZE + br;
  if (r >= max_r) {
    both_quads->n_front = 0;
    both_quads->n_back = 0;
  } else {
    scan1.n_fronts = 0;
    scan2.n_fronts = 0;
    
    for (bs = 0; bs < CHUNK_SIZE; bs++) {
      s = cs * CHUNK_SIZE + bs;
      droppingPhase(&scan1, &scan2, &quads1, &quads2, ct, bs, r, s, axes);
      creationPhase(&scan1, &scan2, ct, bs, r, s, axes, split_grass);
    }

    appendAll(&scan1, &quads1);
    appendAll(&scan2, &quads2);

    both_quads->n_front = quads1.n;
    both_quads->n_back = quads2.n;

    for (i = 0; i < quads1.n; i++) {
      both_quads->quads[i] = quads1.quads[i];
    }

    for (i = 0; i < quads2.n &&
        both_quads->n_front + i < CHUNK_SIZE * CHUNK_SIZE; i++) {
      both_quads->quads[both_quads->n_front + i] = quads2.quads[i];
    }
    both_quads->n_back = i;
  }
}

void makeChunkAxisQuads(DualQuadList *axis_quads, u8 cr, u8 cs, u8 ct, u8 max_r, u8 axes, u8 split_grass) {
  u8 br;
  for (br = 0; br < CHUNK_SIZE; br++) {
    makeChunkPlaneQuads(axis_quads, cr, cs, ct, br, max_r, axes, split_grass);
  }
}

void initGeometry() {
  /* Columns are generated just-in-time by makeColumnGeometry(). */
}

void makeColumnGeometry(u8 cx, u8 cz) {
  u8 cy;
  ChunkQuads *cq;
  for (cy = 0; cy < CHUNKS_Y; cy++) {
    cq = &column_quads[cy];
    makeChunkAxisQuads(cq->x_quads, cx, cz, cy, MAX_X - 1, XZY, TRUE);
    makeChunkAxisQuads(cq->y_quads, cy, cx, cz, MAX_Y,     YXZ, FALSE);
    makeChunkAxisQuads(cq->z_quads, cz, cx, cy, MAX_Z - 1, ZXY, TRUE);
  }
}
