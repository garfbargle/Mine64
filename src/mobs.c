#include "mobs.h"

#include <stddef.h>

#include "audio.h"
#include "blocks.h"
#include "cobblemon.h"
#include "day_cycle.h"
#include "graphics.h"
#include "items.h"
#include "player.h"
#include "world.h"
#include "mods.h"

#define MOB_WALK_SPEED 0.82f
#define MOB_FLEE_SPEED 1.55f
#define MOB_ZOMBIE_SPEED 1.02f
#define MOB_SLIME_SPEED 1.15f
#define MOB_SPIDER_SPEED 1.42f
#define MOB_RETREAT_SPEED 1.68f
/*
 * CRITTERS: how a world of creatures differs from a world with creatures in
 * it.  Animals notice the player from seven blocks and walk over, stopping
 * short of standing inside them.  Slightly faster than a wander so they
 * actually arrive, slower than the player so they trail rather than crowd --
 * and a frightened animal still runs, because the FLEE branch is ahead of
 * this one.
 */
#define MOB_FOLLOW_SPEED 0.95f
#define MOB_FOLLOW_DISTANCE (BLOCK_SIZE * 7.f)
#define MOB_FOLLOW_SPACING (BLOCK_SIZE * 1.7f)
#define MOB_DECISION_MIN 45.f
#define MOB_DECISION_VARIATION 105.f
#define MOB_HIT_RANGE 120.f
#define MOB_DESPAWN_DISTANCE (BLOCK_SIZE * 34.f)
#define MOB_RESPAWN_DELAY 150.f
#define MOB_FAILED_SPAWN_DELAY 60.f
#define MOB_FLEE_DURATION 110.f
#define MOB_DAWN_RETREAT_MIN 150.f
#define MOB_DAWN_RETREAT_VARIATION 120.f
#define MOB_ATTACK_COOLDOWN 70.f
#define MOB_SLIME_WINDUP 24.f
#define MOB_ZOMBIE_WINDUP 31.f
#define MOB_SPIDER_WINDUP 16.f
#define MOB_LOS_STEP (BLOCK_SIZE * .42f)
#define MOB_LOS_MAX_STEPS 16

#define WOOD_RUSH_RANGE (BLOCK_SIZE * 2.45f)
#define STONE_CLEAVE_RANGE (BLOCK_SIZE * 2.15f)
#define IRON_SHOCKWAVE_RANGE (BLOCK_SIZE * 4.15f)
#define WOOD_RUSH_COOLDOWN 150.f
#define STONE_CLEAVE_COOLDOWN 210.f
#define IRON_SHOCKWAVE_COOLDOWN 300.f
#define SPECIAL_CLEAVE_TARGETS 3
#define SPECIAL_SHOCKWAVE_TARGETS 4

Mob mobs[MAX_MOBS];
MobSpecialEffect mob_special_effects[MAX_PLAYERS];

static float mob_respawn_time;
static float special_cooldowns[MAX_PLAYERS];

u8 mobTypeIsHostile(u8 type) {
  return type == MOB_SLIME || type == MOB_ZOMBIE || type == MOB_SPIDER;
}

static u8 mobMaxHealth(u8 type) {
  switch (type) {
    case MOB_SLIME:
      return 6;
    case MOB_ZOMBIE:
      return 12;
    case MOB_SPIDER:
      return 8;
    case MOB_PIG:
      return 10;
    default:
      return 8;
  }
}

static u8 mobCanStandAt(u8 type, int x, int ground_y, int z) {
  u8 ground;

  if ((u32) ground_y >= (u32) (MAX_Y - 2)) {
    return FALSE;
  }
  ground = blockGet(x, ground_y, z);
  if (!BLOCK_IS_SOLID(ground)) {
    return FALSE;
  }
  /* Grazers stay on grass.  Monsters and their knockback are allowed across
     player-built floors, stone, and sand, which keeps combat around a base
     from looking like an invisible navigation wall. */
  if (!mobTypeIsHostile(type) && ground != GRASS) {
    return FALSE;
  }
  return blockGet(x, ground_y + 1, z) == AIR &&
    blockGet(x, ground_y + 2, z) == AIR;
}

/* A full-height scan is reserved for the infrequent spawn attempt.  Once a
   mob exists, its cached ground_y drives the three-cell local query below. */
