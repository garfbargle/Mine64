#include <nusys.h>
#include "player.h"
#include "math.h"
#include "menu.h"
#include "blocks.h"
#include "graphics.h"
#include "geometry.h"
#include "items.h"
#include "storage.h"

#define START_X 32
#define START_Z 48

#define STICK_DAMPER 22
#define MOVE_SPEED (1 / 8.f)
#define JUMP_SPEED (BLOCK_SIZE / 4.5)
#define TERMINAL_SPEED (BLOCK_SIZE / 2)
#define GRAVITY (BLOCK_SIZE / 40)
#define BOX_RADIUS 0.35
#define BOX_HEIGHT 1.8
#define EYE_HEIGHT 1.5

Player players[MAX_PLAYERS];
u8 active_player_count = 1;

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
    /* Logs and planks are gathered/crafted materials.  The other current
       block types retain their creative stacks until their own gathering
       rules are added. */
    player->inventory[INVENTORY_HOTBAR_START + slot].count =
      (slot + FIRST_PLACEABLE_BLOCK == WOOD ||
       slot + FIRST_PLACEABLE_BLOCK == PLANKS) ? 0 : MAX_ITEM_STACK;
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

  for (slot = 0; slot < INVENTORY_SIZE && remaining > 0; slot++) {
    ItemStack *stack = &player->inventory[slot];
    u8 added;
    if (stack->item != item || stack->count >= MAX_ITEM_STACK) {
      continue;
    }
    added = remaining < MAX_ITEM_STACK - stack->count ? remaining : MAX_ITEM_STACK - stack->count;
    stack->count += added;
    remaining -= added;
  }
  for (slot = 0; slot < INVENTORY_SIZE && remaining > 0; slot++) {
    ItemStack *stack = &player->inventory[slot];
    u8 added;
    if (stack->count != 0) {
      continue;
    }
    added = remaining < MAX_ITEM_STACK ? remaining : MAX_ITEM_STACK;
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
    } else if (target->item == carried->item && target->count < MAX_ITEM_STACK) {
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
      player->carried_item.count + result.count > MAX_ITEM_STACK) {
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
    if (player->crafting[slot].count > 0 &&
        addItemToInventory(player, player->crafting[slot].item, player->crafting[slot].count) ==
          player->crafting[slot].count) {
      player->crafting[slot].item = AIR;
      player->crafting[slot].count = 0;
    }
  }
  if (player->carried_item.count > 0 &&
      addItemToInventory(player, player->carried_item.item, player->carried_item.count) ==
        player->carried_item.count) {
    player->carried_item.item = AIR;
    player->carried_item.count = 0;
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
  player->held_block = COBBLESTONE;
  resetPlayerInventory(player);
  player->active = TRUE;
  player->target_present = FALSE;
  player->breaking = FALSE;
  player->break_progress = 0;
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
  Vector3i step;
  Vector3 direction = {0, 0, -1};

  direction = rotateX(direction, player->pitch);
  direction = rotateY(direction, -player->yaw);

  player->target_x = player->position.x / BLOCK_SIZE;
  player->target_y = player->position.y / BLOCK_SIZE;
  player->target_z = player->position.z / BLOCK_SIZE;

  player->target_present = TRUE;
  while (player->target_x >= MAX_X || player->target_y >= MAX_Y || player->target_z >= MAX_Z ||
    !blocks[player->target_x * MAX_Y * MAX_Z + player->target_y * MAX_Z + player->target_z]) {
    t = 9999;

    rayStepAxis(player->position.x, direction.x, player->target_x, &t, &step, 0);
    rayStepAxis(player->position.y, direction.y, player->target_y, &t, &step, 1);
    rayStepAxis(player->position.z, direction.z, player->target_z, &t, &step, 2);

    if (t > BLOCK_SIZE * 6 || (player->target_x == 0 && step.x < 0) ||
        (player->target_y == 0 && step.y < 0) || (player->target_z == 0 && step.z < 0)) {
      player->target_present = FALSE;
      break;
    }

    player->target_x += step.x;
    player->target_y += step.y;
    player->target_z += step.z;
  }

  if (player->target_present) {
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

  if ((player->held_block == WOOD || player->held_block == CRAFTING_TABLE) &&
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
  if (player->held_block == WOOD || player->held_block == CRAFTING_TABLE) {
    held_stack->count--;
  }
  regenerateBlock(x, y, z);
  makeDisplayListsAt(x, z);
}

static void breakBlock(u8 x, u8 y, u8 z) {
  u8 block = blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z];

  /* The log turns into a physical pickup rather than entering inventory
     immediately.  A full stack therefore leaves the wood on the ground. */
  if (block == WOOD) {
    spawnDroppedItem(WOOD, 1, x, y, z);
  }
  blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z] = AIR;
  regenerateBlock(x, y, z);
  makeDisplayListsAt(x, z);
}

