#include "details.h"

#include "blocks.h"
#include "graphics.h"
#include "items.h"
#include "world.h"

DetailCell details[MAX_DETAILS];
u16 detail_count;
u16 detail_scan_limit;
u32 detail_overflows;

u8 detailKindIsTall(u8 kind) {
  return kind == DETAIL_WOOD_DOOR || kind == DETAIL_FENCE ||
    kind == DETAIL_FENCE_GATE;
}

u8 detailItemIsTall(u8 item) {
  return detailKindIsTall(detailKindForItem(item));
}

static u8 detailCovers(const DetailCell *detail, int x, int y, int z) {
  if (!detail->active || detail->x != x || detail->z != z) {
    return FALSE;
  }
  if (detail->y == y) {
    return TRUE;
  }
  return detailKindIsTall(detail->kind) && detail->y + 1 == y;
}

void initDetails(void) {
  u16 index;

  detail_count = 0;
  detail_scan_limit = 0;
  detail_overflows = 0;
  for (index = 0; index < MAX_DETAILS; index++) {
    details[index].active = FALSE;
  }
}

/*
 * A detail lookup must be answerable in roughly the cost of a block read.
 *
 * blockAt asks detailIsCustomAt about every cell of every plane of every
 * column the greedy mesher compiles -- tens of thousands of queries per
 * column, and a column is meshed again after every edit.  A linear walk of
 * the record pool there costs far more than building the mesh itself, and it
 * is paid in full even in a world that contains no details at all, which is
 * every world until the player places a torch.
 *
 * Two facts make the common answer free.  Every live record keeps
 * CRAFTING_TABLE in the terrain as its occupied proxy -- detailPlace writes
 * it and detailsApplyColumn restores it after a column regenerates -- so a
 * cell holding anything else cannot carry a record, and one nibble read
 * settles it.  Beyond that, records are handed out from the low end of the
 * pool, so the scan needs to reach only the high-water mark rather than the
 * 384-slot ceiling.
 */
DetailCell *detailAt(int x, int y, int z) {
  u16 index;

  if ((u32) y >= MAX_Y || detail_count == 0 ||
      blockGet(x, y, z) != CRAFTING_TABLE) {
    return NULL;
  }
  for (index = 0; index < detail_scan_limit; index++) {
    if (detailCovers(&details[index], x, y, z)) {
      return &details[index];
    }
  }
  return NULL;
}

u8 detailKindForItem(u8 item) {
  if (item == TORCH) return DETAIL_TORCH;
  if (item == WOOD_STAIRS) return DETAIL_WOOD_STAIRS;
  if (item == STONE_STAIRS) return DETAIL_STONE_STAIRS;
  if (item == WOOD_DOOR) return DETAIL_WOOD_DOOR;
  if (item == GLASS_WINDOW) return DETAIL_WINDOW;
  if (item == FENCE) return DETAIL_FENCE;
  if (item == FENCE_GATE) return DETAIL_FENCE_GATE;
  return DETAIL_NONE;
}

u8 detailItemForKind(u8 kind) {
  if (kind == DETAIL_TORCH) return TORCH;
  if (kind == DETAIL_WOOD_STAIRS) return WOOD_STAIRS;
  if (kind == DETAIL_STONE_STAIRS) return STONE_STAIRS;
  if (kind == DETAIL_WOOD_DOOR) return WOOD_DOOR;
  if (kind == DETAIL_WINDOW) return GLASS_WINDOW;
  if (kind == DETAIL_FENCE) return FENCE;
  if (kind == DETAIL_FENCE_GATE) return FENCE_GATE;
  return AIR;
}

/*
 * The fence post rooted exactly at (x, y, z), or NULL.
 *
 * A gate counts: a rail should run into one, which is what makes a gate read
 * as a way through a fence line rather than a door standing in a gap.
 *
 * The root has to match, not merely be covered.  detailAt answers for a tall
 * detail's upper cell too, so without the `detail->y != y` test a post one
 * step down the hill would link to its neighbour's head height and hang a
 * rail in the air.
 */
static DetailCell *fencePostAt(int x, int y, int z) {
  DetailCell *detail = detailAt(x, y, z);

  if (detail == NULL || detail->y != y) {
    return NULL;
  }
  return (detail->kind == DETAIL_FENCE ||
    detail->kind == DETAIL_FENCE_GATE) ? detail : NULL;
}

