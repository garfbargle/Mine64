#include "edits.h"

#include "blocks.h"
#include "world.h"

WorldEdit world_edits[MAX_WORLD_EDITS];
u16 world_edit_count;
u32 world_edit_overflows;

static WorldEdit *findEdit(int x, int y, int z) {
  u16 index;

  for (index = 0; index < MAX_WORLD_EDITS; index++) {
    WorldEdit *edit = &world_edits[index];
    if (edit->active && edit->x == x && edit->y == y && edit->z == z) {
      return edit;
    }
  }
  return NULL;
}

void initWorldEdits(void) {
  u16 index;

  world_edit_count = 0;
  world_edit_overflows = 0;
  for (index = 0; index < MAX_WORLD_EDITS; index++) {
    world_edits[index].active = FALSE;
  }
}

u8 worldEditCanSet(int x, int y, int z) {
  u16 index;

  if ((u32) y >= MAX_Y ||
      !windowColumnResident(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT)) {
    return FALSE;
  }
  if (findEdit(x, y, z) != NULL) {
    return TRUE;
  }
  for (index = 0; index < MAX_WORLD_EDITS; index++) {
    if (!world_edits[index].active) {
      return TRUE;
    }
  }
  return FALSE;
}

u8 worldEditSet(int x, int y, int z, u8 block) {
  WorldEdit *edit;
  u16 index;

  if (!BLOCK_IS_VALID(block) || !worldEditCanSet(x, y, z)) {
    world_edit_overflows++;
    return FALSE;
  }
  edit = findEdit(x, y, z);
  if (edit == NULL) {
    for (index = 0; index < MAX_WORLD_EDITS; index++) {
      if (!world_edits[index].active) {
        edit = &world_edits[index];
        break;
      }
    }
    if (edit == NULL) {
      world_edit_overflows++;
      return FALSE;
    }
    edit->x = x;
    edit->z = z;
    edit->y = y;
    edit->active = TRUE;
    edit->reserved = 0;
    world_edit_count++;
  }
  edit->block = block;
  blockSet(x, y, z, block);
  return TRUE;
}

void worldApplyEditsToColumn(int cx, int cz) {
  u16 index;

  if (!windowColumnResident(cx, cz)) {
    return;
  }
  for (index = 0; index < MAX_WORLD_EDITS; index++) {
    WorldEdit *edit = &world_edits[index];
    if (edit->active && (edit->x >> CHUNK_SHIFT) == cx &&
        (edit->z >> CHUNK_SHIFT) == cz) {
      blockSet(edit->x, edit->y, edit->z, edit->block);
    }
  }
}
