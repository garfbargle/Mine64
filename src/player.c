#include <nusys.h>
#include "player.h"
#include "math.h"
#include "menu.h"
#include "blocks.h"
#include "graphics.h"
#include "camera.h"
#include "geometry.h"
#include "items.h"
#include "storage.h"

#define START_X 32
#define START_Z 48

#define STICK_DAMPER 22
#define MOVE_SPEED (1 / 8.f)
#define SPRINT_MULTIPLIER 1.5f
#define JUMP_SPEED (BLOCK_SIZE / 4.5)
#define TERMINAL_SPEED (BLOCK_SIZE / 2)
#define GRAVITY (BLOCK_SIZE / 40)
#define MAX_FRAME_DELTA 2.5f
#define BOX_RADIUS 0.35
#define BOX_HEIGHT 1.8
#define EYE_HEIGHT 1.5
#define INVENTORY_STICK_THRESHOLD 38
#define INVENTORY_REPEAT_DELAY 12
#define INVENTORY_REPEAT_RATE 4

#define NAV_LEFT  0x01
#define NAV_RIGHT 0x02
#define NAV_UP    0x04
#define NAV_DOWN  0x08

Player players[MAX_PLAYERS];
u8 active_player_count = 1;
u8 inventory_player = 0;

static Vector3 bounding_box[] = {
  {-BOX_RADIUS, -EYE_HEIGHT, -BOX_RADIUS},
  {-BOX_RADIUS, -EYE_HEIGHT,  BOX_RADIUS},
  {-BOX_RADIUS, BOX_HEIGHT - EYE_HEIGHT, -BOX_RADIUS},
  {-BOX_RADIUS, BOX_HEIGHT - EYE_HEIGHT,  BOX_RADIUS},
  { BOX_RADIUS, -EYE_HEIGHT, -BOX_RADIUS},
  { BOX_RADIUS, -EYE_HEIGHT,  BOX_RADIUS},
  { BOX_RADIUS, BOX_HEIGHT - EYE_HEIGHT, -BOX_RADIUS},
  { BOX_RADIUS, BOX_HEIGHT - EYE_HEIGHT,  BOX_RADIUS}
};

static NUContData cont_data[MAX_PLAYERS];
static OSTime last_time = 0;
static u16 down_held = FALSE;
static u16 up_held = FALSE;
static u16 act_held = FALSE;
static u8 block_dec_held[MAX_PLAYERS];
static u8 block_inc_held[MAX_PLAYERS];
static u8 inventory_nav_previous;
static u8 inventory_nav_repeat;

static void resetInventoryNavigation(void) {
  inventory_nav_previous = 0;
  inventory_nav_repeat = 0;
}

static u8 inventoryNavigation(NUContData *cont) {
  u8 held = 0;
  u8 pressed;

  if ((cont->button & (L_CBUTTONS | L_JPAD)) ||
      cont->stick_x < -INVENTORY_STICK_THRESHOLD) {
    held |= NAV_LEFT;
  }
  if ((cont->button & (R_CBUTTONS | R_JPAD)) ||
      cont->stick_x > INVENTORY_STICK_THRESHOLD) {
    held |= NAV_RIGHT;
  }
  if ((cont->button & (U_CBUTTONS | U_JPAD)) ||
      cont->stick_y > INVENTORY_STICK_THRESHOLD) {
    held |= NAV_UP;
  }
  if ((cont->button & (D_CBUTTONS | D_JPAD)) ||
      cont->stick_y < -INVENTORY_STICK_THRESHOLD) {
    held |= NAV_DOWN;
  }

  pressed = held & ~inventory_nav_previous;
  if (pressed) {
    inventory_nav_repeat = INVENTORY_REPEAT_DELAY;
  } else if (held) {
    if (inventory_nav_repeat > 0) {
      inventory_nav_repeat--;
    }
    if (inventory_nav_repeat == 0) {
      pressed = held;
      inventory_nav_repeat = INVENTORY_REPEAT_RATE;
    }
  } else {
    inventory_nav_repeat = 0;
  }
  inventory_nav_previous = held;
  return pressed;
}

