#include "mobs.h"

#include "audio.h"
#include "blocks.h"
#include "day_cycle.h"
#include "graphics.h"
#include "items.h"
#include "player.h"
#include "world.h"

#define MOB_WALK_SPEED 0.82f
#define MOB_FLEE_SPEED 1.55f
#define MOB_DECISION_MIN 45.f
#define MOB_DECISION_VARIATION 105.f
#define MOB_HIT_RANGE 120.f
#define MOB_DESPAWN_DISTANCE (BLOCK_SIZE * 34.f)
#define MOB_RESPAWN_DELAY 150.f
#define MOB_FLEE_DURATION 110.f
#define MOB_CHASE_DISTANCE (BLOCK_SIZE * 11.f)
#define MOB_CONTACT_DISTANCE 56.f
#define MOB_ATTACK_COOLDOWN 70.f

Mob mobs[MAX_MOBS];
static float mob_respawn_time;

static u8 mobMaxHealth(u8 type) {
  if (type == MOB_SLIME) return 6;
  return type == MOB_PIG ? MOB_MAX_HEALTH : 8;
}

static u8 mobGroundAt(u8 type, int x, int z, int *ground_y) {
  int y;

  if (!windowColumnResident(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT)) {
    return FALSE;
  }
  for (y = MAX_Y - 3; y >= 0; y--) {
    u8 ground = blockGet(x, y, z);
    if (!BLOCK_IS_SOLID(ground) ||
        (type != MOB_SLIME && ground != GRASS)) {
      continue;
    }
    if (blockGet(x, y + 1, z) == AIR &&
        blockGet(x, y + 2, z) == AIR) {
      *ground_y = y;
      return TRUE;
    }
  }
  return FALSE;
}

static Player *nearestPlayer(float x, float z, float *distance_squared) {
  Player *nearest = NULL;
  u8 player_num;

  *distance_squared = 999999999.f;
  for (player_num = 0; player_num < active_player_count; player_num++) {
    float dx = players[player_num].position.x - x;
    float dz = players[player_num].position.z - z;
    float distance = dx * dx + dz * dz;
    if (players[player_num].active && distance < *distance_squared) {
      *distance_squared = distance;
      nearest = &players[player_num];
    }
  }
  return nearest;
}

