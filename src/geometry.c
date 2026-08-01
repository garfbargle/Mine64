#include <nusys.h>
#include "blocks.h"
#include "geometry.h"
#include "details.h"

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

/*
 * One column's blocks, unpacked once per mesh compile.
 *
 * The greedy mesher samples every cell of every plane from three axes, and
 * the dropping and creation phases each sample the pair again -- roughly
 * 24,000 blockAt calls per column against its 2,048 blocks.  Every one of
 * those calls repeated the window-key residency check, the nibble unpack,
 * and (once any detail record existed) a second full blockGet inside the
 * detail probe.  A mesh compile is the single largest unit of work the
 * gameplay callback pays, and it is paid for every column a chunk crossing
 * streams in, promotes, demotes, or re-seams -- so the redundancy here was
 * most of the cost of walking.
 *
 * Resolve each cell exactly once instead: the column's own 8x32x8, plus the
 * one-block halo the phases' r + 1 samples read across the +x and +z
 * boundaries, with the detail override already applied.  The y == MAX_Y
 * plane stays AIR, which is the answer the YXZ axis's old r >= MAX_Y
 * special case gave.  ~2,600 resolved cells replace ~48,000 block reads.
 */
static u8 mesh_cache[CHUNK_SIZE + 1][MAX_Y + 1][CHUNK_SIZE + 1];
static int mesh_cache_base_x;
static int mesh_cache_base_z;

/*
 * The sampling rule applied to every cell: a detail proxy meshes as AIR (its
 * box is drawn by the detail pass), everything else is the terrain nibble --
 * including BLOCK_NOT_RESIDENT, which the scanline logic needs to tell "no
 * block" from "face against unloaded terrain".  The CRAFTING_TABLE gate keeps
 * the record-pool scan off the overwhelmingly common cell, exactly as
 * detailAt's own fast path does.
 *
 * Filled a vertical run at a time rather than by per-cell blockGet.  All
 * 2,592 cells live in at most four window columns, so the residency check is
 * paid 81 times (once per x/z pair) instead of per cell -- and within one
 * pair, BLOCK_LOCAL_INDEX steps by exactly CHUNK_SIZE per block of height,
 * so the packed bytes are a stride-4 walk whose nibble half never changes.
 */
static void fillMeshCache(int cx, int cz) {
  int bx, y, bz;

  mesh_cache_base_x = cx * CHUNK_SIZE;
  mesh_cache_base_z = cz * CHUNK_SIZE;
  for (bx = 0; bx <= CHUNK_SIZE; bx++) {
    int world_x = mesh_cache_base_x + bx;

    for (bz = 0; bz <= CHUNK_SIZE; bz++) {
      int world_z = mesh_cache_base_z + bz;
      /* The bx/bz == CHUNK_SIZE halo rows cross into the +x/+z neighbours;
         every other cell is the compiled column itself. */
      const u8 *column = columnSlotData(world_x >> CHUNK_SHIFT,
        world_z >> CHUNK_SHIFT);
      u8 *out = &mesh_cache[bx][0][bz];

      if (column == (u8 *) 0) {
        for (y = 0; y < MAX_Y; y++) {
          *out = BLOCK_NOT_RESIDENT;
          out += CHUNK_SIZE + 1;
        }
      } else {
        const u8 *packed =
          column + (BLOCK_LOCAL_INDEX(world_x, 0, world_z) >> 1);
        u8 low_nibble = (u8) (world_z & 1);

        for (y = 0; y < MAX_Y; y++) {
          u8 block = low_nibble ? (u8) (*packed & 0x0F) : (u8) (*packed >> 4);

          if (block == CRAFTING_TABLE && detailIsCustomAt(world_x, y,
              world_z)) {
            block = AIR;
          }
          *out = block;
          out += CHUNK_SIZE + 1;
          packed += CHUNK_SIZE / 2;
        }
      }
      /* The y == MAX_Y plane stays AIR: the answer the YXZ axis's r + 1
         boundary sample always read. */
      mesh_cache[bx][MAX_Y][bz] = AIR;
    }
  }
}