void resetPlayerInventory(Player *player) {
  u8 slot;
  int selected_slot = player->held_block - FIRST_PLACEABLE_BLOCK;

  if (selected_slot < 0 || selected_slot >= INVENTORY_COLUMNS) {
    selected_slot = COBBLESTONE - FIRST_PLACEABLE_BLOCK;
  }

  for (slot = 0; slot < INVENTORY_SIZE; slot++) {
    player->inventory[slot].item = AIR;
    player->inventory[slot].count = 0;
  }
  for (slot = 0; slot < CRAFTING_SIZE; slot++) {
    player->crafting[slot].item = AIR;
    player->crafting[slot].count = 0;
  }
  player->carried_item.item = AIR;
  player->carried_item.count = 0;
  for (slot = 0; slot < INVENTORY_COLUMNS; slot++) {
    player->inventory[INVENTORY_HOTBAR_START + slot].item = slot + FIRST_PLACEABLE_BLOCK;
    /* New worlds start with an empty hotbar.  Every placeable block is now
       obtained by mining or crafting it first. */
    player->inventory[INVENTORY_HOTBAR_START + slot].count = 0;
  }

  player->selected_hotbar_slot = selected_slot;
  player->inventory_cursor = INVENTORY_HOTBAR_START + selected_slot;
  player->crafting_cursor = 0;
  player->inventory_area = INVENTORY_AREA_ITEMS;
  player->crafting_table_open = FALSE;
  player->held_block = player->inventory[player->inventory_cursor].item;
}

static void selectHotbarSlot(Player *player, u8 slot) {
  player->selected_hotbar_slot = slot;
  player->inventory_cursor = INVENTORY_HOTBAR_START + slot;
  player->held_block = player->inventory[player->inventory_cursor].item;
}

u8 addItemToInventory(Player *player, u8 item, u8 count) {
  u8 slot;
  u8 remaining = count;
  u8 max_stack = itemMaxStack(item);

  for (slot = 0; slot < INVENTORY_SIZE && remaining > 0; slot++) {
    ItemStack *stack = &player->inventory[slot];
    u8 added;
    if (stack->item != item || stack->count >= max_stack) {
      continue;
    }
    added = remaining < max_stack - stack->count ? remaining : max_stack - stack->count;
    stack->count += added;
    remaining -= added;
  }
  for (slot = 0; slot < INVENTORY_SIZE && remaining > 0; slot++) {
    ItemStack *stack = &player->inventory[slot];
    u8 added;
    if (stack->count != 0) {
      continue;
    }
    added = remaining < max_stack ? remaining : max_stack;
    stack->item = item;
    stack->count = added;
    remaining -= added;
  }
  return count - remaining;
}

static u8 recipeSlotIs(Player *player, u8 slot, u8 item) {
  return player->crafting[slot].item == item && player->crafting[slot].count > 0;
}

static u8 craftingColumns(Player *player) {
  return player->crafting_table_open ? CRAFTING_TABLE_COLUMNS : PLAYER_CRAFTING_COLUMNS;
}

static u8 craftingRows(Player *player) {
  return player->crafting_table_open ? CRAFTING_TABLE_ROWS : PLAYER_CRAFTING_ROWS;
}

static u8 craftingSlotVisible(Player *player, u8 slot) {
  u8 row = slot / CRAFTING_TABLE_COLUMNS;
  u8 column = slot % CRAFTING_TABLE_COLUMNS;
  return row < craftingRows(player) && column < craftingColumns(player);
}

static u8 recipeHasOnly(Player *player, u16 used_slots) {
  u8 slot;
  for (slot = 0; slot < CRAFTING_SIZE; slot++) {
    if (player->crafting[slot].count > 0 && !(used_slots & (1 << slot))) {
      return FALSE;
    }
  }
  return TRUE;
}

/* Returns both the visible result and the ingredient slots used by one craft.
   Recipes follow their familiar 3x3 layouts, including the vertical sword
   and the three-wide pickaxe head. */