static const int fence_neighbours[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

static u8 fenceLinkMask(int x, int y, int z) {
  u8 links = 0;

  if (fencePostAt(x - 1, y, z) != NULL) links |= DETAIL_LINK_NEG_X;
  if (fencePostAt(x + 1, y, z) != NULL) links |= DETAIL_LINK_POS_X;
  if (fencePostAt(x, y, z - 1) != NULL) links |= DETAIL_LINK_NEG_Z;
  if (fencePostAt(x, y, z + 1) != NULL) links |= DETAIL_LINK_POS_Z;
  return links;
}

/* Recompute this cell's mask and all four neighbours', which is what stops a
   rail outliving the post at its far end.  Both edits that can change a fence
   line -- placing and breaking -- end here. */
static void fenceRefreshLinks(int x, int y, int z) {
  DetailCell *self = fencePostAt(x, y, z);
  u8 i;

  if (self != NULL) {
    self->links = fenceLinkMask(x, y, z);
  }
  for (i = 0; i < 4; i++) {
    DetailCell *neighbour = fencePostAt(x + fence_neighbours[i][0], y,
      z + fence_neighbours[i][1]);

    if (neighbour != NULL) {
      neighbour->links = fenceLinkMask(neighbour->x, y, neighbour->z);
    }
  }
}

static u8 detailHasSupport(int x, int y, int z) {
  if (y > 0 && worldCellSolid(x, y - 1, z)) return TRUE;
  if (worldCellSolid(x - 1, y, z)) return TRUE;
  if (worldCellSolid(x + 1, y, z)) return TRUE;
  if (worldCellSolid(x, y, z - 1)) return TRUE;
  return worldCellSolid(x, y, z + 1);
}

u8 detailPlace(u8 item, int x, int y, int z, u8 orientation, u8 flags) {
  DetailCell *detail = NULL;
  u8 kind = detailKindForItem(item);
  u16 index;

  if (kind == DETAIL_NONE || (u32) y >= MAX_Y || blockGet(x, y, z) != AIR ||
      !windowColumnResident(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT)) {
    return FALSE;
  }
  if (detailKindIsTall(kind) &&
      (y + 1 >= MAX_Y || blockGet(x, y + 1, z) != AIR)) {
    return FALSE;
  }
  if (!detailHasSupport(x, y, z)) {
    return FALSE;
  }
  for (index = 0; index < MAX_DETAILS; index++) {
    if (!details[index].active) {
      detail = &details[index];
      break;
    }
  }
  if (detail == NULL) {
    detail_overflows++;
    return FALSE;
  }
  if (index >= detail_scan_limit) {
    detail_scan_limit = index + 1;
  }

  detail->x = x;
  detail->z = z;
  detail->y = y;
  detail->kind = kind;
  /* A fence has no facing.  Its rails are named in absolute world directions
     by the link mask, so letting the player's heading rotate the model would
     swing them off their neighbours. */
  detail->orientation = kind == DETAIL_FENCE ? 0 : (orientation & 3);
  detail->state = 0;
  detail->flags = flags;
  detail->active = TRUE;
  detail->links = 0;
  detail->reserved = 0;
  detail_count++;
  blockSet(x, y, z, CRAFTING_TABLE);
  if (detailKindIsTall(kind)) {
    blockSet(x, y + 1, z, CRAFTING_TABLE);
  }
  /* After the proxy is written, so this post can see itself. */
  fenceRefreshLinks(x, y, z);
  return TRUE;
}

u8 detailRemove(int x, int y, int z, u8 *drop_item) {
  DetailCell *detail = detailAt(x, y, z);
  int root_x;
  int root_y;
  int root_z;

  if (detail == NULL) {
    return FALSE;
  }
  /* Breaking a tall detail's upper cell removes the whole record, so the
     coordinates to rebuild links around are its root's, not the ones the
     player was looking at. */
  root_x = detail->x;
  root_y = detail->y;
  root_z = detail->z;
  if (drop_item != NULL) {
    *drop_item = detailItemForKind(detail->kind);
  }
  if (blockGet(root_x, root_y, root_z) == CRAFTING_TABLE) {
    blockSet(root_x, root_y, root_z, AIR);
  }
  if (detailKindIsTall(detail->kind) && root_y + 1 < MAX_Y &&
      blockGet(root_x, root_y + 1, root_z) == CRAFTING_TABLE) {
    blockSet(root_x, root_y + 1, root_z, AIR);
  }
  detail->active = FALSE;
  if (detail_count > 0) {
    detail_count--;
  }
  /* After deactivation, so the neighbours re-survey a gap rather than the
     post that is on its way out. */
  fenceRefreshLinks(root_x, root_y, root_z);
  return TRUE;
}

u8 detailToggle(int x, int y, int z) {
  DetailCell *detail = detailAt(x, y, z);

  if (detail == NULL || (detail->kind != DETAIL_WOOD_DOOR &&
      detail->kind != DETAIL_FENCE_GATE)) {
    return FALSE;
  }
  detail->state ^= DETAIL_STATE_OPEN;
  return TRUE;
}

u8 detailIsCustomAt(int x, int y, int z) {
  return detailAt(x, y, z) != NULL;
}

u8 detailIsStairAt(int x, int y, int z) {
  DetailCell *detail = detailAt(x, y, z);
  return detail != NULL && (detail->kind == DETAIL_WOOD_STAIRS ||
    detail->kind == DETAIL_STONE_STAIRS);
}

u8 worldCellSolid(int x, int y, int z) {
  u8 block;
  DetailCell *detail;

  if (y < 0) {
    return TRUE;
  }
  if (y >= MAX_Y) {
    return FALSE;
  }
  block = blockGet(x, y, z);
  if (block != CRAFTING_TABLE) {
    return BLOCK_IS_SOLID(block);
  }
  detail = detailAt(x, y, z);
  if (detail == NULL) {
    return TRUE;
  }
  if (detail->kind == DETAIL_TORCH) {
    return FALSE;
  }
  /* Both hinged kinds stand open the same way.  A gate's upper cell opens
     with it, which is what lets a player walk through a fence line without
     the fence's own head-height block stopping them. */
  if ((detail->kind == DETAIL_WOOD_DOOR ||
       detail->kind == DETAIL_FENCE_GATE) &&
      (detail->state & DETAIL_STATE_OPEN)) {
    return FALSE;
  }
  return TRUE;
}

u8 detailLightAt(Vector3 position) {
  int px = floor(position.x / BLOCK_SIZE);
  int py = floor(position.y / BLOCK_SIZE);
  int pz = floor(position.z / BLOCK_SIZE);
  u16 index;
  int strongest = 0;

  for (index = 0; index < detail_scan_limit; index++) {
    DetailCell *detail = &details[index];
    int dx, dy, dz, distance;
    int light;

    if (!detail->active || detail->kind != DETAIL_TORCH) {
      continue;
    }
    dx = detail->x - px;
    dy = detail->y - py;
    dz = detail->z - pz;
    distance = dx * dx + dy * dy + dz * dz;
    if (distance > 64) {
      continue;
    }
    light = 224 - distance * 3;
    if (light > strongest) {
      strongest = light;
    }
  }
  return (u8) strongest;
}

void detailsApplyColumn(int cx, int cz) {
  u16 index;

  if (detail_count == 0 || !windowColumnResident(cx, cz)) {
    return;
  }
  for (index = 0; index < detail_scan_limit; index++) {
    DetailCell *detail = &details[index];
    if (!detail->active || (detail->x >> CHUNK_SHIFT) != cx ||
        (detail->z >> CHUNK_SHIFT) != cz) {
      continue;
    }
    blockSet(detail->x, detail->y, detail->z, CRAFTING_TABLE);
    if (detailKindIsTall(detail->kind) && detail->y + 1 < MAX_Y) {
      blockSet(detail->x, detail->y + 1, detail->z, CRAFTING_TABLE);
    }
  }
}

/* Called on every window claim and release, so it must cost nothing in the
   overwhelmingly common case of a world with no generated details. */
void detailsEvictGeneratedColumn(int cx, int cz) {
  u16 index;

  for (index = 0; index < detail_scan_limit; index++) {
    DetailCell *detail = &details[index];
    if (detail->active && (detail->flags & DETAIL_FLAG_GENERATED) &&
        (detail->x >> CHUNK_SHIFT) == cx &&
        (detail->z >> CHUNK_SHIFT) == cz) {
      detail->active = FALSE;
      if (detail_count > 0) {
        detail_count--;
      }
    }
  }
}
