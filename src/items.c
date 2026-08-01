#include "items.h"
#include "blocks.h"
#include "graphics.h"
#include "math.h"
#include "player.h"
#include "world.h"
#include "audio.h"
#include "details.h"

#define ITEM_RADIUS 14.f
#define ITEM_GRAVITY 0.45f
#define ITEM_TERMINAL_SPEED 12.f
#define ITEM_PICKUP_RADIUS 90.f
#define ITEM_PICKUP_HEIGHT 160.f
#define ITEM_PICKUP_DELAY 12.f
#define ITEM_PICKUP_PULL_TIME 12.f
#define ITEM_IDLE_ROTATION_SPEED 2.f
#define ITEM_NO_PICKUP_PLAYER MAX_PLAYERS

DroppedItem dropped_items[MAX_DROPPED_ITEMS];
u8 pickup_message[MAX_PLAYERS];
u8 pickup_item[MAX_PLAYERS];

u8 itemMaxStack(u8 item) {
  return itemIsTool(item) ? 1 : MAX_ITEM_STACK;
}

const char *itemName(u8 item) {
  static const char *names[] = {
    "Empty", "Dirt", "Stone", "Grass", "Cobblestone", "Sand", "Log",
    "Leaves", "Planks", "Bricks", "Crafting Table", "Stick",
    "Wood Sword", "Wood Pickaxe", "Sapling", "Wool", "Coal",
    "Iron Chunk", "Stone Sword", "Stone Pickaxe", "Wood Axe",
    "Stone Axe", "Apple", "Raw Mutton", "Raw Pork", "Slime Gel",
    "Iron Sword", "Iron Pickaxe", "Iron Axe", "Torch", "Wood Stairs",
    "Stone Stairs", "Wood Door", "Lattice Window", "Raw Chicken",
    "Feather", "Fence", "Fence Gate"
  };

  if (item <= CRAFTING_TABLE) {
    return names[item];
  }
  return ITEM_IS_VALID(item) ? names[item - 5] : "Unknown";
}

u8 itemIsSword(u8 item) {
  return item == WOOD_SWORD || item == STONE_SWORD || item == IRON_SWORD;
}

u8 itemIsPickaxe(u8 item) {
  return item == WOOD_PICKAXE || item == STONE_PICKAXE ||
    item == IRON_PICKAXE;
}

u8 itemIsAxe(u8 item) {
  return item == WOOD_AXE || item == STONE_AXE || item == IRON_AXE;
}

u8 itemIsTool(u8 item) {
  return itemIsSword(item) || itemIsPickaxe(item) || itemIsAxe(item);
}

/* Two ranges rather than one, because the detail items are not contiguous and
   deliberately were not made so: storage.c writes raw item IDs into save
   files, so renumbering TORCH..GLASS_WINDOW to make room would silently
   reinterpret every existing world's inventory. */
u8 itemIsDetail(u8 item) {
  return (item >= TORCH && item <= GLASS_WINDOW) ||
    (item >= FENCE && item <= FENCE_GATE);
}

u8 rollLeafDrop(u8 *item) {
  /* A leaf produces one outcome: saplings are rare, and the remainder has an
     even chance to survive as a leaf cube.  This keeps a small felled tree
     from flooding the 16-slot pickup pool. */
  if (random(10) == 0) {
    *item = SAPLING;
    return TRUE;
  }
  if (random(24) == 0) {
    *item = APPLE;
    return TRUE;
  }
  if (random(2) == 0) {
    *item = LEAVES;
    return TRUE;
  }
  return FALSE;
}

void initDroppedItems() {
  u8 i;

  for (i = 0; i < MAX_DROPPED_ITEMS; i++) {
    dropped_items[i].active = FALSE;
  }
  for (i = 0; i < MAX_PLAYERS; i++) {
    pickup_message[i] = 0;
    pickup_item[i] = AIR;
  }
}

u8 spawnDroppedItem(u8 item, u8 count, int x, u8 y, int z) {
  u8 i;
  DroppedItem *drop = NULL;

  for (i = 0; i < MAX_DROPPED_ITEMS; i++) {
    if (!dropped_items[i].active) {
      drop = &dropped_items[i];
      break;
    }
  }
  if (drop == NULL) {
    return FALSE;
  }

  drop->position.x = (x + 0.5f) * BLOCK_SIZE;
  drop->position.y = (y + 0.5f) * BLOCK_SIZE;
  drop->position.z = (z + 0.5f) * BLOCK_SIZE;
  drop->velocity.x = ((int) random(17) - 8) / 4.f;
  drop->velocity.y = 7.f;
  drop->velocity.z = ((int) random(17) - 8) / 4.f;
  drop->rotation = random(360);
  drop->item = item;
  drop->count = count;
  drop->pickup_delay = ITEM_PICKUP_DELAY;
  drop->pickup_progress = 0;
  drop->pickup_player = ITEM_NO_PICKUP_PLAYER;
  drop->active = TRUE;
  return TRUE;
}