static u8 getCraftRecipe(Player *player, ItemStack *result, u16 *used_slots) {
  u8 slot, column;

  for (slot = 0; slot < CRAFTING_SIZE; slot++) {
    if (craftingSlotVisible(player, slot) && recipeSlotIs(player, slot, WOOD) &&
        recipeHasOnly(player, 1 << slot)) {
      result->item = PLANKS;
      result->count = 4;
      *used_slots = 1 << slot;
      return TRUE;
    }
  }
  for (column = 0; column < craftingColumns(player); column++) {
    u16 used = (1 << column) | (1 << (column + CRAFTING_TABLE_COLUMNS));
    if (recipeSlotIs(player, column, PLANKS) &&
        recipeSlotIs(player, column + CRAFTING_TABLE_COLUMNS, PLANKS) &&
        recipeHasOnly(player, used)) {
      result->item = STICK;
      result->count = 4;
      *used_slots = used;
      return TRUE;
    }
  }
  if (!player->crafting_table_open && recipeSlotIs(player, 0, PLANKS) &&
      recipeSlotIs(player, 1, PLANKS) && recipeSlotIs(player, 3, PLANKS) &&
      recipeSlotIs(player, 4, PLANKS) && recipeHasOnly(player,
        (1 << 0) | (1 << 1) | (1 << 3) | (1 << 4))) {
    result->item = CRAFTING_TABLE;
    result->count = 1;
    *used_slots = (1 << 0) | (1 << 1) | (1 << 3) | (1 << 4);
    return TRUE;
  }
  if (player->crafting_table_open && recipeSlotIs(player, 0, PLANKS) &&
      recipeSlotIs(player, 3, PLANKS) && recipeSlotIs(player, 6, STICK) &&
      recipeHasOnly(player, (1 << 0) | (1 << 3) | (1 << 6))) {
    result->item = WOOD_SWORD;
    result->count = 1;
    *used_slots = (1 << 0) | (1 << 3) | (1 << 6);
    return TRUE;
  }
  if (player->crafting_table_open && recipeSlotIs(player, 0, PLANKS) &&
      recipeSlotIs(player, 1, PLANKS) && recipeSlotIs(player, 2, PLANKS) &&
      recipeSlotIs(player, 4, STICK) && recipeSlotIs(player, 7, STICK) && recipeHasOnly(player,
        (1 << 0) | (1 << 1) | (1 << 2) | (1 << 4) | (1 << 7))) {
    result->item = WOOD_PICKAXE;
    result->count = 1;
    *used_slots = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 4) | (1 << 7);
    return TRUE;
  }
  result->item = AIR;
  result->count = 0;
  *used_slots = 0;
  return FALSE;
}

u8 getCraftResult(Player *player, ItemStack *result) {
  u16 used_slots;
  return getCraftRecipe(player, result, &used_slots);
}

static void swapItemStacks(ItemStack *one, ItemStack *two) {
  ItemStack temporary = *one;
  *one = *two;
  *two = temporary;
}

/* A moves a complete stack.  B is the precise placement control needed for
   patterns such as two planks over a stick without discarding the remainder. */
static void moveOneItem(ItemStack *target, ItemStack *carried) {
  if (carried->count > 0) {
    if (target->count == 0) {
      target->item = carried->item;
      target->count = 1;
      carried->count--;
    } else if (target->item == carried->item &&
        target->count < itemMaxStack(target->item)) {
      target->count++;
      carried->count--;
    }
    if (carried->count == 0) {
      carried->item = AIR;
    }
  } else if (target->count > 0) {
    carried->item = target->item;
    carried->count = 1;
    target->count--;
    if (target->count == 0) {
      target->item = AIR;
    }
  }
}

static void craftOutput(Player *player) {
  ItemStack result;
  u16 used_slots;
  u8 slot;

  if (!getCraftRecipe(player, &result, &used_slots) ||
      (player->carried_item.count > 0 && player->carried_item.item != result.item) ||
      player->carried_item.count + result.count > itemMaxStack(result.item)) {
    return;
  }
  for (slot = 0; slot < CRAFTING_SIZE; slot++) {
    if (used_slots & (1 << slot)) {
      player->crafting[slot].count--;
      if (player->crafting[slot].count == 0) {
        player->crafting[slot].item = AIR;
      }
    }
  }
  player->carried_item.item = result.item;
  player->carried_item.count += result.count;
}

static void returnCraftingItems(Player *player) {
  u8 slot;
  for (slot = 0; slot < CRAFTING_SIZE; slot++) {
    if (player->crafting[slot].count > 0) {
      u8 added = addItemToInventory(player, player->crafting[slot].item,
        player->crafting[slot].count);
      player->crafting[slot].count -= added;
      if (player->crafting[slot].count == 0) {
        player->crafting[slot].item = AIR;
      }
    }
  }
  if (player->carried_item.count > 0) {
    u8 added = addItemToInventory(player, player->carried_item.item,
      player->carried_item.count);
    player->carried_item.count -= added;
    if (player->carried_item.count == 0) {
      player->carried_item.item = AIR;
    }
  }
}

