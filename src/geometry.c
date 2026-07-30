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

/*
 * T-junction refinement restarts a full O(n^2) pairwise scan after every
 * split, and every split adds another quad, so the work is O(n^3) in the
 * quads on a plane.  A fragmented plane can therefore stall inside a single
 * column for seconds -- indistinguishable from a hang, and dependent on world
 * content, which is why it only bites some worlds.
 *
 * Cap the refinements instead.  A skipped one leaves a hairline seam between
 * two quads; an uncapped one stops the console.
 */
#define MAX_TJUNCTION_REFINEMENTS 8

static u8 refine_tjunctions = TRUE;
void geometrySetTjunctionRefinement(u8 enabled) {
  refine_tjunctions = enabled;
}

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

static u8 blocksOcclude(u8 block, u8 cover_block) {
  /* Water is passable.  A solid face bordering it must remain in the mesh so
   * the player cannot see through a shoreline or cave wall while swimming. */
  return cover_block != AIR && !(block != WATER && cover_block == WATER);
}

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
    return blockGet(s, t, r);
  } else if (axes == XZY) {
    return blockGet(r, t, s);
  } else if (axes == YXZ) {
    if (r >= MAX_Y) {
      return 0;
    }
    return blockGet(s, r, t);
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

  if (scan->fi < scan->n_fronts && bt >= scan->fronts[scan->fi].lower &&
      scan->status != OBSTRUCTED && !blocksOcclude(block, cover_block)) {
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
    if (block && !blocksOcclude(block, cover_block)) {
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

static u8 splitQuadAt(QuadList *list, u8 index, u8 cut, u8 vertical,
    u8 total_quads) {
  Quad original = list->quads[index];
  Quad *first = &list->quads[index];
  u8 right = original.bs + original.width + 1;
  u8 upper = original.bt + original.height + 1;

  /* A plane contains at most one exposed face per 8x8 cell, so a fully
     refined pair of front/back lists still fits the shared 64-quad buffer. */
  if (list->n >= CHUNK_SIZE * CHUNK_SIZE ||
      total_quads >= CHUNK_SIZE * CHUNK_SIZE) {
    return FALSE;
  }

  if (vertical) {
    if (cut <= original.bs || cut >= right) {
      return FALSE;
    }
    first->width = cut - original.bs - 1;
    appendQuad(list, cut, original.bt, right - cut,
      original.height + 1, original.block);
  } else {
    if (cut <= original.bt || cut >= upper) {
      return FALSE;
    }
    first->height = cut - original.bt - 1;
    appendQuad(list, original.bs, cut, original.width + 1,
      upper - cut, original.block);
  }
  return TRUE;
}

static u8 splitOneTjunction(QuadList *front, QuadList *back) {
  QuadList *lists[2] = {front, back};
  u8 ai, al, bi, bl;

  for (al = 0; al < 2; al++) {
    for (ai = 0; ai < lists[al]->n; ai++) {
      Quad a = lists[al]->quads[ai];
      u8 a_left = a.bs;
      u8 a_right = a.bs + a.width + 1;
      u8 a_lower = a.bt;
      u8 a_upper = a.bt + a.height + 1;

      for (bl = 0; bl < 2; bl++) {
        for (bi = 0; bi < lists[bl]->n; bi++) {
          Quad b;
          u8 b_left, b_right, b_lower, b_upper;

          if (al == bl && ai == bi) {
            continue;
          }
          b = lists[bl]->quads[bi];
          b_left = b.bs;
          b_right = b.bs + b.width + 1;
          b_lower = b.bt;
          b_upper = b.bt + b.height + 1;

          /* If a neighbouring rectangle ends midway along A's horizontal
             edge, split A vertically so both primitives use that endpoint. */
          if (a_lower == b_upper || a_upper == b_lower) {
            if (b_left > a_left && b_left < a_right &&
                splitQuadAt(lists[al], ai, b_left, TRUE,
                  front->n + back->n)) {
              return TRUE;
            }
            if (b_right > a_left && b_right < a_right &&
                splitQuadAt(lists[al], ai, b_right, TRUE,
                  front->n + back->n)) {
              return TRUE;
            }
          }

          /* Do the corresponding refinement along vertical shared edges. */
          if (a_left == b_right || a_right == b_left) {
            if (b_lower > a_lower && b_lower < a_upper &&
                splitQuadAt(lists[al], ai, b_lower, FALSE,
                  front->n + back->n)) {
              return TRUE;
            }
            if (b_upper > a_lower && b_upper < a_upper &&
                splitQuadAt(lists[al], ai, b_upper, FALSE,
                  front->n + back->n)) {
              return TRUE;
            }
          }
        }
      }
    }
  }
  return FALSE;
}

static void splitTjunctions(QuadList *front, QuadList *back) {
  u8 refinements = 0;

  if (!refine_tjunctions) {
    return;
  }
  /* Splitting can introduce a new endpoint on the opposite edge, so restart
     the search after each refinement -- but stop well before the quad budget
     makes that quadratic rescan the dominant cost of the whole world. */
  while (refinements < MAX_TJUNCTION_REFINEMENTS &&
      splitOneTjunction(front, back)) {
    refinements++;
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
    splitTjunctions(&quads1, &quads2);

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