static u8 mobTooClose(Mob *self, float x, float z) {
  u8 index;
  float player_distance;

  nearestPlayer(x, z, &player_distance);
  if (player_distance < (BLOCK_SIZE * 7.f) * (BLOCK_SIZE * 7.f)) {
    return TRUE;
  }
  for (index = 0; index < MAX_MOBS; index++) {
    Mob *other = &mobs[index];
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

static u8 spawnMob(Mob *mob, u8 type) {
  u8 attempt;
  Player *anchor = &players[random(active_player_count)];
  int origin_x = floor(anchor->position.x / BLOCK_SIZE);
  int origin_z = floor(anchor->position.z / BLOCK_SIZE);

  for (attempt = 0; attempt < 96; attempt++) {
    int x = origin_x + (int) random(53) - 26;
    int z = origin_z + (int) random(53) - 26;
    int ground_y;
    float world_x;
    float world_z;

    if (!mobGroundAt(type, x, z, &ground_y)) {
      continue;
    }
    world_x = (x + .5f) * BLOCK_SIZE;
    world_z = (z + .5f) * BLOCK_SIZE;
    if (mobTooClose(mob, world_x, world_z)) {
      continue;
    }
    mob->position = (Vector3) {world_x, (ground_y + 1) * BLOCK_SIZE,
      world_z};
    mob->knockback_velocity = (Vector3) {0, 0, 0};
    mob->yaw = random(360);
    mob->walk_time = random(360) * M_DTOR;
    mob->decision_time = MOB_DECISION_MIN +
      random((u32) MOB_DECISION_VARIATION);
    mob->state_time = 0;
    mob->hurt_time = 0;
    mob->attack_time = 0;
    mob->health = mobMaxHealth(type);
    mob->type = type;
    mob->state = MOB_WANDER;
    mob->active = TRUE;
    return TRUE;
  }
  return FALSE;
}

void initMobs() {
  u8 index;

  mob_respawn_time = MOB_RESPAWN_DELAY;
  for (index = 0; index < MAX_MOBS; index++) {
    mobs[index].active = FALSE;
  }
  for (index = 0; index < 6; index++) {
    /* A small mixed herd feels livelier than four identical agents while
       retaining two guaranteed night-creature slots in the hard entity
       ceiling. */
    spawnMob(&mobs[index], index % 3 == 2 ? MOB_PIG : MOB_SHEEP);
  }
}

static u8 moveMob(Mob *mob, Vector3 motion) {
  Vector3 candidate = add(mob->position, motion);
  int x = floor(candidate.x / BLOCK_SIZE);
  int z = floor(candidate.z / BLOCK_SIZE);
  int current_ground = floor(mob->position.y / BLOCK_SIZE) - 1;
  int ground_y;

  if (!mobGroundAt(mob->type, x, z, &ground_y) ||
      ground_y < current_ground - 1 || ground_y > current_ground + 1) {
    return FALSE;
  }
  mob->position.x = candidate.x;
  mob->position.z = candidate.z;
  /* Cheap vertical easing lets animals climb a block without popping. */
  mob->position.y += ((ground_y + 1) * BLOCK_SIZE - mob->position.y) * .32f;
  return TRUE;
}

static void chooseMobAction(Mob *mob) {
  mob->decision_time = MOB_DECISION_MIN +
    random((u32) MOB_DECISION_VARIATION);
  if (random(4) == 0) {
    mob->state = MOB_IDLE;
    return;
  }
  mob->state = MOB_WANDER;
  mob->yaw += 65.f + random(231);
  while (mob->yaw >= 360) {
    mob->yaw -= 360;
  }
}

static Vector3 fleeDirection(Mob *mob) {
  float distance;
  Player *threat = nearestPlayer(mob->position.x, mob->position.z, &distance);
  Vector3 direction = {0, 0, -1};

  if (threat != NULL && distance > 1.f) {
    direction.x = mob->position.x - threat->position.x;
    direction.z = mob->position.z - threat->position.z;
    distance = sqrtf(direction.x * direction.x + direction.z * direction.z);
    direction.x /= distance;
    direction.z /= distance;
    if ((direction.x < 0 ? -direction.x : direction.x) >
        (direction.z < 0 ? -direction.z : direction.z)) {
      mob->yaw = direction.x > 0 ? 270.f : 90.f;
    } else {
      mob->yaw = direction.z > 0 ? 180.f : 0.f;
    }
  }
  return direction;
}

static Vector3 chaseDirection(Mob *mob, Player *target, float distance) {
  Vector3 direction = {0, 0, -1};

  if (target != NULL && distance > 1.f) {
    direction.x = target->position.x - mob->position.x;
    direction.z = target->position.z - mob->position.z;
    distance = sqrtf(direction.x * direction.x + direction.z * direction.z);
    direction.x /= distance;
    direction.z /= distance;
    if ((direction.x < 0 ? -direction.x : direction.x) >
        (direction.z < 0 ? -direction.z : direction.z)) {
      mob->yaw = direction.x > 0 ? 270.f : 90.f;
    } else {
      mob->yaw = direction.z > 0 ? 180.f : 0.f;
    }
  }
  return direction;
}

static u8 worldIsNight() {
  u32 time = dayCycleTimeOfDay();
  return time >= 13000 && time <= 23000;
}

void updateMobs(float delta) {
  u8 index;

  for (index = 0; index < MAX_MOBS; index++) {
    Mob *mob = &mobs[index];
    Vector3 forward;
    Vector3 motion = {0, 0, 0};
    float speed = MOB_WALK_SPEED;
    float player_distance;
    float vertical_distance;
    Player *nearest;

    if (!mob->active) {
      continue;
    }
    nearest = nearestPlayer(mob->position.x, mob->position.z,
      &player_distance);
    vertical_distance = nearest == NULL ? 999999.f :
      nearest->position.y - mob->position.y;
    if (vertical_distance < 0) {
      vertical_distance = -vertical_distance;
    }
    if (nearest == NULL ||
        player_distance > MOB_DESPAWN_DISTANCE * MOB_DESPAWN_DISTANCE) {
      mob->active = FALSE;
      mob_respawn_time = min(mob_respawn_time, 20.f);
      continue;
    }

    mob->hurt_time = max(0, mob->hurt_time - delta);
    mob->attack_time = max(0, mob->attack_time - delta);
    mob->decision_time -= delta;
    mob->state_time = max(0, mob->state_time - delta);
    if (mob->state == MOB_FLEE && mob->state_time == 0) {
      chooseMobAction(mob);
    } else if (mob->state != MOB_FLEE && mob->decision_time <= 0) {
      chooseMobAction(mob);
    }

    if (mob->type == MOB_SLIME &&
        vertical_distance < BLOCK_SIZE * 2.f &&
        player_distance < MOB_CHASE_DISTANCE * MOB_CHASE_DISTANCE) {
      mob->state = MOB_CHASE;
      forward = chaseDirection(mob, nearest, player_distance);
      speed = 1.15f;
      if (player_distance < MOB_CONTACT_DISTANCE * MOB_CONTACT_DISTANCE &&
          vertical_distance < BLOCK_SIZE &&
          mob->attack_time == 0 && nearest != NULL) {
        damagePlayer(nearest - players, 2, mob->position);
        mob->attack_time = MOB_ATTACK_COOLDOWN;
      }
    } else if (mob->state == MOB_CHASE) {
      chooseMobAction(mob);
      forward = rotateY((Vector3) {0, 0, -1}, -mob->yaw);
    } else if (mob->state == MOB_FLEE) {
      forward = fleeDirection(mob);
      speed = MOB_FLEE_SPEED;
    } else {
      forward = rotateY((Vector3) {0, 0, -1}, -mob->yaw);
    }
    if (mob->state != MOB_IDLE) {
      motion = mul(forward, speed * delta);
    }
    motion = add(motion, mul(mob->knockback_velocity, delta));
    mob->knockback_velocity = mul(mob->knockback_velocity,
      max(0, 1.f - delta * .18f));

    if (moveMob(mob, motion)) {
      if (mob->state != MOB_IDLE) {
        mob->walk_time += delta * (mob->state == MOB_FLEE ? 13.f : 8.f);
      }
    } else {
      mob->knockback_velocity.x = 0;
      mob->knockback_velocity.z = 0;
      mob->yaw += 120.f + random(121);
      if (mob->yaw >= 360) mob->yaw -= 360;
      mob->decision_time = 20.f;
    }
  }

  mob_respawn_time = max(0, mob_respawn_time - delta);
  if (mob_respawn_time == 0) {
    u8 night = worldIsNight();
    for (index = 0; index < MAX_MOBS; index++) {
      u8 type;

      if (mobs[index].active || (index >= 6 && !night)) {
        continue;
      }
      type = index >= 6 ? MOB_SLIME :
        (random(3) == 0 ? MOB_PIG : MOB_SHEEP);
      if (spawnMob(&mobs[index], type)) {
        mob_respawn_time = MOB_RESPAWN_DELAY;
        return;
      }
    }
    mob_respawn_time = 60.f;
  }
}

static Mob *punchTarget(u8 attacker_num) {
  Player *attacker = &players[attacker_num];
  Vector3 forward = rotateY((Vector3) {0, 0, -1}, -attacker->yaw);
  Mob *target = NULL;
  float nearest = MOB_HIT_RANGE + 1;
  u8 index;

  for (index = 0; index < MAX_MOBS; index++) {
    Mob *candidate = &mobs[index];
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
    if (horizontal_distance < 1.f || horizontal_distance > MOB_HIT_RANGE ||
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

static u8 emitMobDrops(Mob *target) {
  int x = floor(target->position.x / BLOCK_SIZE);
  int y = floor(target->position.y / BLOCK_SIZE);
  int z = floor(target->position.z / BLOCK_SIZE);

  if (target->type == MOB_PIG) {
    return spawnDroppedItem(RAW_PORK, 1 + random(3), x, y, z);
  }
  if (target->type == MOB_SLIME) {
    return spawnDroppedItem(SLIME_GEL, 1 + random(2), x, y, z);
  }
  if (!spawnDroppedItem(WOOL, 1 + random(3), x, y, z)) {
    return FALSE;
  }
  spawnDroppedItem(RAW_MUTTON, 1 + random(2), x, y, z);
  return TRUE;
}

u8 punchMob(u8 attacker_num) {
  Player *attacker = &players[attacker_num];
  ItemStack *held = &attacker->inventory[INVENTORY_HOTBAR_START +
    attacker->selected_hotbar_slot];
  Mob *target;
  float dx;
  float dz;
  float horizontal_distance;
  u8 damage;

  if (attacker->attack_time > 0) {
    return FALSE;
  }
  target = punchTarget(attacker_num);
  if (target == NULL) {
    return FALSE;
  }
  /* Every held item, including an empty hand, can punch.  Only swords use
     their weapon damage; this keeps early encounters survivable without
     making a random block as effective as a crafted weapon. */
  damage = held->count > 0 && held->item == IRON_SWORD ? 8 :
    (held->count > 0 && held->item == STONE_SWORD ? 6 :
    (held->count > 0 && held->item == WOOD_SWORD ? 4 : 1));
  if (target->health <= damage) {
    if (!emitMobDrops(target)) {
      return FALSE;
    }
    target->active = FALSE;
    mob_respawn_time = MOB_RESPAWN_DELAY;
  } else {
    target->health -= damage;
    target->hurt_time = PLAYER_ATTACK_DURATION;
    target->state = MOB_FLEE;
    target->state_time = MOB_FLEE_DURATION;
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
