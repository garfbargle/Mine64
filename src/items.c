#include "items.h"
#include "blocks.h"
#include "graphics.h"
#include "math.h"
#include "player.h"
#include "world.h"

#define ITEM_RADIUS 10.f
#define ITEM_GRAVITY 0.45f
#define ITEM_PICKUP_RADIUS 48.f
#define ITEM_PICKUP_HEIGHT 96.f
#define ITEM_PICKUP_DELAY 18

DroppedItem dropped_items[MAX_DROPPED_ITEMS];

void initDroppedItems() {
  u8 i;

  for (i = 0; i < MAX_DROPPED_ITEMS; i++) {
    dropped_items[i].active = FALSE;
  }
}

void spawnDroppedItem(u8 item, u8 count, u8 x, u8 y, u8 z) {
  u8 i;
  DroppedItem *drop = NULL;

  for (i = 0; i < MAX_DROPPED_ITEMS; i++) {
    if (!dropped_items[i].active) {
      drop = &dropped_items[i];
      break;
    }
  }
  if (drop == NULL) {
    return;
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
  drop->active = TRUE;
}

static u8 solidBlockAt(float x, float y, float z) {
  int bx = floor(x / BLOCK_SIZE);
  int by = floor(y / BLOCK_SIZE);
  int bz = floor(z / BLOCK_SIZE);

  if (bx < 0 || bz < 0 || bx >= MAX_X || bz >= MAX_Z || by < 0) {
    return TRUE;
  }
  if (by >= MAX_Y) {
    return FALSE;
  }
  return blocks[bx * MAX_Y * MAX_Z + by * MAX_Z + bz] != AIR;
}

static void updateDropPhysics(DroppedItem *drop, float delta) {
  drop->position = add(drop->position, mul(drop->velocity, delta));
  drop->velocity.y -= ITEM_GRAVITY * delta;
  drop->rotation += delta * 6;
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

static void tryPickup(DroppedItem *drop) {
  u8 player_num;

  if (drop->pickup_delay > 0) {
    drop->pickup_delay--;
    return;
  }

  for (player_num = 0; player_num < active_player_count; player_num++) {
    Player *player = &players[player_num];
    float dx = player->position.x - drop->position.x;
    float dy = player->position.y - drop->position.y;
    float dz = player->position.z - drop->position.z;

    /* Player positions are at eye height, so use a short horizontal radius
       and a taller vertical range that reaches the ground below their feet. */
    if (dx * dx + dz * dz <= ITEM_PICKUP_RADIUS * ITEM_PICKUP_RADIUS &&
        dy >= -ITEM_PICKUP_HEIGHT && dy <= ITEM_PICKUP_HEIGHT) {
      u8 added = addItemToInventory(player, drop->item, drop->count);
      drop->count -= added;
      if (drop->count == 0) {
        drop->active = FALSE;
      }
      return;
    }
  }
}

void updateDroppedItems(float delta) {
  u8 i;

  for (i = 0; i < MAX_DROPPED_ITEMS; i++) {
    if (dropped_items[i].active) {
      updateDropPhysics(&dropped_items[i], delta);
      tryPickup(&dropped_items[i]);
    }
  }
}