static void spawnPlayer(Player *player, int x, int z) {
  int y;

  player->pitch = 0;
  player->yaw = 0;
  player->body_yaw = 0;
  player->walk_time = 0;
  player->walk_swing = 0;
  player->y_velocity = 0;
  player->camera_mode = CAMERA_FIRST_PERSON;
  player->held_block = COBBLESTONE;
  resetPlayerInventory(player);
  player->active = TRUE;
  player->target_present = FALSE;
  player->breaking = FALSE;
  player->break_progress = 0;
  player->break_time = WOOD_BREAK_TIME;
  player->position.x = (x + 0.5) * BLOCK_SIZE;
  player->position.z = (z + 0.5) * BLOCK_SIZE;

  for (y = MAX_Y - 1; y >= 0; y--) {
    if (blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z]) {
      player->position.y = (y + 1 + EYE_HEIGHT) * BLOCK_SIZE;
      return;
    }
  }

  player->position.y = (MAX_Y + EYE_HEIGHT) * BLOCK_SIZE;
}

void initPlayers() {
  active_player_count = 1;
  spawnPlayer(&players[0], START_X, START_Z);
  players[1].active = FALSE;
  players[1].target_present = FALSE;
}

void activatePlayerTwo() {
  if (!players[1].active) {
    /* A nearby, separate spawn avoids an immediate overlap without needing
       a second world copy or an expensive entity system. */
    spawnPlayer(&players[1], START_X + 3, START_Z);
    active_player_count = 2;
    player_two_joined_message = 120;
  }
}

void updateTargetBlock(u8 player_num) {
  Player *player = &players[player_num];
  float t;
  Vector3i step = {0, 0, 0};
  Vector3 direction = {0, 0, -1};
  Vector3 origin = playerCameraPosition(player_num);
  float ray_limit = BLOCK_SIZE *
    (player->camera_mode == CAMERA_THIRD_PERSON ? 9.f : 6.f);

  direction = rotateX(direction, player->pitch);
  direction = rotateY(direction, -player->yaw);

  player->target_x = origin.x / BLOCK_SIZE;
  player->target_y = origin.y / BLOCK_SIZE;
  player->target_z = origin.z / BLOCK_SIZE;

  player->target_present = TRUE;
  while (player->target_x >= MAX_X || player->target_y >= MAX_Y || player->target_z >= MAX_Z ||
    !blocks[player->target_x * MAX_Y * MAX_Z + player->target_y * MAX_Z + player->target_z]) {
    t = 9999;

    rayStepAxis(origin.x, direction.x, player->target_x, &t, &step, 0);
    rayStepAxis(origin.y, direction.y, player->target_y, &t, &step, 1);
    rayStepAxis(origin.z, direction.z, player->target_z, &t, &step, 2);

    if (t > ray_limit || (player->target_x == 0 && step.x < 0) ||
        (player->target_y == 0 && step.y < 0) || (player->target_z == 0 && step.z < 0)) {
      player->target_present = FALSE;
      break;
    }

    player->target_x += step.x;
    player->target_y += step.y;
    player->target_z += step.z;
  }

  if (player->target_present) {
    float dx = (player->target_x + 0.5f) * BLOCK_SIZE -
      player->position.x;
    float dy = (player->target_y + 0.5f) * BLOCK_SIZE -
      player->position.y;
    float dz = (player->target_z + 0.5f) * BLOCK_SIZE -
      player->position.z;
    if (dx * dx + dy * dy + dz * dz >
        (BLOCK_SIZE * 6.5f) * (BLOCK_SIZE * 6.5f)) {
      player->target_present = FALSE;
      return;
    }
    player->build_offset_x = -step.x;
    player->build_offset_y = -step.y;
    player->build_offset_z = -step.z;
  }
}

static void boxBlockRange(Vector3 pos, Vector3i *min_block, Vector3i *max_block) {
  int i;
  Vector3 v;
  Vector3 box_min = {9999, 9999, 9999};
  Vector3 box_max = {-9999, -9999, -9999};

  for (i = 0; i < 8; i++) {
    v = add(pos, bounding_box[i]);
    box_min.x = min(box_min.x, v.x);
    box_min.y = min(box_min.y, v.y);
    box_min.z = min(box_min.z, v.z);
    box_max.x = max(box_max.x, v.x);
    box_max.y = max(box_max.y, v.y);
    box_max.z = max(box_max.z, v.z);
  }

  min_block->x = floor(box_min.x);
  min_block->y = floor(box_min.y);
  min_block->z = floor(box_min.z);
  max_block->x = floor(box_max.x);
  max_block->y = floor(box_max.y);
  max_block->z = floor(box_max.z);
}