static u8 solidBlockAt(float x, float y, float z) {
  int bx = floor(x / BLOCK_SIZE);
  int by = floor(y / BLOCK_SIZE);
  int bz = floor(z / BLOCK_SIZE);

  /* Unloaded terrain reads as solid, so a drop cannot fall through a column
     that has not streamed in yet. */
  if (by < 0) {
    return TRUE;
  }
  if (by >= MAX_Y) {
    return FALSE;
  }
  return worldCellSolid(bx, by, bz);
}

static void updateDropPhysics(DroppedItem *drop, float delta) {
  drop->position = add(drop->position, mul(drop->velocity, delta));
  drop->velocity.y -= ITEM_GRAVITY * delta;
  if (drop->velocity.y < -ITEM_TERMINAL_SPEED) {
    drop->velocity.y = -ITEM_TERMINAL_SPEED;
  }
  drop->rotation += delta * ITEM_IDLE_ROTATION_SPEED;
  if (drop->rotation >= 360) {
    drop->rotation -= 360;
  }

  if (drop->velocity.y <= 0 && solidBlockAt(drop->position.x,
      drop->position.y - ITEM_RADIUS, drop->position.z)) {
    int ground_y = floor((drop->position.y - ITEM_RADIUS) / BLOCK_SIZE);
    drop->position.y = (ground_y + 1) * BLOCK_SIZE + ITEM_RADIUS;
    drop->velocity.y = 0;
    drop->velocity.x *= 0.8f;
    drop->velocity.z *= 0.8f;
  }
}

static void startPickup(DroppedItem *drop) {
  u8 player_num;

  if (drop->pickup_delay > 0) {
    return;
  }

  for (player_num = 0; player_num < active_player_count; player_num++) {
    Player *player = &players[player_num];
    float dx = player->position.x - drop->position.x;
    float dy = player->position.y - drop->position.y;
    float dz = player->position.z - drop->position.z;

    /* Player positions are at eye height.  A little more than one block of
       horizontal reach makes collection forgiving without vacuuming items
       across the screen; the taller range covers slopes and tree canopies. */
    if (dx * dx + dz * dz <= ITEM_PICKUP_RADIUS * ITEM_PICKUP_RADIUS &&
        dy >= -ITEM_PICKUP_HEIGHT && dy <= ITEM_PICKUP_HEIGHT) {
      drop->pickup_player = player_num;
      drop->pickup_progress = 0;
      drop->velocity.x = 0;
      drop->velocity.y = 0;
      drop->velocity.z = 0;
      return;
    }
  }
}

static void updatePickupAnimation(DroppedItem *drop, float delta) {
  Player *player = &players[drop->pickup_player];
  Vector3 target = player->position;
  float pull = min(1.f, delta * 0.32f);
  u8 added;

  /* Aim below the eye so the cube visibly converges on the player rather
     than clipping into the camera.  The exponential pull looks smooth even
     when a frame takes longer than normal. */
  target.y -= BLOCK_SIZE * 0.45f;
  drop->position.x += (target.x - drop->position.x) * pull;
  drop->position.y += (target.y - drop->position.y) * pull;
  drop->position.z += (target.z - drop->position.z) * pull;
  drop->rotation += delta * 30.f;
  if (drop->rotation >= 360) {
    drop->rotation -= 360;
  }
  drop->pickup_progress += delta;

  if (drop->pickup_progress < ITEM_PICKUP_PULL_TIME) {
    return;
  }

  added = addItemToInventory(player, drop->item, drop->count);
  drop->count -= added;
  if (added > 0) {
    pickup_item[drop->pickup_player] = drop->item;
    pickup_message[drop->pickup_player] = 60;
    playSound(SOUND_PICKUP);
  }
  if (drop->count == 0) {
    drop->active = FALSE;
  } else {
    /* An inventory can change while the animation plays.  Leave any
       remainder in the world and make it eligible for another player. */
    drop->pickup_player = ITEM_NO_PICKUP_PLAYER;
    drop->pickup_progress = 0;
  }
}

void updateDroppedItems(float delta) {
  u8 i;

  for (i = 0; i < active_player_count; i++) {
    if (pickup_message[i] > 0) {
      pickup_message[i]--;
    }
  }
  for (i = 0; i < MAX_DROPPED_ITEMS; i++) {
    if (dropped_items[i].active) {
      DroppedItem *drop = &dropped_items[i];

      if (drop->pickup_player < active_player_count) {
        updatePickupAnimation(drop, delta);
      } else {
        updateDropPhysics(drop, delta);
        drop->pickup_delay = max(0, drop->pickup_delay - delta);
        startPickup(drop);
      }
    }
  }
}