static u8 mobSpawnGroundAt(u8 type, int x, int z, int *ground_y) {
  int y;

  if (!windowColumnResident(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT)) {
    return FALSE;
  }
  for (y = MAX_Y - 3; y >= 0; y--) {
    /* A canopy is technically solid but produces stranded ground AI.  It may
       be crossed after knockback; it is never selected as a spawn pad. */
    if (mobCanStandAt(type, x, y, z) && blockGet(x, y, z) != LEAVES) {
      *ground_y = y;
      return TRUE;
    }
  }
  return FALSE;
}

static u8 mobGroundNear(u8 type, int x, int z, int current_y,
    int *ground_y) {
  static const s8 offsets[3] = {0, 1, -1};
  u8 index;

  if (!windowColumnResident(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT)) {
    return FALSE;
  }
  for (index = 0; index < 3; index++) {
    int y = current_y + offsets[index];
    if (mobCanStandAt(type, x, y, z)) {
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

static Player *spawnAnchor(void) {
  u8 offset;
  u8 start;

  if (active_player_count == 0) {
    return NULL;
  }
  start = random(active_player_count);
  for (offset = 0; offset < active_player_count; offset++) {
    u8 player_num = (start + offset) % active_player_count;
    if (players[player_num].active) {
      return &players[player_num];
    }
  }
  return NULL;
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
  Player *anchor = spawnAnchor();
  int origin_x;
  int origin_z;

  if (anchor == NULL) {
    return FALSE;
  }
  origin_x = floor(anchor->position.x / BLOCK_SIZE);
  origin_z = floor(anchor->position.z / BLOCK_SIZE);
  for (attempt = 0; attempt < 96; attempt++) {
    int x = origin_x + (int) random(53) - 26;
    int z = origin_z + (int) random(53) - 26;
    int ground_y;
    float world_x;
    float world_z;

    if (!mobSpawnGroundAt(type, x, z, &ground_y)) {
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
    /* A newly materialized monster cannot connect before the player has had
       time to notice it, even if streaming exposes a very close ledge. */
    mob->attack_time = mobTypeIsHostile(type) ? 36.f + random(30) : 0;
    mob->health = mobMaxHealth(type);
    mob->type = type;
    mob->state = MOB_WANDER;
    mob->active = TRUE;
    mob->ground_y = ground_y;
    mob->target_player = 0;
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
  for (index = 0; index < MAX_PLAYERS; index++) {
    special_cooldowns[index] = 0;
    mob_special_effects[index].origin = (Vector3) {0, 0, 0};
    mob_special_effects[index].yaw = 0;
    mob_special_effects[index].time = 0;
    mob_special_effects[index].type = MOB_SPECIAL_NONE;
    mob_special_effects[index].hit_count = 0;
  }
  /* Seed the world with the animals the pool still owns after COBBLEMON has
     taken its share, so a creature world does not open with a full herd that
     then despawns to make room. */
  {
    u8 reserve = cobblemonRoamerReserve();
    u8 seeded = MOB_PASSIVE_BUDGET > reserve ?
      (u8) (MOB_PASSIVE_BUDGET - reserve) : 0;

    for (index = 0; index < seeded; index++) {
      spawnMob(&mobs[index], index % 3 == 2 ? MOB_PIG : MOB_SHEEP);
    }
  }
}

static u8 moveMob(Mob *mob, Vector3 motion) {
  Vector3 candidate = add(mob->position, motion);
  int x = floor(candidate.x / BLOCK_SIZE);
  int z = floor(candidate.z / BLOCK_SIZE);
  int ground_y;

  if (!mobGroundNear(mob->type, x, z, mob->ground_y, &ground_y)) {
    return FALSE;
  }
  mob->position.x = candidate.x;
  mob->position.z = candidate.z;
  mob->ground_y = ground_y;
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

static void faceDirection(Mob *mob, Vector3 direction) {
  if ((direction.x < 0 ? -direction.x : direction.x) >
      (direction.z < 0 ? -direction.z : direction.z)) {
    mob->yaw = direction.x > 0 ? 270.f : 90.f;
  } else {
    mob->yaw = direction.z > 0 ? 180.f : 0.f;
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
    faceDirection(mob, direction);
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
    faceDirection(mob, direction);
  }
  return direction;
}

static Vector3 spiderChaseDirection(Mob *mob, Player *target,
    float distance_squared) {
  Vector3 direction = chaseDirection(mob, target, distance_squared);

  /* Close spiders orbit before committing.  A slow bit of walk_time supplies
     handedness without another state byte, pathfinder, or per-frame trig. */
  if (target != NULL && distance_squared <
      (BLOCK_SIZE * 5.f) * (BLOCK_SIZE * 5.f)) {
    float side = ((int) mob->walk_time & 256) ? -.52f : .52f;
    float x = direction.x + direction.z * side;
    float z = direction.z - direction.x * side;
    float length = sqrtf(x * x + z * z);
    if (length > .01f) {
      direction.x = x / length;
      direction.z = z / length;
      faceDirection(mob, direction);
    }
  }
  return direction;
}

static u8 worldIsNight() {
  u32 time = dayCycleTimeOfDay();
  return time >= 13000 && time <= 23000;
}

static float mobChaseDistance(u8 type) {
  if (type == MOB_SPIDER) return BLOCK_SIZE * 13.f;
  if (type == MOB_ZOMBIE) return BLOCK_SIZE * 12.f;
  return BLOCK_SIZE * 10.f;
}

static float mobContactDistance(u8 type) {
  if (type == MOB_SPIDER) return 62.f;
  if (type == MOB_ZOMBIE) return 58.f;
  return 56.f;
}

static float mobWindupTime(u8 type) {
  if (type == MOB_SPIDER) return MOB_SPIDER_WINDUP;
  if (type == MOB_ZOMBIE) return MOB_ZOMBIE_WINDUP;
  return MOB_SLIME_WINDUP;
}

static u8 mobAttackDamage(u8 type) {
  if (type == MOB_ZOMBIE) return 3;
  if (type == MOB_SPIDER) return 1;
  return 2;
}

/* Fixed-step visibility is only used at the moment of an attack.  The cap is
   enough for the longest special and makes a missing streaming column a wall
   rather than permission to hit through unloaded terrain. */
static u8 clearMobLineOfSight(Vector3 from, Vector3 to) {
  Vector3 difference = add(to, mul(from, -1.f));
  float distance = sqrtf(dot(difference, difference));
  u8 steps;
  u8 step;

  if (distance < 1.f) {
    return TRUE;
  }
  steps = (u8) (distance / MOB_LOS_STEP) + 1;
  if (steps > MOB_LOS_MAX_STEPS) {
    return FALSE;
  }
  for (step = 1; step < steps; step++) {
    float amount = step / (float) steps;
    Vector3 sample = add(from, mul(difference, amount));
    u8 block = blockGet(floor(sample.x / BLOCK_SIZE),
      floor(sample.y / BLOCK_SIZE), floor(sample.z / BLOCK_SIZE));
    if (block == BLOCK_NOT_RESIDENT || BLOCK_IS_SOLID(block)) {
      return FALSE;
    }
  }
  return TRUE;
}

static void beginMobWindup(Mob *mob, Player *target) {
  mob->state = MOB_WINDUP;
  mob->state_time = mobWindupTime(mob->type);
  mob->decision_time = MOB_DECISION_MIN;
  mob->target_player = target == NULL ? MAX_PLAYERS : target - players;
}

static Player *windupTarget(Mob *mob, float *distance_squared,
    float *vertical_distance) {
  Player *target;
  float dx;
  float dz;

  if (mob->target_player >= active_player_count ||
      !players[mob->target_player].active) {
    *distance_squared = 999999999.f;
    *vertical_distance = 999999.f;
    return NULL;
  }
  target = &players[mob->target_player];
  dx = target->position.x - mob->position.x;
  dz = target->position.z - mob->position.z;
  *distance_squared = dx * dx + dz * dz;
  *vertical_distance = target->position.y - mob->position.y;
  if (*vertical_distance < 0) {
    *vertical_distance = -*vertical_distance;
  }
  return target;
}

static void finishMobAttack(Mob *mob, Player *target,
    float distance_squared, float vertical_distance) {
  Vector3 target_center;
  Vector3 mob_center;
  float reach = mobContactDistance(mob->type) + 12.f;

  mob->state = MOB_CHASE;
  mob->state_time = 0;
  mob->attack_time = MOB_ATTACK_COOLDOWN;
  if (target == NULL || vertical_distance >= BLOCK_SIZE ||
      distance_squared > reach * reach) {
    return;
  }
  mob_center = mob->position;
  mob_center.y += BLOCK_SIZE * .62f;
  target_center = target->position;
  if (!clearMobLineOfSight(mob_center, target_center)) {
    return;
  }
  damagePlayer(target - players, mobAttackDamage(mob->type), mob->position);
  /* The short post-strike kick is motion, not teleportation, so moveMob still
     rejects a wall or ledge on the following frame. */
  if (mob->type == MOB_SLIME || mob->type == MOB_SPIDER) {
    Vector3 direction = chaseDirection(mob, target, distance_squared);
    mob->knockback_velocity = add(mob->knockback_velocity,
      mul(direction, mob->type == MOB_SPIDER ? 4.f : 3.f));
  }
}

static void beginDawnRetreat(Mob *mob) {
  if (mob->state == MOB_RETREAT) {
    return;
  }
  mob->state = MOB_RETREAT;
  mob->state_time = MOB_DAWN_RETREAT_MIN +
    random((u32) MOB_DAWN_RETREAT_VARIATION);
  mob->decision_time = mob->state_time;
  mob->attack_time = max(mob->attack_time, mob->state_time);
}

static void countMobs(u8 *passives, u8 *hostiles) {
  u8 index;

  *passives = 0;
  *hostiles = 0;
  for (index = 0; index < MAX_MOBS; index++) {
    if (!mobs[index].active) {
      continue;
    }
    if (mobTypeIsHostile(mobs[index].type)) {
      (*hostiles)++;
    } else {
      (*passives)++;
    }
  }
}

static u8 randomHostileType(void) {
  u8 roll = random(8);

  if (roll < 4) return MOB_ZOMBIE;
  if (roll < 7) return MOB_SLIME;
  return MOB_SPIDER;
}

static u8 spawnForBudget(u8 night) {
  u8 passives;
  u8 hostiles;
  u8 type;
  u8 index;
  /* CRITTERS hands the monsters' half of the pool to the animals, so a world
     chosen for its wildlife actually has more of it.  The pool itself never
     grows: the AI and matrix cost of MAX_MOBS is what four-player frames are
     budgeted around. */
  u8 passive_budget = worldModOn(MOD_CRITTERS) ?
    MOB_PASSIVE_BUDGET + MOB_NIGHT_HOSTILE_BUDGET : MOB_PASSIVE_BUDGET;
  /* COBBLEMON takes its roamers out of this budget rather than adding a pool
     beside it.  A world with creatures in it therefore simulates and draws
     exactly as many entities as one without -- it simply has fewer sheep.
     The reserve is zero when the mod is off. */
  u8 roamer_reserve = cobblemonRoamerReserve();

  passive_budget = passive_budget > roamer_reserve ?
    (u8) (passive_budget - roamer_reserve) : 0;

  countMobs(&passives, &hostiles);
  if (night && !worldModOn(MOD_PEACEFUL) &&
      hostiles < MOB_NIGHT_HOSTILE_BUDGET) {
    type = randomHostileType();
  } else if (passives < passive_budget) {
    type = random(3) == 0 ? MOB_PIG : MOB_SHEEP;
  } else {
    return FALSE;
  }
  for (index = 0; index < MAX_MOBS; index++) {
    if (!mobs[index].active) {
      return spawnMob(&mobs[index], type);
    }
  }
  return FALSE;
}

void updateMobs(float delta) {
  u8 index;
  u8 night = worldIsNight();

  for (index = 0; index < MAX_PLAYERS; index++) {
    special_cooldowns[index] = max(0, special_cooldowns[index] - delta);
    mob_special_effects[index].time = max(0,
      mob_special_effects[index].time - delta);
    if (mob_special_effects[index].time == 0) {
      mob_special_effects[index].type = MOB_SPECIAL_NONE;
    }
  }

  for (index = 0; index < MAX_MOBS; index++) {
    Mob *mob = &mobs[index];
    Vector3 forward = {0, 0, -1};
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

    if (mobTypeIsHostile(mob->type) && !night) {
      beginDawnRetreat(mob);
    }
    if (mob->state == MOB_RETREAT) {
      if (mob->state_time == 0) {
        mob->active = FALSE;
        mob_respawn_time = min(mob_respawn_time, 20.f);
        continue;
      }
      forward = fleeDirection(mob);
      speed = MOB_RETREAT_SPEED;
    } else if (mob->state == MOB_WINDUP) {
      nearest = windupTarget(mob, &player_distance, &vertical_distance);
      forward = chaseDirection(mob, nearest, player_distance);
      if (mob->state_time == 0) {
        finishMobAttack(mob, nearest, player_distance, vertical_distance);
      }
      /* Wind-up movement would turn anticipation into unavoidable contact.
         Knockback still resolves below, so a player can interrupt spacing. */
      speed = 0;
    } else if (mobTypeIsHostile(mob->type) && night &&
        vertical_distance < BLOCK_SIZE * 2.f &&
        player_distance < mobChaseDistance(mob->type) *
          mobChaseDistance(mob->type)) {
      float contact = mobContactDistance(mob->type);

      mob->state = MOB_CHASE;
      forward = mob->type == MOB_SPIDER ?
        spiderChaseDirection(mob, nearest, player_distance) :
        chaseDirection(mob, nearest, player_distance);
      speed = mob->type == MOB_SPIDER ? MOB_SPIDER_SPEED :
        (mob->type == MOB_ZOMBIE ? MOB_ZOMBIE_SPEED : MOB_SLIME_SPEED);
      if (player_distance < contact * contact &&
          vertical_distance < BLOCK_SIZE && mob->attack_time == 0) {
        beginMobWindup(mob, nearest);
        speed = 0;
      }
    } else if (worldModOn(MOD_CRITTERS) && !mobTypeIsHostile(mob->type) &&
        mob->state != MOB_FLEE &&
        vertical_distance < BLOCK_SIZE * 2.f &&
        player_distance < MOB_FOLLOW_DISTANCE * MOB_FOLLOW_DISTANCE) {
      /* Reusing CHASE rather than adding a state: the movement gate, the
         walk animation and the stuck-recovery all already read it, and a
         following sheep wants every one of those behaviours.  Inside the
         spacing ring it holds position and simply watches. */
      mob->state = MOB_CHASE;
      forward = chaseDirection(mob, nearest, player_distance);
      speed = player_distance < MOB_FOLLOW_SPACING * MOB_FOLLOW_SPACING ?
        0.f : MOB_FOLLOW_SPEED;
    } else if (mob->state == MOB_CHASE) {
      chooseMobAction(mob);
      forward = rotateY((Vector3) {0, 0, -1}, -mob->yaw);
    } else if (mob->state == MOB_FLEE) {
      if (mob->state_time == 0) {
        chooseMobAction(mob);
        forward = rotateY((Vector3) {0, 0, -1}, -mob->yaw);
      } else {
        forward = fleeDirection(mob);
        speed = MOB_FLEE_SPEED;
      }
    } else {
      if (mob->decision_time <= 0) {
        chooseMobAction(mob);
      }
      forward = rotateY((Vector3) {0, 0, -1}, -mob->yaw);
    }

    if (mob->state != MOB_IDLE && mob->state != MOB_WINDUP) {
      motion = mul(forward, speed * delta);
    }
    motion = add(motion, mul(mob->knockback_velocity, delta));
    mob->knockback_velocity = mul(mob->knockback_velocity,
      max(0, 1.f - delta * .18f));

    if (moveMob(mob, motion)) {
      if (mob->state != MOB_IDLE && mob->state != MOB_WINDUP) {
        mob->walk_time += delta *
          (mob->state == MOB_FLEE || mob->state == MOB_RETREAT ? 13.f : 8.f);
      }
    } else {
      mob->knockback_velocity.x = 0;
      mob->knockback_velocity.z = 0;
      /* Chasers recompute their heading next frame.  Wanderers use this turn
         to escape local corners without a pathfinding allocation. */
      if (mob->state != MOB_CHASE && mob->state != MOB_WINDUP) {
        mob->yaw += 120.f + random(121);
        if (mob->yaw >= 360) mob->yaw -= 360;
      }
      mob->decision_time = 20.f;
    }
  }

  mob_respawn_time = max(0, mob_respawn_time - delta);
  if (mob_respawn_time == 0) {
    if (spawnForBudget(night)) {
      mob_respawn_time = MOB_RESPAWN_DELAY;
    } else {
      mob_respawn_time = MOB_FAILED_SPAWN_DELAY;
    }
  }
}

static u8 mobAttackCandidate(Player *attacker, Mob *candidate,
    Vector3 forward, float range, float minimum_facing,
    float *distance_squared) {
  Vector3 target_center;
  float dx;
  float dy;
  float dz;
  float horizontal_distance;
  float facing;

  if (!candidate->active) {
    return FALSE;
  }
  dx = candidate->position.x - attacker->position.x;
  dy = candidate->position.y + BLOCK_SIZE * .55f - attacker->position.y;
  dz = candidate->position.z - attacker->position.z;
  *distance_squared = dx * dx + dz * dz;
  if (*distance_squared < 1.f || *distance_squared > range * range ||
      dy < -104.f || dy > 48.f) {
    return FALSE;
  }
  horizontal_distance = sqrtf(*distance_squared);
  facing = (forward.x * dx + forward.z * dz) / horizontal_distance;
  if (minimum_facing > -1.f && facing < minimum_facing) {
    return FALSE;
  }
  target_center = candidate->position;
  target_center.y += BLOCK_SIZE * .55f;
  return clearMobLineOfSight(attacker->position, target_center);
}

static Mob *punchTarget(u8 attacker_num) {
  Player *attacker = &players[attacker_num];
  Vector3 forward = rotateY((Vector3) {0, 0, -1}, -attacker->yaw);
  Mob *target = NULL;
  float nearest = MOB_HIT_RANGE * MOB_HIT_RANGE + 1.f;
  u8 index;

  for (index = 0; index < MAX_MOBS; index++) {
    float distance_squared;
    if (!mobAttackCandidate(attacker, &mobs[index], forward, MOB_HIT_RANGE,
        .5f, &distance_squared) || distance_squared >= nearest) {
      continue;
    }
    nearest = distance_squared;
    target = &mobs[index];
  }
  return target;
}

static void emitMobDrops(Mob *target) {
  int x = floor(target->position.x / BLOCK_SIZE);
  int y = floor(target->position.y / BLOCK_SIZE);
  int z = floor(target->position.z / BLOCK_SIZE);

  switch (target->type) {
    case MOB_PIG:
      spawnDroppedItem(RAW_PORK, 1 + random(3), x, y, z);
      break;
    case MOB_SLIME:
      spawnDroppedItem(SLIME_GEL, 1 + random(2), x, y, z);
      break;
    case MOB_ZOMBIE:
      spawnDroppedItem(COAL, 1 + random(2), x, y, z);
      if (random(4) == 0) {
        spawnDroppedItem(APPLE, 1, x, y, z);
      }
      break;
    case MOB_SPIDER:
      spawnDroppedItem(SLIME_GEL, 1, x, y, z);
      break;
    default:
      spawnDroppedItem(WOOL, 1 + random(3), x, y, z);
      spawnDroppedItem(RAW_MUTTON, 1 + random(2), x, y, z);
      break;
  }
}

static void damageMob(Mob *target, u8 damage, Vector3 source,
    float knockback) {
  float dx;
  float dz;
  float horizontal_distance;

  if (target->health <= damage) {
    emitMobDrops(target);
    target->active = FALSE;
    mob_respawn_time = MOB_RESPAWN_DELAY;
    return;
  }
  target->health -= damage;
  target->hurt_time = PLAYER_ATTACK_DURATION;
  if (!mobTypeIsHostile(target->type)) {
    target->state = MOB_FLEE;
    target->state_time = MOB_FLEE_DURATION;
  } else if (target->state != MOB_RETREAT) {
    target->state = MOB_CHASE;
    target->state_time = 0;
  }
  dx = target->position.x - source.x;
  dz = target->position.z - source.z;
  horizontal_distance = sqrtf(dx * dx + dz * dz);
  if (horizontal_distance > 1.f) {
    target->knockback_velocity.x = dx / horizontal_distance * knockback;
    target->knockback_velocity.z = dz / horizontal_distance * knockback;
  }
}

u8 punchMob(u8 attacker_num) {
  Player *attacker;
  ItemStack *held;
  Mob *target;
  u8 damage;

  if (attacker_num >= active_player_count || !players[attacker_num].active) {
    return FALSE;
  }
  attacker = &players[attacker_num];
  held = &attacker->inventory[INVENTORY_HOTBAR_START +
    attacker->selected_hotbar_slot];
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
  damageMob(target, damage, attacker->position, 13.f);
  attacker->attack_time = PLAYER_ATTACK_DURATION;
  playSound(SOUND_PUNCH);
  return TRUE;
}

static u8 specialDamageMobs(Player *attacker, Vector3 forward, float range,
    float minimum_facing, u8 damage, float knockback, u8 target_limit) {
  u8 candidate_indices[MAX_MOBS];
  float candidate_distances[MAX_MOBS];
  u8 candidate_count = 0;
  u8 hit_count = 0;
  u8 index;

  /* Classify each pool entry once.  In particular, do not repeat the bounded
     block ray while selecting the nearest four shockwave victims. */
  for (index = 0; index < MAX_MOBS; index++) {
    float distance_squared;
    if (mobAttackCandidate(attacker, &mobs[index], forward, range,
        minimum_facing, &distance_squared)) {
      candidate_indices[candidate_count] = index;
      candidate_distances[candidate_count] = distance_squared;
      candidate_count++;
    }
  }
  while (hit_count < target_limit && candidate_count > 0) {
    u8 best = 0;
    u8 slot;

    for (slot = 1; slot < candidate_count; slot++) {
      if (candidate_distances[slot] < candidate_distances[best]) {
        best = slot;
      }
    }
    damageMob(&mobs[candidate_indices[best]], damage, attacker->position,
      knockback);
    hit_count++;
    candidate_count--;
    candidate_indices[best] = candidate_indices[candidate_count];
    candidate_distances[best] = candidate_distances[candidate_count];
  }
  return hit_count;
}

u8 useMobWeaponSpecial(u8 attacker_num) {
  Player *attacker;
  ItemStack *held;
  MobSpecialEffect *effect;
  Vector3 forward;
  u8 hit_count;

  if (attacker_num >= active_player_count || !players[attacker_num].active ||
      special_cooldowns[attacker_num] > 0) {
    return FALSE;
  }
  attacker = &players[attacker_num];
  held = &attacker->inventory[INVENTORY_HOTBAR_START +
    attacker->selected_hotbar_slot];
  if (held->count == 0 || !itemIsSword(held->item) ||
      attacker->attack_time > 0) {
    return FALSE;
  }

  forward = rotateY((Vector3) {0, 0, -1}, -attacker->yaw);
  effect = &mob_special_effects[attacker_num];
  effect->origin = attacker->position;
  effect->yaw = attacker->yaw;
  effect->time = MOB_SPECIAL_EFFECT_DURATION;

  if (held->item == WOOD_SWORD) {
    effect->type = MOB_SPECIAL_WOOD_RUSH;
    special_cooldowns[attacker_num] = WOOD_RUSH_COOLDOWN;
    hit_count = specialDamageMobs(attacker, forward, WOOD_RUSH_RANGE,
      .68f, 5, 17.f, 1);
    /* Player collision owns the actual lunge, so even a frame spike cannot
       carry the rush through a wall. */
    attacker->knockback_velocity = add(attacker->knockback_velocity,
      mul(forward, 10.f));
  } else if (held->item == STONE_SWORD) {
    effect->type = MOB_SPECIAL_STONE_CLEAVE;
    special_cooldowns[attacker_num] = STONE_CLEAVE_COOLDOWN;
    hit_count = specialDamageMobs(attacker, forward, STONE_CLEAVE_RANGE,
      .18f, 6, 16.f, SPECIAL_CLEAVE_TARGETS);
  } else {
    effect->type = MOB_SPECIAL_IRON_SHOCKWAVE;
    special_cooldowns[attacker_num] = IRON_SHOCKWAVE_COOLDOWN;
    /* -2 disables the facing gate.  Visibility still makes terrain split the
       radial wave into natural rooms instead of damaging through a base. */
    hit_count = specialDamageMobs(attacker, forward, IRON_SHOCKWAVE_RANGE,
      -2.f, 7, 20.f, SPECIAL_SHOCKWAVE_TARGETS);
  }

  effect->hit_count = hit_count;
  attacker->attack_time = PLAYER_ATTACK_DURATION * 1.5f;
  playSound(SOUND_PUNCH);
  return TRUE;
}

float mobWeaponSpecialCooldown(u8 attacker_num) {
  if (attacker_num >= MAX_PLAYERS) {
    return 0;
  }
  return special_cooldowns[attacker_num];
}
