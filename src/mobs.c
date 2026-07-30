#include "mobs.h"

#include "audio.h"
#include "blocks.h"
#include "graphics.h"
#include "items.h"
#include "player.h"
#include "world.h"

#define SHEEP_WALK_SPEED 0.9f
#define SHEEP_TURN_MIN 50.f
#define SHEEP_TURN_VARIATION 110.f
#define SHEEP_HIT_RANGE 120.f
#define SHEEP_DESPAWN_DISTANCE (BLOCK_SIZE * 32.f)
#define SHEEP_RESPAWN_DELAY 180.f

Sheep sheep[MAX_SHEEP];
static float sheep_respawn_time;

static u32 blockIndex(int x, int y, int z) {
  return x * MAX_Y * MAX_Z + y * MAX_Z + z;
}

/* A sheep only walks on an exposed grass surface with enough head room for
   its woolly body.  Checking the destination column before moving gives it
   cheap wall and ledge avoidance without giving this first passive mob a
   second copy of player-grade swept collision. */
static u8 sheepGroundAt(int x, int z, int *ground_y) {
  int y;

  if (x < 0 || z < 0 || x >= MAX_X || z >= MAX_Z) {
    return FALSE;
  }
  for (y = MAX_Y - 3; y >= 0; y--) {
    if (blocks[blockIndex(x, y, z)] != GRASS) {
      continue;
    }
    if (blocks[blockIndex(x, y + 1, z)] == AIR &&
        blocks[blockIndex(x, y + 2, z)] == AIR) {
      *ground_y = y;
      return TRUE;
    }
  }
  return FALSE;
}

static u8 sheepTooCloseToPlayer(float x, float z) {
  u8 player_num;

  for (player_num = 0; player_num < active_player_count; player_num++) {
    float dx = players[player_num].position.x - x;
    float dz = players[player_num].position.z - z;
    if (dx * dx + dz * dz < (BLOCK_SIZE * 7.f) * (BLOCK_SIZE * 7.f)) {
      return TRUE;
    }
  }
  return FALSE;
}

static u8 sheepTooCloseToSheep(Sheep *self, float x, float z) {
  u8 index;

  for (index = 0; index < MAX_SHEEP; index++) {
    Sheep *other = &sheep[index];
    float dx;
    float dz;

    if (other == self || !other->active) {
      continue;
    }
    dx = other->position.x - x;
    dz = other->position.z - z;
    if (dx * dx + dz * dz < (BLOCK_SIZE * 2.f) * (BLOCK_SIZE * 2.f)) {
      return TRUE;
    }
  }
  return FALSE;
}

static u8 spawnSheep(Sheep *mob) {
  u8 attempt;
  Player *anchor = &players[random(active_player_count)];
  int origin_x = floor(anchor->position.x / BLOCK_SIZE);
  int origin_z = floor(anchor->position.z / BLOCK_SIZE);

  for (attempt = 0; attempt < 96; attempt++) {
    int x = origin_x + (int) random(49) - 24;
    int z = origin_z + (int) random(49) - 24;
    int ground_y;
    float world_x;
    float world_z;

    if (!sheepGroundAt(x, z, &ground_y)) {
      continue;
    }
    world_x = (x + .5f) * BLOCK_SIZE;
    world_z = (z + .5f) * BLOCK_SIZE;
    if (sheepTooCloseToPlayer(world_x, world_z) ||
        sheepTooCloseToSheep(mob, world_x, world_z)) {
      continue;
    }
    mob->position = (Vector3) {world_x, (ground_y + 1) * BLOCK_SIZE, world_z};
    mob->knockback_velocity = (Vector3) {0, 0, 0};
    mob->yaw = random(360);
    mob->walk_time = random(360) * M_DTOR;
    mob->turn_time = SHEEP_TURN_MIN + random((u32) SHEEP_TURN_VARIATION);
    mob->hurt_time = 0;
    mob->health = SHEEP_MAX_HEALTH;
    mob->active = TRUE;
    return TRUE;
  }
  return FALSE;
}

void initMobs() {
  u8 index;

  sheep_respawn_time = SHEEP_RESPAWN_DELAY;
  for (index = 0; index < MAX_SHEEP; index++) {
    sheep[index].active = FALSE;
  }
  for (index = 0; index < MAX_SHEEP; index++) {
    spawnSheep(&sheep[index]);
  }
}

static u8 sheepNearAnyPlayer(Sheep *mob) {
  u8 player_num;

  for (player_num = 0; player_num < active_player_count; player_num++) {
    float dx = players[player_num].position.x - mob->position.x;
    float dz = players[player_num].position.z - mob->position.z;
    if (dx * dx + dz * dz <= SHEEP_DESPAWN_DISTANCE * SHEEP_DESPAWN_DISTANCE) {
      return TRUE;
    }
  }
  return FALSE;
}

