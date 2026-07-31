#include "details.h"

#include "blocks.h"
#include "graphics.h"
#include "items.h"
#include "world.h"

DetailCell details[MAX_DETAILS];
u16 detail_count;
u32 detail_overflows;

static u8 detailCovers(const DetailCell *detail, int x, int y, int z) {
  if (!detail->active || detail->x != x || detail->z != z) {
    return FALSE;
  }
  if (detail->y == y) {
    return TRUE;
  }
  return detail->kind == DETAIL_WOOD_DOOR && detail->y + 1 == y;
}

void initDetails(void) {
  u16 index;

  detail_count = 0;
  detail_overflows = 0;
  for (index = 0; index < MAX_DETAILS; index++) {
    details[index].active = FALSE;
  }
}

DetailCell *detailAt(int x, int y, int z) {
  u16 index;

  if ((u32) y >= MAX_Y) {
    return NULL;
  }
  for (index = 0; index < MAX_DETAILS; index++) {
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
  return DETAIL_NONE;
}

u8 detailItemForKind(u8 kind) {
  if (kind == DETAIL_TORCH) return TORCH;
  if (kind == DETAIL_WOOD_STAIRS) return WOOD_STAIRS;
  if (kind == DETAIL_STONE_STAIRS) return STONE_STAIRS;
  if (kind == DETAIL_WOOD_DOOR) return WOOD_DOOR;
  if (kind == DETAIL_WINDOW) return GLASS_WINDOW;
  return AIR;
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
  if (kind == DETAIL_WOOD_DOOR &&
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

  detail->x = x;
  detail->z = z;
  detail->y = y;
  detail->kind = kind;
  detail->orientation = orientation & 3;
  detail->state = 0;
  detail->flags = flags;
  detail->active = TRUE;
  detail_count++;
  blockSet(x, y, z, CRAFTING_TABLE);
  if (kind == DETAIL_WOOD_DOOR) {
    blockSet(x, y + 1, z, CRAFTING_TABLE);
  }
  return TRUE;
}

u8 detailRemove(int x, int y, int z, u8 *drop_item) {
  DetailCell *detail = detailAt(x, y, z);

  if (detail == NULL) {
    return FALSE;
  }
  if (drop_item != NULL) {
    *drop_item = detailItemForKind(detail->kind);
  }
  if (blockGet(detail->x, detail->y, detail->z) == CRAFTING_TABLE) {
    blockSet(detail->x, detail->y, detail->z, AIR);
  }
  if (detail->kind == DETAIL_WOOD_DOOR && detail->y + 1 < MAX_Y &&
      blockGet(detail->x, detail->y + 1, detail->z) == CRAFTING_TABLE) {
    blockSet(detail->x, detail->y + 1, detail->z, AIR);
  }
  detail->active = FALSE;
  if (detail_count > 0) {
    detail_count--;
  }
  return TRUE;
}

u8 detailToggle(int x, int y, int z) {
  DetailCell *detail = detailAt(x, y, z);

  if (detail == NULL || detail->kind != DETAIL_WOOD_DOOR) {
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
  if (detail->kind == DETAIL_WOOD_DOOR &&
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

  for (index = 0; index < MAX_DETAILS; index++) {
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

  if (!windowColumnResident(cx, cz)) {
    return;
  }
  for (index = 0; index < MAX_DETAILS; index++) {
    DetailCell *detail = &details[index];
    if (!detail->active || (detail->x >> CHUNK_SHIFT) != cx ||
        (detail->z >> CHUNK_SHIFT) != cz) {
      continue;
    }
    blockSet(detail->x, detail->y, detail->z, CRAFTING_TABLE);
    if (detail->kind == DETAIL_WOOD_DOOR && detail->y + 1 < MAX_Y) {
      blockSet(detail->x, detail->y + 1, detail->z, CRAFTING_TABLE);
    }
  }
}

void detailsEvictGeneratedColumn(int cx, int cz) {
  u16 index;

  for (index = 0; index < MAX_DETAILS; index++) {
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