static u8 boxObstructed(Vector3 pos, int override_axis, int override_block) {
  int x, y, z;
  Vector3i min_block, max_block;

  boxBlockRange(pos, &min_block, &max_block);
  *ati(&min_block, override_axis) = override_block;
  *ati(&max_block, override_axis) = override_block;

  for (x = min_block.x; x <= max_block.x; x++) {
    for (y = min_block.y; y <= max_block.y; y++) {
      for (z = min_block.z; z <= max_block.z; z++) {
        if (x < 0 || y < 0 || z < 0 || x >= MAX_X || z >= MAX_Z ||
            (y < MAX_Y && blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z])) {
          return TRUE;
        }
      }
    }
  }
  return FALSE;
}

static float detectCollision(Player *player, Vector3 velocity, float max_t, int *collision_axis) {
  float t;
  Vector3i step;
  int step_axis;
  Vector3 pos;
  Vector3 origin = add(player->position,
    mul(bounding_box[(velocity.x > 0) * 4 + (velocity.y > 0) * 2 + (velocity.z > 0)], BLOCK_SIZE));
  Vector3i block = divToInt(origin, BLOCK_SIZE);

  while (TRUE) {
    t = 1;
    rayStepAxis(origin.x, velocity.x, block.x, &t, &step, 0);
    rayStepAxis(origin.y, velocity.y, block.y, &t, &step, 1);
    rayStepAxis(origin.z, velocity.z, block.z, &t, &step, 2);

    if (t >= max_t) {
      return max_t;
    }
    step_axis = step.x != 0 ? 0 : (step.y != 0 ? 1 : 2);
    block = addi(block, step);
    pos = div(add(player->position, mul(velocity, t)), BLOCK_SIZE);

    if (boxObstructed(pos, step_axis, *ati(&block, step_axis))) {
      *collision_axis = step_axis;
      return t;
    }
  }
}

static u8 blockUsesInventory(u8 block) {
  return block >= FIRST_PLACEABLE_BLOCK && block <= BLOCK_TYPE_COUNT;
}

static void placeBlock(u8 player_num, u8 x, u8 y, u8 z) {
  int bx, by, bz, i;
  Vector3i min_block, max_block;
  Player *player = &players[player_num];
  ItemStack *held_stack = &player->inventory[
    INVENTORY_HOTBAR_START + player->selected_hotbar_slot];

  if (x >= MAX_X || y >= MAX_Y || z >= MAX_Z ||
      player->held_block < FIRST_PLACEABLE_BLOCK || player->held_block > BLOCK_TYPE_COUNT) {
    return;
  }

  if (blockUsesInventory(player->held_block) &&
      (held_stack->item != player->held_block || held_stack->count == 0)) {
    return;
  }

  /* Co-op cannot bury either player inside a freshly placed block. */
  for (i = 0; i < active_player_count; i++) {
    boxBlockRange(div(players[i].position, BLOCK_SIZE), &min_block, &max_block);
    for (bx = min_block.x; bx <= max_block.x; bx++) {
      for (by = min_block.y; by <= max_block.y; by++) {
        for (bz = min_block.z; bz <= max_block.z; bz++) {
          if (bx == x && by == y && bz == z) {
            return;
          }
        }
      }
    }
  }

  blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z] = player->held_block;
  if (blockUsesInventory(player->held_block)) {
    held_stack->count--;
  }
  regenerateBlock(x, y, z);
  makeDisplayListsAt(x, z);
}

static u8 dropForBlock(u8 block, u8 tool, u8 *item) {
  /* Rock is deliberately breakable by hand, but only a pickaxe harvests a
     cube.  The remaining current terrain is soft or wooden enough to gather
     with the tools the game already offers. */
  if ((block == STONE || block == COBBLESTONE || block == BRICKS) &&
      tool != WOOD_PICKAXE) {
    return FALSE;
  }
  *item = block;
  return TRUE;
}

static u8 breakBlock(u8 x, u8 y, u8 z, u8 tool) {
  u8 block = blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z];
  u8 item;

  /* Every terrain block that has a valid harvest yields the same small cube
     that can later be placed.  If the item pool is full, preserve the block
     rather than silently destroying its resource. */
  if (dropForBlock(block, tool, &item) &&
      !spawnDroppedItem(item, 1, x, y, z)) {
    return FALSE;
  }
  blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z] = AIR;
  regenerateBlock(x, y, z);
  makeDisplayListsAt(x, z);
  return TRUE;
}

static void resetBreaking(Player *player) {
  player->breaking = FALSE;
  player->break_progress = 0;
}

static u8 heldItem(Player *player) {
  ItemStack *held_stack = &player->inventory[
    INVENTORY_HOTBAR_START + player->selected_hotbar_slot];

  return held_stack->count > 0 ? held_stack->item : AIR;
}

