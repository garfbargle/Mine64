#include <nusys.h>
#include "player.h"
#include "math.h"
#include "menu.h"
#include "blocks.h"
#include "graphics.h"
#include "camera.h"
#include "geometry.h"
#include "items.h"
#include "mobs.h"
#include "trees.h"
#include "storage.h"
#include "audio.h"
#include "world.h"

#define START_X (MAX_X / 2)
#define START_Z (MAX_Z / 2)

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

const CraftRecipe craft_recipes[CRAFT_RECIPE_COUNT] = {
  {PLANKS, 4, {WOOD, AIR}, {1, 0}},
  {STICK, 4, {PLANKS, AIR}, {2, 0}},
  {CRAFTING_TABLE, 1, {PLANKS, AIR}, {4, 0}},
  {WOOD_SWORD, 1, {PLANKS, STICK}, {2, 1}},
  {WOOD_PICKAXE, 1, {PLANKS, STICK}, {3, 2}},
  {WOOD_AXE, 1, {PLANKS, STICK}, {3, 2}},
  {STONE_SWORD, 1, {COBBLESTONE, STICK}, {2, 1}},
  {STONE_PICKAXE, 1, {COBBLESTONE, STICK}, {3, 2}},
  {STONE_AXE, 1, {COBBLESTONE, STICK}, {3, 2}},
  {IRON_SWORD, 1, {IRON_CHUNK, STICK}, {2, 1}},
  {IRON_PICKAXE, 1, {IRON_CHUNK, STICK}, {3, 2}},
  {IRON_AXE, 1, {IRON_CHUNK, STICK}, {3, 2}}
};

static void resetInventoryNavigation(void) {
  inventory_nav_previous = 0;
  inventory_nav_repeat = 0;
}