/*
 * World coordinates, not block-local ones: r, s and t are int because two of
 * them are world x/z in some permutation, and a streaming world walks both
 * negative and past 255 -- the u8 these used to be silently wrapped there,
 * which meshed every column outside blocks 0..255 as empty.
 *
 * Only valid between makeColumnGeometry filling the cache and the end of
 * that column's compile: every caller is one of the two scanline phases,
 * whose coordinates stay inside the cached column and its +1 halo.
 */
u8 blockAt(int r, int s, int t, u8 axes) {
  if (axes == ZXY) {
    return mesh_cache[s - mesh_cache_base_x][t][r - mesh_cache_base_z];
  } else if (axes == XZY) {
    return mesh_cache[r - mesh_cache_base_x][t][s - mesh_cache_base_z];
  }
  /* YXZ: r is y and reaches MAX_Y through the r + 1 sample; the cache's
     top plane holds the AIR that boundary always read. */
  return mesh_cache[s - mesh_cache_base_x][r][t - mesh_cache_base_z];
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

void droppingPhase(Scanline *scan1, Scanline *scan2, QuadList *quads1, QuadList *quads2, int ct, u8 bs, int r, int s, u8 axes) {
  u8 bt;
  int t;
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
    /* A non-resident sample is not a block: it must never grow a face of its
       own, or the edge of loaded terrain sprouts phantom walls whose 0xFF
       type truncates to whatever block the 4-bit quad field makes of it.
       (Its role as an occluder is handled in blocksOcclude's caller: faces
       *against* unloaded terrain stay hidden until the neighbour arrives and
       the column is remeshed.) */
    if (block && block != BLOCK_NOT_RESIDENT &&
        !blocksOcclude(block, cover_block)) {
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

void creationPhase(Scanline *scan1, Scanline *scan2, int ct, u8 bs, int r, int s, u8 axes, u8 split_grass) {
  u8 bt;
  int t;
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

void makeChunkPlaneQuads(DualQuadList *axis_quads, int cr, int cs, int ct, u8 br, u8 axes, u8 split_grass) {
  u8 bs, i;
  int r, s;

  Scanline scan1, scan2;
  QuadList quads1, quads2;

  DualQuadList *both_quads;

  quads1.n = 0;
  quads2.n = 0;

  both_quads = &(axis_quads)[br];

  /*
   * No world-edge clamp any more: the world has no x/z edge.  The fixed
   * extent's boundary planes come out identical anyway -- the sample across
   * the boundary reads BLOCK_NOT_RESIDENT, which occludes the resident side's
   * face and is barred from growing one of its own, so the plane stays empty
   * exactly as the old `r >= max_r` skip left it.
   */
  r = cr * CHUNK_SIZE + br;
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

void makeChunkAxisQuads(DualQuadList *axis_quads, int cr, int cs, int ct, u8 axes, u8 split_grass) {
  u8 br;
  for (br = 0; br < CHUNK_SIZE; br++) {
    makeChunkPlaneQuads(axis_quads, cr, cs, ct, br, axes, split_grass);
  }
}

void initGeometry() {
  /* Columns are generated just-in-time by makeColumnGeometry(). */
}

void makeColumnGeometry(int cx, int cz) {
  u8 cy;
  ChunkQuads *cq;

  fillMeshCache(cx, cz);
  for (cy = 0; cy < CHUNKS_Y; cy++) {
    cq = &column_quads[cy];
    makeChunkAxisQuads(cq->x_quads, cx, cz, cy, XZY, TRUE);
    makeChunkAxisQuads(cq->y_quads, cy, cx, cz, YXZ, FALSE);
    makeChunkAxisQuads(cq->z_quads, cz, cx, cy, ZXY, TRUE);
  }
}