static float blockBreakTime(u8 block, u8 tool) {
  switch (block) {
    case LEAVES:
      return tool == WOOD_SWORD ? 5.f : 12.f;
    case DIRT:
    case GRASS:
    case SAND:
      return 22.f;
    case WOOD:
    case PLANKS:
    case CRAFTING_TABLE:
      return 36.f;
    case STONE:
    case COBBLESTONE:
    case BRICKS:
      return tool == WOOD_PICKAXE ? 32.f : 120.f;
    default:
      return 0;
  }
}

static void updateBreaking(u8 player_num, float delta) {
  Player *player = &players[player_num];
  NUContData *cont = &cont_data[player_num];
  u8 block;
  float break_time;

  if (!(cont->button & B_BUTTON) || !player->target_present) {
    resetBreaking(player);
    return;
  }

  block = blocks[player->target_x * MAX_Y * MAX_Z +
    player->target_y * MAX_Z + player->target_z];
  break_time = blockBreakTime(block, heldItem(player));
  if (break_time <= 0) {
    resetBreaking(player);
    return;
  }

  if (!player->breaking || player->breaking_x != player->target_x ||
      player->breaking_y != player->target_y || player->breaking_z != player->target_z) {
    player->breaking = TRUE;
    player->breaking_x = player->target_x;
    player->breaking_y = player->target_y;
    player->breaking_z = player->target_z;
    player->break_progress = 0;
  }
  player->break_time = break_time;

  player->break_progress += delta;
  if (player->break_progress >= player->break_time) {
    if (breakBlock(player->target_x, player->target_y,
        player->target_z, heldItem(player))) {
      resetBreaking(player);
    } else {
      player->break_progress = player->break_time;
    }
  }
}

static u8 onGround(Player *player) {
  int x, y, z;
  Vector3i min_block, max_block;
  Vector3 low_pos = div(player->position, BLOCK_SIZE);
  low_pos.y -= 0.01;
  boxBlockRange(low_pos, &min_block, &max_block);

  y = min_block.y;
  for (x = min_block.x; x <= max_block.x; x++) {
    for (z = min_block.z; z <= max_block.z; z++) {
      if (x < 0 || y < 0 || z < 0 || x >= MAX_X || z >= MAX_Z ||
          (y < MAX_Y && blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z])) {
        return TRUE;
      }
    }
  }
  return FALSE;
}