static u8 inventoryNavigation(NUContData *cont) {
  u8 held = 0;
  u8 pressed;

  /* The analog stick exclusively owns menu navigation.  C buttons are
     reserved for contextual inventory actions and the D-pad remains a
     gameplay save control. */
  if (cont->stick_x < -INVENTORY_STICK_THRESHOLD) {
    held |= NAV_LEFT;
  }
  if (cont->stick_x > INVENTORY_STICK_THRESHOLD) {
    held |= NAV_RIGHT;
  }
  if (cont->stick_y > INVENTORY_STICK_THRESHOLD) {
    held |= NAV_UP;
  }
  if (cont->stick_y < -INVENTORY_STICK_THRESHOLD) {
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

static void refreshHeldItem(Player *player) {
  player->held_block = player->inventory[INVENTORY_HOTBAR_START +
    player->selected_hotbar_slot].item;
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

static void swapItemStacks(ItemStack *one, ItemStack *two) {
  ItemStack temporary = *one;
  *one = *two;
  *two = temporary;
}

/* The Hand slot keeps conventional inventory rearranging available: A swaps
   full stacks and C-left transfers a single item. */
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

u8 playerRecipeCount(Player *player) {
  return player->crafting_table_open ? CRAFT_RECIPE_COUNT :
    POCKET_RECIPE_COUNT;
}

static u8 removeItemFromStacks(ItemStack *stacks, u8 item, u8 count) {
  u8 slot;
  u8 remaining = count;

  for (slot = 0; slot < INVENTORY_SIZE && remaining > 0; slot++) {
    ItemStack *stack = &stacks[slot];
    u8 removed;

    if (stack->item != item || stack->count == 0) {
      continue;
    }
    removed = remaining < stack->count ? remaining : stack->count;
    stack->count -= removed;
    remaining -= removed;
    if (stack->count == 0) {
      stack->item = AIR;
    }
  }
  return remaining == 0;
}

static u8 addItemToStacks(ItemStack *stacks, u8 item, u8 count) {
  u8 slot;
  u8 remaining = count;
  u8 max_stack = itemMaxStack(item);

  for (slot = 0; slot < INVENTORY_SIZE && remaining > 0; slot++) {
    ItemStack *stack = &stacks[slot];
    u8 added;

    if (stack->item != item || stack->count >= max_stack) {
      continue;
    }
    added = min(remaining, max_stack - stack->count);
    stack->count += added;
    remaining -= added;
  }
  for (slot = 0; slot < INVENTORY_SIZE && remaining > 0; slot++) {
    ItemStack *stack = &stacks[slot];
    u8 added;

    if (stack->count != 0) {
      continue;
    }
    added = min(remaining, max_stack);
    stack->item = item;
    stack->count = added;
    remaining -= added;
  }
  return remaining == 0;
}

/* Applying a recipe to a temporary stack array makes both the availability
   label and the actual craft account for slots freed by consumed materials.
   A full pack can therefore still turn its last log into planks safely. */
static u8 applyRecipeToStacks(ItemStack *stacks,
    const CraftRecipe *recipe) {
  u8 ingredient;
  ItemStack before[INVENTORY_SIZE];

  for (ingredient = 0; ingredient < INVENTORY_SIZE; ingredient++) {
    before[ingredient] = stacks[ingredient];
  }
  for (ingredient = 0; ingredient < 2; ingredient++) {
    if (recipe->ingredient_count[ingredient] > 0 &&
        !removeItemFromStacks(stacks, recipe->ingredient_item[ingredient],
          recipe->ingredient_count[ingredient])) {
      u8 slot;
      for (slot = 0; slot < INVENTORY_SIZE; slot++) {
        stacks[slot] = before[slot];
      }
      return FALSE;
    }
  }
  if (!addItemToStacks(stacks, recipe->result_item,
      recipe->result_count)) {
    u8 slot;
    for (slot = 0; slot < INVENTORY_SIZE; slot++) {
      stacks[slot] = before[slot];
    }
    return FALSE;
  }
  return TRUE;
}

u8 recipeCraftableCount(Player *player, u8 recipe) {
  ItemStack simulated[INVENTORY_SIZE];
  u8 slot;
  u8 count = 0;

  if (recipe >= playerRecipeCount(player)) {
    return 0;
  }
  for (slot = 0; slot < INVENTORY_SIZE; slot++) {
    simulated[slot] = player->inventory[slot];
  }
  while (count < 99 &&
      applyRecipeToStacks(simulated, &craft_recipes[recipe])) {
    count++;
  }
  return count;
}

u8 craftSelectedRecipe(Player *player, u8 craft_all) {
  u8 recipe_count = playerRecipeCount(player);
  u8 made = 0;

  if (player->crafting_cursor >= recipe_count) {
    player->crafting_cursor = recipe_count - 1;
  }
  do {
    if (!applyRecipeToStacks(player->inventory,
        &craft_recipes[player->crafting_cursor])) {
      break;
    }
    made++;
  } while (craft_all && made < 99);
  refreshHeldItem(player);
  return made;
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

static void quickMoveInventoryStack(Player *player) {
  u8 source_index = player->inventory_cursor;
  ItemStack *source = &player->inventory[source_index];
  u8 first = source_index < INVENTORY_HOTBAR_START ?
    INVENTORY_HOTBAR_START : 0;
  u8 end = source_index < INVENTORY_HOTBAR_START ?
    INVENTORY_SIZE : INVENTORY_HOTBAR_START;
  u8 slot;

  if (source->count == 0) {
    return;
  }
  for (slot = first; slot < end && source->count > 0; slot++) {
    ItemStack *target = &player->inventory[slot];
    u8 moved;

    if (target->item != source->item ||
        target->count >= itemMaxStack(target->item)) {
      continue;
    }
    moved = min(source->count, itemMaxStack(target->item) - target->count);
    target->count += moved;
    source->count -= moved;
  }
  for (slot = first; slot < end && source->count > 0; slot++) {
    ItemStack *target = &player->inventory[slot];

    if (target->count == 0) {
      *target = *source;
      source->item = AIR;
      source->count = 0;
    }
  }
  if (source->count == 0) {
    source->item = AIR;
  }
  refreshHeldItem(player);
}

static void dropInventorySelection(Player *player, u8 drop_stack) {
  ItemStack *source = player->carried_item.count > 0 ?
    &player->carried_item : &player->inventory[player->inventory_cursor];
  u8 count;
  u8 x;
  u8 y;
  u8 z;

  if (source->count == 0) {
    return;
  }
  count = drop_stack ? source->count : 1;
  x = max(0, min(MAX_X - 1, floor(player->position.x / BLOCK_SIZE)));
  y = max(0, min(MAX_Y - 1,
    floor((player->position.y - EYE_HEIGHT * BLOCK_SIZE) / BLOCK_SIZE)));
  z = max(0, min(MAX_Z - 1, floor(player->position.z / BLOCK_SIZE)));
  if (!spawnDroppedItem(source->item, count, x, y, z)) {
    return;
  }
  source->count -= count;
  if (source->count == 0) {
    source->item = AIR;
  }
  refreshHeldItem(player);
}

static void spawnPlayer(Player *player, int x, int z) {
  int y;

  player->pitch = 0;
  player->yaw = 0;
  player->body_yaw = 0;
  player->walk_time = 0;
  player->walk_swing = 0;
  player->y_velocity = 0;
  player->fall_distance = 0;
  player->vault_time = 0;
  player->camera_y_offset = 0;
  player->knockback_velocity = (Vector3) {0, 0, 0};
  player->attack_time = 0;
  player->hurt_time = 0;
  player->objective_stage = 0;
  player->objective_time = 420.f;
  player->health = PLAYER_MAX_HEALTH;
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
    if (BLOCK_IS_SOLID(blockGet(x, y, z))) {
      player->position.y = (y + 1 + EYE_HEIGHT) * BLOCK_SIZE;
      return;
    }
  }

  player->position.y = (MAX_Y + EYE_HEIGHT) * BLOCK_SIZE;
}

/* Death returns an avatar to its join spawn without consuming its hard-won
   inventory.  The initial spawn routine remains responsible for new players. */
static void respawnPlayer(Player *player, int x, int z) {
  int y;

  player->position.x = (x + 0.5f) * BLOCK_SIZE;
  player->position.z = (z + 0.5f) * BLOCK_SIZE;
  for (y = MAX_Y - 1; y >= 0; y--) {
    if (BLOCK_IS_SOLID(blockGet(x, y, z))) {
      player->position.y = (y + 1 + EYE_HEIGHT) * BLOCK_SIZE;
      break;
    }
  }
  if (y < 0) {
    player->position.y = (MAX_Y + EYE_HEIGHT) * BLOCK_SIZE;
  }
  player->y_velocity = 0;
  player->fall_distance = 0;
  player->vault_time = 0;
  player->camera_y_offset = 0;
  player->knockback_velocity = (Vector3) {0, 0, 0};
  player->attack_time = 0;
  player->hurt_time = 0;
  player->objective_time = 180.f;
  player->health = PLAYER_MAX_HEALTH;
  player->breaking = FALSE;
  player->break_progress = 0;
}

void damagePlayer(u8 player_num, u8 damage, Vector3 source) {
  Player *player;
  float dx;
  float dz;
  float distance;

  if (player_num >= active_player_count) {
    return;
  }
  player = &players[player_num];
  if (!player->active || player->hurt_time > 0) {
    return;
  }
  player->health = player->health > damage ? player->health - damage : 0;
  player->hurt_time = PLAYER_ATTACK_DURATION;
  dx = player->position.x - source.x;
  dz = player->position.z - source.z;
  distance = sqrtf(dx * dx + dz * dz);
  if (distance > 1.f) {
    player->knockback_velocity.x = dx / distance * 10.f;
    player->knockback_velocity.z = dz / distance * 10.f;
  }
  player->y_velocity = max(player->y_velocity, 4.f);
  playSound(SOUND_PUNCH);
  if (player->health == 0) {
    respawnPlayer(player, START_X + player_num * 3,
      START_Z + (player_num & 1 ? 0 : 3));
  }
}

void initPlayers() {
  u8 player_num;

  active_player_count = 1;
  spawnPlayer(&players[0], START_X, START_Z);
  for (player_num = 1; player_num < MAX_PLAYERS; player_num++) {
    players[player_num].active = FALSE;
    players[player_num].target_present = FALSE;
    players[player_num].breaking = FALSE;
  }
}

void activatePlayer(u8 player_num) {
  if (player_num < MAX_PLAYERS && !players[player_num].active) {
    /* Nearby, staggered spawns avoid an immediate overlap without needing
       a second world copy or an expensive entity system. */
    spawnPlayer(&players[player_num], START_X + player_num * 3,
      START_Z + (player_num & 1 ? 0 : 3));
    active_player_count = player_num + 1;
    player_joined_number = player_num + 1;
    player_joined_message = 120;
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
  while (player->target_x >= MAX_X || player->target_y >= MAX_Y ||
      player->target_z >= MAX_Z ||
      !blockGet(player->target_x, player->target_y, player->target_z)) {
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
  if (override_axis >= 0) {
    *ati(&min_block, override_axis) = override_block;
    *ati(&max_block, override_axis) = override_block;
  }

  for (x = min_block.x; x <= max_block.x; x++) {
    for (y = min_block.y; y <= max_block.y; y++) {
      for (z = min_block.z; z <= max_block.z; z++) {
        if (x < 0 || y < 0 || z < 0 || x >= MAX_X || z >= MAX_Z ||
            (y < MAX_Y && BLOCK_IS_SOLID(blockGet(x, y, z)))) {
          return TRUE;
        }
      }
    }
  }
  return FALSE;
}

static u8 tryVault(Player *player, Vector3 velocity, float delta,
    u8 vault_button) {
  Vector3 candidate = player->position;
  Vector3 raised;

  if (!vault_button || player->vault_time > 0 ||
      (velocity.x == 0 && velocity.z == 0)) {
    return FALSE;
  }
  candidate.x += velocity.x * delta * 1.35f;
  candidate.z += velocity.z * delta * 1.35f;
  if (!boxObstructed(div(candidate, BLOCK_SIZE), -1, 0)) {
    return FALSE;
  }

  /* Both the destination and the vertical lift must fit the full player box.
     This prevents a mantle from entering a low ceiling or vaulting a wall
     taller than one block. */
  raised = candidate;
  raised.y += BLOCK_SIZE + 2.f;
  if (boxObstructed(div(raised, BLOCK_SIZE), -1, 0)) {
    return FALSE;
  }
  raised = player->position;
  raised.y += BLOCK_SIZE + 2.f;
  if (boxObstructed(div(raised, BLOCK_SIZE), -1, 0)) {
    return FALSE;
  }

  player->position.y += BLOCK_SIZE + 2.f;
  player->camera_y_offset -= BLOCK_SIZE;
  player->vault_time = PLAYER_VAULT_DURATION;
  player->y_velocity = 2.f;
  return TRUE;
}

static u8 consumeInventoryItem(Player *player, u8 item) {
  u8 slot;

  for (slot = 0; slot < INVENTORY_SIZE; slot++) {
    ItemStack *stack = &player->inventory[slot];
    if (stack->item == item && stack->count > 0) {
      stack->count--;
      if (stack->count == 0) {
        stack->item = AIR;
      }
      return TRUE;
    }
  }
  return FALSE;
}

static void landPlayer(Player *player) {
  if (player->fall_distance > BLOCK_SIZE * 3.f) {
    if (consumeInventoryItem(player, SLIME_GEL)) {
      u8 player_num = player - players;
      pickup_item[player_num] = SLIME_GEL;
      pickup_message[player_num] = 60;
      playSound(SOUND_PICKUP);
      player->fall_distance = 0;
      return;
    }
    u8 damage = (player->fall_distance / BLOCK_SIZE - 2.f) * 2.f;
    if (damage > 0) {
      player->health = player->health > damage ? player->health - damage : 0;
      player->hurt_time = PLAYER_ATTACK_DURATION;
      playSound(SOUND_PUNCH);
      if (player->health == 0) {
        u8 player_num = player - players;
        respawnPlayer(player, START_X + player_num * 3,
          START_Z + (player_num & 1 ? 0 : 3));
      }
    }
  }
  player->fall_distance = 0;
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

  if (x >= MAX_X || y >= MAX_Y || z >= MAX_Z) {
    return;
  }

  if (player->held_block == SAPLING) {
    if (held_stack->item == SAPLING && held_stack->count > 0 &&
        tryPlantTree(x, y, z)) {
      held_stack->count--;
      /*
       * A planted tree writes its trunk and five-by-five canopy in one step.
       * Mark every touched horizontal coordinate so canopies spanning chunk
       * boundaries become visible as soon as their columns are rebuilt.
       */
      for (bx = max((int) x - 2, 0); bx < min((int) x + 3, MAX_X); bx++) {
        for (bz = max((int) z - 2, 0); bz < min((int) z + 3, MAX_Z); bz++) {
          makeDisplayListsAt(bx, bz);
        }
      }
      playSound(SOUND_PLACE);
    }
    return;
  }

  if (player->held_block < FIRST_PLACEABLE_BLOCK ||
      player->held_block > BLOCK_TYPE_COUNT) {
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

  blockSet(x, y, z, player->held_block);
  if (blockUsesInventory(player->held_block)) {
    held_stack->count--;
  }
  makeDisplayListsAt(x, z);
  playSound(SOUND_PLACE);
}

static u8 dropForBlock(u8 block, u8 tool, u8 *item) {
  /* Rock is deliberately breakable by hand, but only a pickaxe harvests a
     cube.  The remaining current terrain is soft or wooden enough to gather
     with the tools the game already offers. */
  if (block == BEDROCK) {
    return FALSE;
  }
  if ((block == STONE || block == COBBLESTONE || block == BRICKS ||
      block == MOSSY_COBBLESTONE || block == COAL_ORE) &&
      !itemIsPickaxe(tool)) {
    return FALSE;
  }
  if (block == IRON_ORE &&
      tool != STONE_PICKAXE && tool != IRON_PICKAXE) {
    return FALSE;
  }
  if (block == LEAVES) {
    return rollLeafDrop(item);
  }
  if (block == COAL_ORE) {
    *item = COAL;
    return TRUE;
  }
  if (block == IRON_ORE) {
    *item = IRON_CHUNK;
    return TRUE;
  }
  if (block == MOSSY_COBBLESTONE) {
    *item = COBBLESTONE;
    return TRUE;
  }
  *item = block;
  return TRUE;
}

static u8 breakBlock(u8 x, u8 y, u8 z, u8 tool) {
  u8 block = blockGet(x, y, z);
  u8 item;

  if (block == WOOD && beginTreeFelling(x, y, z)) {
    playSound(SOUND_BREAK);
    return TRUE;
  }

  /* Every terrain block that has a valid harvest yields the same small cube
     that can later be placed.  If the item pool is full, preserve the block
     rather than silently destroying its resource. */
  if (dropForBlock(block, tool, &item) &&
      !spawnDroppedItem(item, 1, x, y, z)) {
    return FALSE;
  }
  blockSet(x, y, z, AIR);
  treeBlockDestroyed(x, y, z);
  makeDisplayListsAt(x, z);
  playSound(SOUND_BREAK);
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

/* Avatars already carry all of the state a first PvP pass needs.  Keep the
   sword hit test intentionally short and forward-facing so mining retains its
   B-button behavior unless another player is actually within reach. */
static Player *swordTarget(u8 attacker_num) {
  Player *attacker = &players[attacker_num];
  Vector3 forward = {0, 0, -1};
  Player *target = NULL;
  float nearest = 999999.f;
  u8 player_num;

  forward = rotateY(forward, -attacker->yaw);
  for (player_num = 0; player_num < active_player_count; player_num++) {
    Player *candidate = &players[player_num];
    float dx;
    float dy;
    float dz;
    float horizontal_distance;
    float facing;

    if (player_num == attacker_num || !candidate->active) {
      continue;
    }
    dx = candidate->position.x - attacker->position.x;
    dy = candidate->position.y - attacker->position.y;
    dz = candidate->position.z - attacker->position.z;
    horizontal_distance = sqrtf(dx * dx + dz * dz);
    if (horizontal_distance < 1.f || horizontal_distance > 100.f ||
        dy < -88.f || dy > 48.f) {
      continue;
    }
    facing = (forward.x * dx + forward.z * dz) / horizontal_distance;
    if (facing < .35f || horizontal_distance >= nearest) {
      continue;
    }
    nearest = horizontal_distance;
    target = candidate;
  }
  return target;
}

static u8 swingSword(u8 attacker_num) {
  Player *attacker = &players[attacker_num];
  Player *target;
  float dx;
  float dz;
  float horizontal_distance;

  if (!itemIsSword(heldItem(attacker)) || attacker->attack_time > 0) {
    return FALSE;
  }
  target = swordTarget(attacker_num);
  if (target == NULL) {
    return FALSE;
  }

  attacker->attack_time = PLAYER_ATTACK_DURATION;
  {
    u8 damage = heldItem(attacker) == IRON_SWORD ? 8 :
      (heldItem(attacker) == STONE_SWORD ? 6 : 4);
    target->health = target->health > damage ? target->health - damage : 0;
  }
  target->hurt_time = PLAYER_ATTACK_DURATION;
  dx = target->position.x - attacker->position.x;
  dz = target->position.z - attacker->position.z;
  horizontal_distance = sqrtf(dx * dx + dz * dz);
  target->knockback_velocity.x = dx / horizontal_distance * 13.f;
  target->knockback_velocity.z = dz / horizontal_distance * 13.f;
  target->y_velocity = max(target->y_velocity, 5.f);
  playSound(SOUND_PUNCH);

  if (target->health == 0) {
    u8 target_num = target - players;
    respawnPlayer(target, START_X + target_num * 3,
      START_Z + (target_num & 1 ? 0 : 3));
  }
  return TRUE;
}

static float blockBreakTime(u8 block, u8 tool) {
  u8 pickaxe = itemIsPickaxe(tool);
  u8 axe = itemIsAxe(tool);

  switch (block) {
    case LEAVES:
      return itemIsSword(tool) ? 5.f : 12.f;
    case DIRT:
    case GRASS:
    case SAND:
      return 22.f;
    case WOOD:
    case PLANKS:
    case CRAFTING_TABLE:
      return axe ? (tool == IRON_AXE ? 8.f :
        (tool == STONE_AXE ? 12.f : 20.f)) : 36.f;
    case STONE:
    case COBBLESTONE:
    case BRICKS:
    case MOSSY_COBBLESTONE:
    case COAL_ORE:
      return pickaxe ? (tool == IRON_PICKAXE ? 12.f :
        (tool == STONE_PICKAXE ? 18.f : 32.f)) : 120.f;
    case IRON_ORE:
      return tool == IRON_PICKAXE ? 14.f : (tool == STONE_PICKAXE ? 24.f :
        (tool == WOOD_PICKAXE ? 80.f : 140.f));
    case BEDROCK:
      return 0;
    default:
      return 0;
  }
}

static u8 consumeHeldFood(Player *player) {
  ItemStack *held = &player->inventory[INVENTORY_HOTBAR_START +
    player->selected_hotbar_slot];
  u8 healing;

  if (held->count == 0 || player->health >= PLAYER_MAX_HEALTH) {
    return FALSE;
  }
  healing = held->item == APPLE ? 4 :
    ((held->item == RAW_MUTTON || held->item == RAW_PORK) ? 3 : 0);
  if (healing == 0) {
    return FALSE;
  }
  player->health = min(PLAYER_MAX_HEALTH, player->health + healing);
  held->count--;
  if (held->count == 0) {
    held->item = AIR;
  }
  playSound(SOUND_PICKUP);
  return TRUE;
}

static u8 inventoryHas(Player *player, u8 item) {
  u8 slot;
  for (slot = 0; slot < INVENTORY_SIZE; slot++) {
    if (player->inventory[slot].item == item &&
        player->inventory[slot].count > 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static u8 inventoryHasToolClass(Player *player, u8 tool_class) {
  u8 slot;
  for (slot = 0; slot < INVENTORY_SIZE; slot++) {
    u8 item = player->inventory[slot].count > 0 ?
      player->inventory[slot].item : AIR;
    if ((tool_class == 0 && itemIsPickaxe(item)) ||
        (tool_class == 1 &&
         (item == STONE_PICKAXE || item == STONE_SWORD ||
          item == STONE_AXE || item == IRON_PICKAXE ||
          item == IRON_SWORD || item == IRON_AXE))) {
      return TRUE;
    }
  }
  return FALSE;
}

static u8 objectiveComplete(Player *player) {
  switch (player->objective_stage) {
    case 0:
      return inventoryHas(player, WOOD);
    case 1:
      return inventoryHas(player, PLANKS);
    case 2:
      return inventoryHas(player, CRAFTING_TABLE) ||
        inventoryHasToolClass(player, 0);
    case 3:
      return inventoryHasToolClass(player, 0);
    case 4:
      return inventoryHas(player, COBBLESTONE) ||
        inventoryHasToolClass(player, 1) || inventoryHas(player, COAL);
    case 5:
      return inventoryHasToolClass(player, 1);
    case 6:
      return inventoryHas(player, COAL);
    case 7:
      return inventoryHas(player, IRON_SWORD) ||
        inventoryHas(player, IRON_PICKAXE) || inventoryHas(player, IRON_AXE);
    default:
      return FALSE;
  }
}

static void updatePlayerObjective(Player *player, float delta) {
  player->objective_time = max(0, player->objective_time - delta);
  while (player->objective_stage < PLAYER_OBJECTIVE_COUNT &&
      objectiveComplete(player)) {
    player->objective_stage++;
    player->objective_time = 300.f;
  }
}

const char *playerObjectiveTitle(Player *player) {
  static const char *titles[] = {
    "GATHER WOOD", "MAKE PLANKS", "BUILD A TABLE", "CRAFT A PICKAXE",
    "MINE COBBLESTONE", "FORGE STONE TOOLS", "FIND COAL",
    "FORGE IRON TOOLS", "EXPLORE"
  };
  return titles[player->objective_stage < PLAYER_OBJECTIVE_COUNT ?
    player->objective_stage : PLAYER_OBJECTIVE_COUNT];
}

const char *playerObjectiveHint(Player *player) {
  static const char *hints[] = {
    "FELL A TREE", "CRAFT FROM A LOG", "USE FOUR PLANKS",
    "USE A CRAFT TABLE", "DIG STONE", "CRAFT WITH COBBLE",
    "SEARCH BELOW", "MINE DEEPER", "THE WORLD IS YOURS"
  };
  return hints[player->objective_stage < PLAYER_OBJECTIVE_COUNT ?
    player->objective_stage : PLAYER_OBJECTIVE_COUNT];
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

  block = blockGet(player->target_x, player->target_y, player->target_z);
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
    playSound(SOUND_PUNCH);
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

static void openInventory(u8 player_num) {
  Player *player = &players[player_num];

  /* Older saves may contain materials in the former manual crafting matrix.
     Recover them before showing the recipe-driven interface. */
  returnCraftingItems(player);
  player->crafting_table_open = player->target_present &&
    blockGet(player->target_x, player->target_y,
      player->target_z) == CRAFTING_TABLE;
  player->inventory_area = INVENTORY_AREA_ITEMS;
  if (player->crafting_cursor >= playerRecipeCount(player)) {
    player->crafting_cursor = playerRecipeCount(player) - 1;
  }
  inventory_player = player_num;
  resetInventoryNavigation();
  current_screen = INVENTORY;
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
          (y < MAX_Y && BLOCK_IS_SOLID(blockGet(x, y, z)))) {
        return TRUE;
      }
    }
  }
  return FALSE;
}

static u8 playerInWater(Player *player) {
  int x = floor(player->position.x / BLOCK_SIZE);
  int y = floor(player->position.y / BLOCK_SIZE);
  int z = floor(player->position.z / BLOCK_SIZE);
  int scan_y;

  if (x < 0 || z < 0 || x >= MAX_X || z >= MAX_Z) {
    return FALSE;
  }
  /* Position is at eye height, so inspect the body volume beneath it rather
   * than only the block containing the camera. */
  for (scan_y = max(0, y - 2); scan_y <= min(MAX_Y - 1, y); scan_y++) {
    if (blockGet(x, scan_y, z) == WATER) {
      return TRUE;
    }
  }
  return FALSE;
}

static u8 updatePlayer(u8 player_num, float delta) {
  Player *player = &players[player_num];
  NUContData *cont = &cont_data[player_num];
  Vector3 velocity = {0, 0, 0};
  float t, t_total = 0;
  float move_t;
  int collision_axis = 0;
  s8 stick_x = cont->stick_x;
  s8 stick_y = cont->stick_y;
  u8 swimming = playerInWater(player);
  u8 grounded;
  u8 vaulted = FALSE;

  if (cont->trigger & U_CBUTTONS) {
    player->camera_mode = player->camera_mode == CAMERA_FIRST_PERSON ?
      CAMERA_THIRD_PERSON : CAMERA_FIRST_PERSON;
  }
  if (cont->trigger & D_CBUTTONS) {
    openInventory(player_num);
    return TRUE;
  }

  if (stick_x > -4 && stick_x < 4) stick_x = 0;
  if (stick_y > -4 && stick_y < 4) stick_y = 0;

  player->attack_time = max(0, player->attack_time - delta);
  player->hurt_time = max(0, player->hurt_time - delta);
  player->vault_time = max(0, player->vault_time - delta);
  player->camera_y_offset *= max(0, 1.f - delta * .24f);
  if (player->camera_y_offset > -.25f && player->camera_y_offset < .25f) {
    player->camera_y_offset = 0;
  }

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
    if (swimming) {
      velocity.x *= 0.55f;
      velocity.z *= 0.55f;
    }
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

  velocity = add(velocity, player->knockback_velocity);
  player->knockback_velocity = mul(player->knockback_velocity,
    max(0, 1.f - delta * .16f));

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

  grounded = onGround(player);
  if (!swimming && grounded && (cont->button & L_TRIG) &&
      (cont->button & R_TRIG)) {
    vaulted = tryVault(player, velocity, delta, TRUE);
  }

  if (swimming) {
    player->fall_distance = 0;
    if (cont->button & R_TRIG) {
      player->y_velocity = JUMP_SPEED * 0.45f;
    } else if (player->y_velocity > -BLOCK_SIZE / 10.f) {
      player->y_velocity -= GRAVITY * delta * 0.18f;
    }
  } else if (vaulted) {
    /* tryVault already supplied a small upward carry. */
  } else if (grounded) {
    player->y_velocity = (cont->button & R_TRIG) ? JUMP_SPEED : 0;
  } else if (player->y_velocity > -TERMINAL_SPEED) {
    player->y_velocity -= GRAVITY * delta;
  }
  if (!swimming && player->y_velocity < 0) {
    player->fall_distance += -player->y_velocity * delta;
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
      if (collision_axis == 1) {
        if (player->y_velocity < 0) {
          landPlayer(player);
        }
        player->y_velocity = 0;
      }
    }
  }

  updateTargetBlock(player_num);
  if (cont->trigger & A_BUTTON) {
    if (player->target_present &&
        blockGet(player->target_x, player->target_y,
          player->target_z) == CRAFTING_TABLE) {
      openInventory(player_num);
      return TRUE;
    }
    if (consumeHeldFood(player)) {
      return FALSE;
    }
    if (player->target_present) {
      placeBlock(player_num, player->build_offset_x + player->target_x,
        player->build_offset_y + player->target_y,
        player->build_offset_z + player->target_z);
    }
  }
  if (cont->trigger & B_BUTTON) {
    if (punchMob(player_num) || swingSword(player_num)) {
      resetBreaking(player);
    } else {
      /* A tap should always read as a punch, even if it hits air or starts
         mining.  Continuous mining has its own rhythmic arm motion. */
      player->attack_time = PLAYER_ATTACK_DURATION;
      updateBreaking(player_num, delta);
    }
  } else {
    updateBreaking(player_num, delta);
  }
  return FALSE;
}

void updatePlayers() {
  OSTime time;
  float delta;
  u16 down_pressed, up_pressed, act_pressed;
  u8 i;

  /* NuSystem snapshots all four pads together and derives each pad's trigger
     state from that shared sample. */
  nuContDataGetExAll(cont_data);
  time = osGetTime();
  delta = last_time == 0 ? 1.f :
    OS_CYCLES_TO_USEC(time - last_time) * 60 / 1000000.f;
  last_time = time;
  if (delta > MAX_FRAME_DELTA) {
    delta = MAX_FRAME_DELTA;
  }
  if (current_screen == GAME) {
    for (i = 0; i < active_player_count; i++) {
      updatePlayerObjective(&players[i], delta);
    }
  }

  if (current_screen == INVENTORY) {
    Player *player = &players[inventory_player];
    NUContData *inventory_cont = &cont_data[inventory_player];
    u8 navigation = inventoryNavigation(inventory_cont);
    u8 row = player->inventory_cursor / INVENTORY_COLUMNS;
    u8 column = player->inventory_cursor % INVENTORY_COLUMNS;

    if (inventory_cont->trigger & (START_BUTTON | B_BUTTON)) {
      returnCraftingItems(player);
      resetInventoryNavigation();
      current_screen = GAME;
      return;
    }
    if (inventory_cont->trigger & A_BUTTON) {
      if (player->inventory_area == INVENTORY_AREA_OUTPUT) {
        craftSelectedRecipe(player, FALSE);
      } else {
        swapItemStacks(&player->inventory[player->inventory_cursor], &player->carried_item);
        refreshHeldItem(player);
      }
      return;
    }
    if (player->inventory_area == INVENTORY_AREA_OUTPUT) {
      if (inventory_cont->trigger & U_CBUTTONS) {
        craftSelectedRecipe(player, TRUE);
        return;
      }
    } else {
      if (inventory_cont->trigger & L_CBUTTONS) {
        moveOneItem(&player->inventory[player->inventory_cursor],
          &player->carried_item);
        refreshHeldItem(player);
        return;
      }
      if (inventory_cont->trigger & R_CBUTTONS) {
        quickMoveInventoryStack(player);
        return;
      }
      if (inventory_cont->trigger & U_CBUTTONS) {
        dropInventorySelection(player, TRUE);
        return;
      }
      if (inventory_cont->trigger & D_CBUTTONS) {
        dropInventorySelection(player, FALSE);
        refreshHeldItem(player);
        return;
      }
    }

    if (player->inventory_area == INVENTORY_AREA_OUTPUT) {
      if (navigation & NAV_LEFT) {
        player->inventory_area = INVENTORY_AREA_ITEMS;
      } else if (navigation & NAV_UP) {
        player->crafting_cursor = player->crafting_cursor == 0 ?
          playerRecipeCount(player) - 1 : player->crafting_cursor - 1;
      } else if (navigation & NAV_DOWN) {
        player->crafting_cursor =
          (player->crafting_cursor + 1) % playerRecipeCount(player);
      }
      return;
    }

    if (navigation & NAV_LEFT) {
      if (column > 0) {
        column--;
      }
    } else if (navigation & NAV_RIGHT) {
      if (column == INVENTORY_COLUMNS - 1) {
        player->inventory_area = INVENTORY_AREA_OUTPUT;
        return;
      }
      column++;
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

  if (current_screen == WORLD_NAMING) {
    u8 name_navigation = inventoryNavigation(&cont_data[0]);

    /* The stick picks a key while C-left/right retain the useful editing
       function of moving the insertion point. */
    if (cont_data[0].trigger & START_BUTTON) {
      confirmWorldName();
    } else if ((name_navigation & NAV_LEFT) ||
        (cont_data[0].trigger & L_JPAD)) {
      worldNameKeyboardLeft();
    } else if ((name_navigation & NAV_RIGHT) ||
        (cont_data[0].trigger & R_JPAD)) {
      worldNameKeyboardRight();
    } else if ((name_navigation & NAV_UP) ||
        (cont_data[0].trigger & U_JPAD)) {
      worldNameKeyboardUp();
    } else if ((name_navigation & NAV_DOWN) ||
        (cont_data[0].trigger & D_JPAD)) {
      worldNameKeyboardDown();
    } else if (cont_data[0].trigger & L_CBUTTONS) {
      worldNameCursorLeft();
    } else if (cont_data[0].trigger & R_CBUTTONS) {
      worldNameCursorRight();
    } else if (cont_data[0].trigger & A_BUTTON) {
      worldNameInsertCharacter();
    } else if (cont_data[0].trigger & B_BUTTON) {
      worldNameErase();
    }
    return;
  }

  if (current_screen == LOADING_PREVIEW) {
    /* The flyover is deliberately non-interactive; do not let a held menu
       button select another world while the current one is becoming ready. */
    return;
  }

  if (current_screen != GAME) {
    if (current_screen == MENU) {
      /* Bisect switches.  Each removes one suspect so the fault can be
         cornered without a rebuild per hypothesis. */
      if (cont_data[0].trigger & L_CBUTTONS) {
        diag_draw_terrain = !diag_draw_terrain;
      }
      if (cont_data[0].trigger & R_CBUTTONS) {
        diag_refine_seams = !diag_refine_seams;
      }
      if (cont_data[0].trigger & U_CBUTTONS) {
        diag_column_cap = diag_column_cap > 8 ? diag_column_cap - 16 : 8;
      }
      if (cont_data[0].trigger & D_CBUTTONS) {
        diag_column_cap = diag_column_cap < 96 ? diag_column_cap + 16 : 96;
      }
    }
    down_pressed = cont_data[0].stick_y < -50;
    up_pressed = cont_data[0].stick_y > 50;
    act_pressed = cont_data[0].button & (START_BUTTON | A_BUTTON | B_BUTTON);
    if (down_pressed && !down_held) menuDown();
    if (up_pressed && !up_held) menuUp();
    if (act_pressed && !act_held) menuAct();
    down_held = down_pressed;
    up_held = up_pressed;
    act_held = act_pressed;
    return;
  }

  /* Players join in port order, keeping the active range contiguous for the
     renderer, saves, and every per-player simulation loop. */
  if (active_player_count < MAX_PLAYERS &&
      (cont_data[active_player_count].trigger & START_BUTTON)) {
    activatePlayer(active_player_count);
    return;
  }
  for (i = 0; i < active_player_count; i++) {
    if (cont_data[i].trigger & START_BUTTON) {
      openInventory(i);
      return;
    }
  }
  for (i = 0; i < active_player_count; i++) {
    if (updatePlayer(i, delta)) {
      return;
    }
  }
  updateTrees(delta);
  updateDroppedItems(delta);
  updateMobs(delta);

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