static void resetBreaking(Player *player) {
  player->breaking = FALSE;
  player->break_progress = 0;
}

static void updateBreaking(u8 player_num, float delta) {
  Player *player = &players[player_num];
  NUContData *cont = &cont_data[player_num];

  /* For now, punching is intentionally a tree-harvesting action.  Other
     terrain remains untouched until it has a proper tool/drop rule. */
  if (!(cont->button & B_BUTTON) || !player->target_present ||
      blocks[player->target_x * MAX_Y * MAX_Z + player->target_y * MAX_Z + player->target_z] != WOOD) {
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

  player->break_progress += delta;
  if (player->break_progress >= WOOD_BREAK_TIME) {
    breakBlock(player->target_x, player->target_y, player->target_z);
    resetBreaking(player);
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
  int collision_axis = 0;
  s8 stick_x = cont->stick_x;
  s8 stick_y = cont->stick_y;

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
    player->y_velocity -= GRAVITY;
  }
  velocity.y += player->y_velocity;

  velocity = mul(velocity, delta);
  while (t_total < 1) {
    t = detectCollision(player, velocity, 1 - t_total, &collision_axis);
    player->position = add(player->position, mul(velocity, t - 0.01));
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
  delta = OS_CYCLES_TO_USEC(time - last_time) * 60 / 1000000.f;
  last_time = time;

  if (current_screen == INVENTORY) {
    Player *player = &players[0];
    u8 row = player->inventory_cursor / INVENTORY_COLUMNS;
    u8 column = player->inventory_cursor % INVENTORY_COLUMNS;

    if (cont_data[0].trigger & START_BUTTON) {
      returnCraftingItems(player);
      current_screen = GAME;
      return;
    }
    if (cont_data[0].trigger & A_BUTTON) {
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
    if (cont_data[0].trigger & B_BUTTON) {
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
      if (cont_data[0].trigger & L_CBUTTONS) {
        column = column == 0 ? craft_columns - 1 : column - 1;
      } else if (cont_data[0].trigger & R_CBUTTONS) {
        if (column == craft_columns - 1) {
          player->inventory_area = INVENTORY_AREA_OUTPUT;
          return;
        }
        column++;
      } else if (cont_data[0].trigger & U_JPAD) {
        row = row == 0 ? craft_rows - 1 : row - 1;
      } else if (cont_data[0].trigger & D_JPAD) {
        row = (row + 1) % craft_rows;
      } else {
        return;
      }
      player->crafting_cursor = row * CRAFTING_TABLE_COLUMNS + column;
      return;
    }

    if (player->inventory_area == INVENTORY_AREA_OUTPUT) {
      if (cont_data[0].trigger & L_CBUTTONS) {
        player->inventory_area = INVENTORY_AREA_CRAFTING;
        player->crafting_cursor = 1 + CRAFTING_TABLE_COLUMNS;
      } else if (cont_data[0].trigger & R_CBUTTONS) {
        player->inventory_area = INVENTORY_AREA_ITEMS;
        player->inventory_cursor = INVENTORY_COLUMNS;
      }
      return;
    }

    if (cont_data[0].trigger & L_CBUTTONS) {
      if (column == 0) {
        player->inventory_area = INVENTORY_AREA_CRAFTING;
        player->crafting_cursor = (row < craftingRows(player) ? row : craftingRows(player) - 1) *
          CRAFTING_TABLE_COLUMNS + (craftingColumns(player) - 1);
        return;
      }
      column--;
    } else if (cont_data[0].trigger & R_CBUTTONS) {
      column = (column + 1) % INVENTORY_COLUMNS;
    } else if (cont_data[0].trigger & U_JPAD) {
      row = row == 0 ? INVENTORY_STORAGE_ROWS : row - 1;
    } else if (cont_data[0].trigger & D_JPAD) {
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
  }
  if (cont_data[0].trigger & START_BUTTON) {
    players[0].crafting_table_open = players[0].target_present &&
      blocks[players[0].target_x * MAX_Y * MAX_Z + players[0].target_y * MAX_Z +
        players[0].target_z] == CRAFTING_TABLE;
    players[0].inventory_area = INVENTORY_AREA_ITEMS;
    current_screen = INVENTORY;
    return;
  }
  for (i = 0; i < active_player_count; i++) {
    updatePlayer(i, delta);
  }
  updateDroppedItems(delta);

  if (saving_available && (cont_data[0].trigger & (U_JPAD | D_JPAD | L_JPAD | R_JPAD))) {
    saveGame();
    save_message_cooldown = 60;
  }
}