static void updatePlayer(u8 player_num, float delta) {
  Player *player = &players[player_num];
  NUContData *cont = &cont_data[player_num];
  Vector3 velocity = {0, 0, 0};
  float t, t_total = 0;
  float move_t;
  int collision_axis = 0;
  s8 stick_x = cont->stick_x;
  s8 stick_y = cont->stick_y;

  if (cont->trigger & U_CBUTTONS) {
    player->camera_mode = player->camera_mode == CAMERA_FIRST_PERSON ?
      CAMERA_THIRD_PERSON : CAMERA_FIRST_PERSON;
  }

  if (stick_x > -4 && stick_x < 4) stick_x = 0;
  if (stick_y > -4 && stick_y < 4) stick_y = 0;

  if (cont->button & Z_TRIG) {
    player->yaw -= stick_x * delta / STICK_DAMPER;
    player->pitch += stick_y * delta / STICK_DAMPER;
    if (player->yaw < 0) player->yaw += 360;
    else if (player->yaw >= 360) player->yaw -= 360;
    if (player->pitch < 0) player->pitch += 360;
    else if (player->pitch >= 360) player->pitch -= 360;
    if (player->pitch > 90 && player->pitch < 180) player->pitch = 90;
    if (player->pitch < 270 && player->pitch > 180) player->pitch = 270;
  } else {
    velocity.x += stick_x * cosf(player->yaw * M_DTOR) - stick_y * sinf(player->yaw * M_DTOR);
    velocity.z -= stick_x * sinf(player->yaw * M_DTOR) + stick_y * cosf(player->yaw * M_DTOR);
    velocity.x *= MOVE_SPEED;
    velocity.z *= MOVE_SPEED;
    if (cont->button & L_TRIG) {
      velocity.x *= SPRINT_MULTIPLIER;
      velocity.z *= SPRINT_MULTIPLIER;
    }
  }

  /* The torso follows travel, while the avatar's head continues to follow
     the camera direction when the player stops. */
  if (velocity.x != 0 || velocity.z != 0) {
    player->body_yaw = player->yaw;
    player->walk_time += delta * 12;
    player->walk_swing = min(1, player->walk_swing + delta / 8);
  } else {
    /* Ease a stopped avatar back to a neutral pose instead of leaving a
       leg or arm hanging at the last animation frame. */
    player->walk_swing = max(0, player->walk_swing - delta / 6);
  }

  if (cont->button & L_CBUTTONS) {
    if (!block_dec_held[player_num]) {
      u8 selected_slot;
      block_dec_held[player_num] = TRUE;
      selected_slot = player->selected_hotbar_slot == 0 ?
        INVENTORY_COLUMNS - 1 : player->selected_hotbar_slot - 1;
      selectHotbarSlot(player, selected_slot);
    }
  } else block_dec_held[player_num] = FALSE;

  if (cont->button & R_CBUTTONS) {
    if (!block_inc_held[player_num]) {
      u8 selected_slot;
      block_inc_held[player_num] = TRUE;
      selected_slot = (player->selected_hotbar_slot + 1) % INVENTORY_COLUMNS;
      selectHotbarSlot(player, selected_slot);
    }
  } else block_inc_held[player_num] = FALSE;

  if (onGround(player)) {
    player->y_velocity = (cont->button & R_TRIG) ? JUMP_SPEED : 0;
  } else if (player->y_velocity > -TERMINAL_SPEED) {
    player->y_velocity -= GRAVITY * delta;
  }
  velocity.y += player->y_velocity;

  velocity = mul(velocity, delta);
  while (t_total < 1) {
    t = detectCollision(player, velocity, 1 - t_total, &collision_axis);
    move_t = t > 0.01f ? t - 0.01f : 0;
    player->position = add(player->position, mul(velocity, move_t));
    t_total += t;
    if (t_total < 1) {
      *at(&velocity, collision_axis) = 0;
      if (collision_axis == 1) player->y_velocity = 0;
    }
  }

  updateTargetBlock(player_num);
  if ((cont->trigger & A_BUTTON) && player->target_present) {
    placeBlock(player_num, player->build_offset_x + player->target_x,
      player->build_offset_y + player->target_y, player->build_offset_z + player->target_z);
  }
  updateBreaking(player_num, delta);
}