static u8 moveSheep(Sheep *mob, Vector3 motion) {
  Vector3 candidate = add(mob->position, motion);
  int x = floor(candidate.x / BLOCK_SIZE);
  int z = floor(candidate.z / BLOCK_SIZE);
  int ground_y;

  if (!sheepGroundAt(x, z, &ground_y) ||
      ground_y != floor(mob->position.y / BLOCK_SIZE) - 1) {
    return FALSE;
  }
  mob->position.x = candidate.x;
  mob->position.z = candidate.z;
  return TRUE;
}

static void turnSheep(Sheep *mob) {
  mob->yaw += 95.f + random(171);
  while (mob->yaw >= 360) {
    mob->yaw -= 360;
  }
  mob->turn_time = SHEEP_TURN_MIN + random((u32) SHEEP_TURN_VARIATION);
}

void updateMobs(float delta) {
  u8 index;

  for (index = 0; index < MAX_SHEEP; index++) {
    Sheep *mob = &sheep[index];
    Vector3 forward;
    Vector3 motion;

    if (!mob->active) {
      continue;
    }
    if (!sheepNearAnyPlayer(mob)) {
      mob->active = FALSE;
      sheep_respawn_time = min(sheep_respawn_time, 20.f);
      continue;
    }

    mob->hurt_time = max(0, mob->hurt_time - delta);
    mob->turn_time -= delta;
    if (mob->turn_time <= 0) {
      turnSheep(mob);
    }
    forward = rotateY((Vector3) {0, 0, -1}, -mob->yaw);
    motion = mul(forward, SHEEP_WALK_SPEED * delta);
    motion = add(motion, mul(mob->knockback_velocity, delta));
    mob->knockback_velocity = mul(mob->knockback_velocity,
      max(0, 1.f - delta * .18f));
    if (moveSheep(mob, motion)) {
      mob->walk_time += delta * 8.f;
    } else {
      mob->knockback_velocity.x = 0;
      mob->knockback_velocity.z = 0;
      turnSheep(mob);
    }
  }

  sheep_respawn_time = max(0, sheep_respawn_time - delta);
  if (sheep_respawn_time == 0) {
    for (index = 0; index < MAX_SHEEP; index++) {
      if (!sheep[index].active && spawnSheep(&sheep[index])) {
        sheep_respawn_time = SHEEP_RESPAWN_DELAY;
        return;
      }
    }
    /* A heavily forested or player-built area may temporarily have no valid
       grass column.  Retry later instead of scanning the full spawn search
       for every frame. */
    sheep_respawn_time = 60.f;
  }
}

static Sheep *swordTarget(u8 attacker_num) {
  Player *attacker = &players[attacker_num];
  Vector3 forward = rotateY((Vector3) {0, 0, -1}, -attacker->yaw);
  Sheep *target = NULL;
  float nearest = SHEEP_HIT_RANGE + 1;
  u8 index;

  for (index = 0; index < MAX_SHEEP; index++) {
    Sheep *candidate = &sheep[index];
    float dx;
    float dy;
    float dz;
    float horizontal_distance;
    float facing;

    if (!candidate->active) {
      continue;
    }
    dx = candidate->position.x - attacker->position.x;
    dy = candidate->position.y + BLOCK_SIZE * .55f - attacker->position.y;
    dz = candidate->position.z - attacker->position.z;
    horizontal_distance = sqrtf(dx * dx + dz * dz);
    if (horizontal_distance < 1.f || horizontal_distance > SHEEP_HIT_RANGE ||
        dy < -104.f || dy > 32.f) {
      continue;
    }
    facing = (forward.x * dx + forward.z * dz) / horizontal_distance;
    if (facing < .5f || horizontal_distance >= nearest) {
      continue;
    }
    nearest = horizontal_distance;
    target = candidate;
  }
  return target;
}

u8 swingSwordAtSheep(u8 attacker_num) {
  Player *attacker = &players[attacker_num];
  ItemStack *held = &attacker->inventory[INVENTORY_HOTBAR_START +
    attacker->selected_hotbar_slot];
  Sheep *target;
  float dx;
  float dz;
  float horizontal_distance;

  if (held->count == 0 || held->item != WOOD_SWORD ||
      attacker->attack_time > 0) {
    return FALSE;
  }
  target = swordTarget(attacker_num);
  if (target == NULL) {
    return FALSE;
  }
  if (target->health <= 4) {
    int x = floor(target->position.x / BLOCK_SIZE);
    int y = floor(target->position.y / BLOCK_SIZE);
    int z = floor(target->position.z / BLOCK_SIZE);
    if (!spawnDroppedItem(WOOL, 1 + random(3), x, y, z)) {
      return FALSE;
    }
    target->active = FALSE;
    sheep_respawn_time = SHEEP_RESPAWN_DELAY;
  } else {
    target->health -= 4;
    target->hurt_time = PLAYER_ATTACK_DURATION;
  }

  attacker->attack_time = PLAYER_ATTACK_DURATION;
  dx = target->position.x - attacker->position.x;
  dz = target->position.z - attacker->position.z;
  horizontal_distance = sqrtf(dx * dx + dz * dz);
  target->knockback_velocity.x = dx / horizontal_distance * 13.f;
  target->knockback_velocity.z = dz / horizontal_distance * 13.f;
  playSound(SOUND_PUNCH);
  return TRUE;
}