void updatePlayers() {
  OSTime time;
  float delta;
  u16 down_pressed, up_pressed, act_pressed;
  u8 i;

  nuContDataGetEx(&cont_data[0], 0);
  nuContDataGetEx(&cont_data[1], 1);
  time = osGetTime();
  delta = last_time == 0 ? 1.f :
    OS_CYCLES_TO_USEC(time - last_time) * 60 / 1000000.f;
  last_time = time;
  if (delta > MAX_FRAME_DELTA) {
    delta = MAX_FRAME_DELTA;
  }

  if (current_screen == INVENTORY) {
    Player *player = &players[inventory_player];
    NUContData *inventory_cont = &cont_data[inventory_player];
    u8 navigation = inventoryNavigation(inventory_cont);
    u8 row = player->inventory_cursor / INVENTORY_COLUMNS;
    u8 column = player->inventory_cursor % INVENTORY_COLUMNS;

    if (inventory_cont->trigger & START_BUTTON) {
      returnCraftingItems(player);
      resetInventoryNavigation();
      current_screen = GAME;
      return;
    }
    if (inventory_cont->trigger & A_BUTTON) {
      if (player->inventory_area == INVENTORY_AREA_CRAFTING) {
        swapItemStacks(&player->crafting[player->crafting_cursor], &player->carried_item);
      } else if (player->inventory_area == INVENTORY_AREA_OUTPUT) {
        craftOutput(player);
      } else {
        swapItemStacks(&player->inventory[player->inventory_cursor], &player->carried_item);
        if (player->inventory_cursor >= INVENTORY_HOTBAR_START &&
            player->inventory_cursor - INVENTORY_HOTBAR_START == player->selected_hotbar_slot) {
          player->held_block = player->inventory[player->inventory_cursor].item;
        }
      }
      return;
    }
    if (inventory_cont->trigger & B_BUTTON) {
      if (player->inventory_area == INVENTORY_AREA_CRAFTING) {
        moveOneItem(&player->crafting[player->crafting_cursor], &player->carried_item);
      } else if (player->inventory_area == INVENTORY_AREA_ITEMS) {
        moveOneItem(&player->inventory[player->inventory_cursor], &player->carried_item);
        if (player->inventory_cursor >= INVENTORY_HOTBAR_START &&
            player->inventory_cursor - INVENTORY_HOTBAR_START == player->selected_hotbar_slot) {
          player->held_block = player->inventory[player->inventory_cursor].item;
        }
      }
      return;
    }

    if (player->inventory_area == INVENTORY_AREA_CRAFTING) {
      u8 craft_columns = craftingColumns(player);
      u8 craft_rows = craftingRows(player);
      row = player->crafting_cursor / CRAFTING_TABLE_COLUMNS;
      column = player->crafting_cursor % CRAFTING_TABLE_COLUMNS;
      if (navigation & NAV_LEFT) {
        column = column == 0 ? craft_columns - 1 : column - 1;
      } else if (navigation & NAV_RIGHT) {
        if (column == craft_columns - 1) {
          player->inventory_area = INVENTORY_AREA_OUTPUT;
          return;
        }
        column++;
      } else if (navigation & NAV_UP) {
        row = row == 0 ? craft_rows - 1 : row - 1;
      } else if (navigation & NAV_DOWN) {
        row = (row + 1) % craft_rows;
      } else {
        return;
      }
      player->crafting_cursor = row * CRAFTING_TABLE_COLUMNS + column;
      return;
    }

    if (player->inventory_area == INVENTORY_AREA_OUTPUT) {
      if (navigation & NAV_LEFT) {
        player->inventory_area = INVENTORY_AREA_CRAFTING;
        player->crafting_cursor = (craftingRows(player) / 2) *
          CRAFTING_TABLE_COLUMNS + craftingColumns(player) - 1;
      } else if (navigation & NAV_RIGHT) {
        player->inventory_area = INVENTORY_AREA_ITEMS;
        player->inventory_cursor = INVENTORY_COLUMNS;
      }
      return;
    }

    if (navigation & NAV_LEFT) {
      if (column == 0) {
        player->inventory_area = INVENTORY_AREA_CRAFTING;
        player->crafting_cursor = (row < craftingRows(player) ? row : craftingRows(player) - 1) *
          CRAFTING_TABLE_COLUMNS + (craftingColumns(player) - 1);
        return;
      }
      column--;
    } else if (navigation & NAV_RIGHT) {
      column = (column + 1) % INVENTORY_COLUMNS;
    } else if (navigation & NAV_UP) {
      row = row == 0 ? INVENTORY_STORAGE_ROWS : row - 1;
    } else if (navigation & NAV_DOWN) {
      row = row == INVENTORY_STORAGE_ROWS ? 0 : row + 1;
    } else {
      return;
    }

    player->inventory_cursor = row * INVENTORY_COLUMNS + column;
    if (row == INVENTORY_STORAGE_ROWS) {
      selectHotbarSlot(player, column);
    }
    return;
  }

  if (current_screen != GAME) {
    down_pressed = (cont_data[0].button & D_CBUTTONS) || cont_data[0].stick_y < -50;
    up_pressed = (cont_data[0].button & U_CBUTTONS) || cont_data[0].stick_y > 50;
    act_pressed = cont_data[0].button & (START_BUTTON | A_BUTTON | B_BUTTON);
    if (down_pressed && !down_held) menuDown();
    if (up_pressed && !up_held) menuUp();
    if (act_pressed && !act_held) menuAct();
    down_held = down_pressed;
    up_held = up_pressed;
    act_held = act_pressed;
    return;
  }

  if (!players[1].active && (cont_data[1].trigger & START_BUTTON)) {
    activatePlayerTwo();
    return;
  }
  for (i = 0; i < active_player_count; i++) {
    if (cont_data[i].trigger & START_BUTTON) {
      players[i].crafting_table_open = players[i].target_present &&
        blocks[players[i].target_x * MAX_Y * MAX_Z + players[i].target_y * MAX_Z +
          players[i].target_z] == CRAFTING_TABLE;
      players[i].inventory_area = INVENTORY_AREA_ITEMS;
      inventory_player = i;
      resetInventoryNavigation();
      current_screen = INVENTORY;
      return;
    }
  }
  for (i = 0; i < active_player_count; i++) {
    updatePlayer(i, delta);
  }
  updateDroppedItems(delta);

  if (saving_available) {
    for (i = 0; i < active_player_count; i++) {
      if (cont_data[i].trigger &
          (U_JPAD | D_JPAD | L_JPAD | R_JPAD)) {
        if (saveGame()) {
          save_message_cooldown = 60;
        } else {
          saving_available = FALSE;
          save_failed_message = 120;
        }
        break;
      }
    }
  }
}
