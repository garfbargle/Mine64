#include "mobs.h"

#include <stddef.h>

#include "audio.h"
#include "blocks.h"
#include "mon64.h"
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
/* A bird is lighter on its feet than a sheep and stops more often; the two
   together are most of what separates it from the quadrupeds in motion. */
#define MOB_CHICKEN_SPEED 0.94f
/* A calf trots to keep up with the herd rather than being left behind. */
#define MOB_BABY_SPEED_BONUS 1.18f
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
/*
 * An offered apple is the one thing that reliably beats a wander.  A tempted
 * animal walks in from further away than the CRITTERS follow and closes to
 * arm's length, because the next thing the player is going to do is press A
 * and that has to be possible without chasing the animal around.
 */
#define MOB_TEMPT_DISTANCE (BLOCK_SIZE * 9.f)
#define MOB_TEMPT_SPACING (BLOCK_SIZE * 1.7f)
#define MOB_TEMPT_SPEED 1.12f
/* Turn rates, in degrees per frame.  A walking animal swings its whole body
   around slowly; the neck is quick, which is what makes a head that follows
   the player read as attention rather than as drift. */
#define MOB_TURN_RATE 4.6f
#define MOB_TURN_RATE_URGENT 8.5f
#define MOB_HEAD_TURN_RATE 7.5f
#define MOB_HEAD_YAW_LIMIT 62.f
#define MOB_LOOK_DISTANCE (BLOCK_SIZE * 9.f)
/* Reach of the apple, both for the on-screen prompt and for the A press. */
#define MOB_FEED_RANGE (BLOCK_SIZE * 2.6f)
#define MOB_FEED_FACING .35f
/* Two animals in love have to actually meet before a calf appears. */
#define MOB_BREED_RANGE (BLOCK_SIZE * 1.9f)
/*
 * Exactly 160 turns of the limb cycle.  Left to run, walk_time reaches values
 * where a float sine loses its phase, and an animal that has been on screen
 * for a few minutes starts to twitch.  The renderer's slower cycles all use
 * multipliers that divide 160 evenly, so this wrap is invisible in every one
 * of them -- keep it that way when adding an animation.
 */
#define MOB_WALK_TIME_WRAP 1005.30965f
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
/* Resolved once per frame by updateMobs; see mobFeedTarget. */
static u8 feed_targets[MAX_PLAYERS];

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
    case MOB_CHICKEN:
      return 4;
    default:
      return 8;
  }
}

/* Species that eat an apple out of the player's hand and raise young.  The
   two questions have the same answer today, and one predicate keeps them
   from drifting apart. */
static u8 mobTypeIsFarmAnimal(u8 type) {
  return type == MOB_SHEEP || type == MOB_PIG || type == MOB_CHICKEN;
}

static u8 playerHoldsApple(Player *player) {
  ItemStack *held = &player->inventory[INVENTORY_HOTBAR_START +
    player->selected_hotbar_slot];

  return held->count > 0 && held->item == APPLE;
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
    mob->head_yaw = 0;
    mob->love_time = 0;
    mob->breed_time = 0;
    mob->baby_time = 0;
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
    feed_targets[index] = MAX_MOBS;
    mob_special_effects[index].origin = (Vector3) {0, 0, 0};
    mob_special_effects[index].yaw = 0;
    mob_special_effects[index].time = 0;
    mob_special_effects[index].type = MOB_SPECIAL_NONE;
    mob_special_effects[index].hit_count = 0;
  }
  /* Seed the world with the animals the pool still owns after 64MON has
     taken its share, so a creature world does not open with a full herd that
     then despawns to make room. */
  {
    u8 reserve = mon64RoamerReserve();
    u8 seeded = MOB_PASSIVE_BUDGET > reserve ?
      (u8) (MOB_PASSIVE_BUDGET - reserve) : 0;

    for (index = 0; index < seeded; index++) {
      static const u8 opening_herd[3] = {MOB_SHEEP, MOB_CHICKEN, MOB_PIG};
      spawnMob(&mobs[index], opening_herd[index % 3]);
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
  /* Birds pause more often than they walk, which is most of what makes a
     chicken look like a chicken from across a field. */
  if (random(4) == 0 || (mob->type == MOB_CHICKEN && random(3) == 0)) {
    mob->state = MOB_IDLE;
    return;
  }
  mob->state = MOB_WANDER;
  mob->goal_yaw = mob->yaw + 45.f + random(271);
  while (mob->goal_yaw >= 360) {
    mob->goal_yaw -= 360;
  }
}

/*
 * Steering used to snap the body to the nearest of four compass headings,
 * which is why animals appeared to walk sideways: the direction they moved in
 * was continuous but the direction they faced was not.  Behaviours now only
 * ever name a heading, and updateMobs turns the body toward it at a bounded
 * rate -- one approximated arc-tangent per frame, and a change of mind that
 * is visible rather than instantaneous.
 */
static void faceDirection(Mob *mob, Vector3 direction) {
  mob->goal_yaw = directionYaw(direction.x, direction.z);
}

/* Where the body currently points.  Every state moves along this rather than
   straight at its goal, so an animal that has changed its mind visibly turns
   into the new heading instead of sliding into it. */
static Vector3 mobFacing(Mob *mob) {
  return rotateY((Vector3) {0, 0, -1}, -mob->yaw);
}

static void fleeDirection(Mob *mob) {
  float distance;
  Player *threat = nearestPlayer(mob->position.x, mob->position.z, &distance);

  if (threat != NULL && distance > 1.f) {
    faceDirection(mob, (Vector3) {mob->position.x - threat->position.x, 0,
      mob->position.z - threat->position.z});
  }
}

static void chaseDirection(Mob *mob, Player *target, float distance) {
  if (target != NULL && distance > 1.f) {
    faceDirection(mob, (Vector3) {target->position.x - mob->position.x, 0,
      target->position.z - mob->position.z});
  }
}

static void spiderChaseDirection(Mob *mob, Player *target,
    float distance_squared) {
  /* Close spiders orbit before committing.  A slow bit of walk_time supplies
     handedness without another state byte, pathfinder, or per-frame trig. */
  if (target != NULL && distance_squared <
      (BLOCK_SIZE * 5.f) * (BLOCK_SIZE * 5.f)) {
    float side = ((int) mob->walk_time & 256) ? -.52f : .52f;
    float dx = target->position.x - mob->position.x;
    float dz = target->position.z - mob->position.z;

    faceDirection(mob, (Vector3) {dx + dz * side, 0, dz - dx * side});
    return;
  }
  chaseDirection(mob, target, distance_squared);
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
     rejects a wall or ledge on the following frame.  A wind-up has already
     spent its whole duration turned toward the target, so the body's own
     facing is the lunge direction. */
  if (mob->type == MOB_SLIME || mob->type == MOB_SPIDER) {
    mob->knockback_velocity = add(mob->knockback_velocity,
      mul(mobFacing(mob), mob->type == MOB_SPIDER ? 4.f : 3.f));
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

/*
 * A newborn takes the pool slot a wandering animal would otherwise have been
 * spawned into, so a thriving farm crowds out the ambient herd rather than
 * enlarging it.  MAX_MOBS is still what four-player frames are budgeted
 * around, and breeding cannot change that number.
 */
static u8 breedMobs(Mob *first, Mob *second) {
  u8 index;
  Mob *calf = NULL;

  for (index = 0; index < MAX_MOBS; index++) {
    if (!mobs[index].active) {
      calf = &mobs[index];
      break;
    }
  }
  if (calf == NULL) {
    /* A full pool is a reason to wait, not a reason to charge the player two
       apples for nothing: leave both animals in love and let the mood run out
       on its own if no slot ever frees. */
    return FALSE;
  }
  first->love_time = 0;
  second->love_time = 0;
  first->breed_time = MOB_BREED_COOLDOWN;
  second->breed_time = MOB_BREED_COOLDOWN;

  /* Between the parents, on the ground one of them is standing on: the
     midpoint of two animals that just walked to each other is reachable by
     construction, whereas a fresh spawn search could place the calf through
     a wall. */
  calf->position.x = (first->position.x + second->position.x) * .5f;
  calf->position.z = (first->position.z + second->position.z) * .5f;
  calf->position.y = first->position.y;
  calf->ground_y = first->ground_y;
  calf->knockback_velocity = (Vector3) {0, 0, 0};
  calf->yaw = first->yaw;
  calf->goal_yaw = first->yaw;
  calf->walk_time = 0;
  calf->decision_time = MOB_DECISION_MIN;
  calf->state_time = 0;
  calf->hurt_time = 0;
  calf->attack_time = 0;
  calf->head_yaw = 0;
  calf->love_time = 0;
  calf->breed_time = MOB_BREED_COOLDOWN;
  calf->baby_time = MOB_BABY_DURATION;
  calf->type = first->type;
  calf->health = mobMaxHealth(calf->type);
  calf->state = MOB_IDLE;
  calf->active = TRUE;
  calf->target_player = 0;
  playSound(SOUND_PICKUP);
  return TRUE;
}

/*
 * Called only for an animal that is already in love, so the scan is over the
 * handful of seconds after a successful feeding rather than every frame.
 * FALSE means there is nobody to walk to, and the caller should let the
 * animal go back to whatever it was doing -- a lone fed animal that took the
 * mate-seeking branch anyway would simply march off in a straight line.
 */
static u8 seekMate(Mob *mob) {
  u8 index;

  for (index = 0; index < MAX_MOBS; index++) {
    Mob *other = &mobs[index];
    float dx;
    float dz;

    if (other == mob || !other->active || other->type != mob->type ||
        other->love_time <= 0) {
      continue;
    }
    dx = other->position.x - mob->position.x;
    dz = other->position.z - mob->position.z;
    if (dx * dx + dz * dz < MOB_BREED_RANGE * MOB_BREED_RANGE) {
      breedMobs(mob, other);
      return TRUE;
    }
    /* Not there yet: walk toward each other.  Both partners run this branch
       on the same frame, so neither has to lead. */
    faceDirection(mob, (Vector3) {dx, 0, dz});
    return TRUE;
  }
  return FALSE;
}

/* One pass per player per frame, shared by the HUD prompt and the A button so
   the creature named on screen is the creature that gets the apple. */
static void updateFeedTargets(void) {
  u8 player_num;

  for (player_num = 0; player_num < MAX_PLAYERS; player_num++) {
    Player *player;
    Vector3 forward;
    float nearest = MOB_FEED_RANGE * MOB_FEED_RANGE;
    u8 index;

    feed_targets[player_num] = MAX_MOBS;
    if (player_num >= active_player_count || !players[player_num].active) {
      continue;
    }
    player = &players[player_num];
    if (!playerHoldsApple(player)) {
      continue;
    }
    forward = rotateY((Vector3) {0, 0, -1}, -player->yaw);
    for (index = 0; index < MAX_MOBS; index++) {
      Mob *mob = &mobs[index];
      float dx;
      float dy;
      float dz;
      float distance_squared;
      float facing;

      if (!mob->active || !mobTypeIsFarmAnimal(mob->type)) {
        continue;
      }
      dx = mob->position.x - player->position.x;
      /* A player's position is their eye, a block and a half above their
         feet, while an animal's is the ground it stands on.  Measuring from
         the animal's middle against the same asymmetric window punchMob uses
         is what keeps "in front of me" from meaning "floating at my head". */
      dy = mob->position.y + BLOCK_SIZE * .55f - player->position.y;
      dz = mob->position.z - player->position.z;
      distance_squared = dx * dx + dz * dz;
      if (distance_squared >= nearest || dy < -104.f || dy > 48.f) {
        continue;
      }
      /* Held out in front, not handed over the shoulder.  Comparing the
         squared dot product against the squared distance keeps the cosine
         test exact without a square root per candidate. */
      facing = forward.x * dx + forward.z * dz;
      if (facing <= 0 || facing * facing <
          MOB_FEED_FACING * MOB_FEED_FACING * distance_squared) {
        continue;
      }
      nearest = distance_squared;
      feed_targets[player_num] = index;
    }
  }
}

u8 mobFeedTarget(u8 player_num) {
  return player_num < MAX_PLAYERS ? feed_targets[player_num] : MAX_MOBS;
}

u8 feedMob(u8 player_num) {
  Player *player;
  ItemStack *held;
  Mob *mob;

  if (player_num >= active_player_count || !players[player_num].active ||
      feed_targets[player_num] >= MAX_MOBS) {
    return FALSE;
  }
  player = &players[player_num];
  held = &player->inventory[INVENTORY_HOTBAR_START +
    player->selected_hotbar_slot];
  if (held->count == 0 || held->item != APPLE) {
    return FALSE;
  }
  mob = &mobs[feed_targets[player_num]];

  if (MOB_IS_BABY(mob)) {
    /* An apple is worth a visible slice of the wait, so a player who wants a
       grown animal has something to spend surplus fruit on. */
    mob->baby_time = max(0, mob->baby_time - MOB_BABY_DURATION * .22f);
  } else if (mob->breed_time > 0) {
    /* Still recovering.  The apple is eaten and heals, but no calf follows;
       refusing the press outright would just look like a broken button. */
    mob->health = min(mobMaxHealth(mob->type), mob->health + 2);
  } else {
    mob->love_time = MOB_LOVE_DURATION;
    mob->health = mobMaxHealth(mob->type);
  }
  /* Being hand-fed cancels a panic: an animal the player just punched should
     not keep running from the hand that is feeding it. */
  if (mob->state == MOB_FLEE) {
    mob->state = MOB_IDLE;
    mob->state_time = 0;
    mob->decision_time = MOB_DECISION_MIN;
  }
  held->count--;
  if (held->count == 0) {
    held->item = AIR;
  }
  playSound(SOUND_PICKUP);
  return TRUE;
}

static float mobWalkSpeed(Mob *mob) {
  return mob->type == MOB_CHICKEN ? MOB_CHICKEN_SPEED : MOB_WALK_SPEED;
}

/* How fast the limb cycle advances.  Short legs have to cycle faster to cover
   the same ground, which is what stops a chicken from looking like a sheep
   that has been scaled down. */
static float mobStrideRate(Mob *mob) {
  float rate = mob->state == MOB_FLEE || mob->state == MOB_RETREAT ?
    13.f : 8.f;

  if (mob->type == MOB_CHICKEN) {
    rate *= 1.45f;
  }
  if (MOB_IS_BABY(mob)) {
    rate *= 1.5f;
  }
  return rate;
}

static void updateHeadYaw(Mob *mob, Player *attention, float delta) {
  float target = 0;
  float step = MOB_HEAD_TURN_RATE * delta;
  float difference;

  if (attention != NULL) {
    /* Relative to the body and clamped to a neck's travel.  Past the limit
       the animal has to turn its whole body, which whichever behaviour cares
       about the player is already doing through goal_yaw. */
    target = wrapDegrees(directionYaw(
      attention->position.x - mob->position.x,
      attention->position.z - mob->position.z) - mob->yaw);
    target = max(-MOB_HEAD_YAW_LIMIT, min(MOB_HEAD_YAW_LIMIT, target));
  }
  /* Both ends are already bounded by the clamp above, so this needs no angle
     wrapping -- the head never takes the long way round. */
  difference = target - mob->head_yaw;
  if (difference > step) {
    difference = step;
  } else if (difference < -step) {
    difference = -step;
  }
  mob->head_yaw += difference;
}

/* Aims a calf at the nearest grown animal of its own kind and reports the
   speed it should travel at; FALSE when it is on its own and should just
   wander. */
static u8 followParent(Mob *mob, float *speed) {
  float nearest = (BLOCK_SIZE * 11.f) * (BLOCK_SIZE * 11.f);
  float parent_x = 0;
  float parent_z = 0;
  u8 found = FALSE;
  u8 index;

  for (index = 0; index < MAX_MOBS; index++) {
    Mob *other = &mobs[index];
    float dx;
    float dz;
    float distance;

    if (other == mob || !other->active || other->type != mob->type ||
        MOB_IS_BABY(other)) {
      continue;
    }
    dx = other->position.x - mob->position.x;
    dz = other->position.z - mob->position.z;
    distance = dx * dx + dz * dz;
    if (distance < nearest) {
      nearest = distance;
      parent_x = dx;
      parent_z = dz;
      found = TRUE;
    }
  }
  if (!found) {
    return FALSE;
  }
  faceDirection(mob, (Vector3) {parent_x, 0, parent_z});
  /* Alongside, not underfoot.  A calf that never stops walking ends up
     standing inside its mother. */
  *speed = nearest < (BLOCK_SIZE * 1.5f) * (BLOCK_SIZE * 1.5f) ? 0 :
    mobWalkSpeed(mob) * MOB_BABY_SPEED_BONUS;
  return TRUE;
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
  /* 64MON takes its roamers out of this budget rather than adding a pool
     beside it.  A world with creatures in it therefore simulates and draws
     exactly as many entities as one without -- it simply has fewer sheep.
     The reserve is zero when the mod is off. */
  u8 roamer_reserve = mon64RoamerReserve();

  passive_budget = passive_budget > roamer_reserve ?
    (u8) (passive_budget - roamer_reserve) : 0;

  countMobs(&passives, &hostiles);
  if (night && !worldModOn(MOD_PEACEFUL) &&
      hostiles < MOB_NIGHT_HOSTILE_BUDGET) {
    type = randomHostileType();
  } else if (passives < passive_budget) {
    u8 roll = random(8);
    type = roll < 3 ? MOB_SHEEP : (roll < 6 ? MOB_CHICKEN : MOB_PIG);
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

  updateFeedTargets();

  for (index = 0; index < MAX_MOBS; index++) {
    Mob *mob = &mobs[index];
    Vector3 motion = {0, 0, 0};
    float speed = mobWalkSpeed(mob);
    float turn_rate = MOB_TURN_RATE;
    float player_distance;
    float vertical_distance;
    /* Nothing to look at unless a behaviour below decides otherwise; a
       grazing animal returns its head to straight ahead. */
    Player *attention = NULL;
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
    mob->love_time = max(0, mob->love_time - delta);
    mob->breed_time = max(0, mob->breed_time - delta);
    if (MOB_IS_BABY(mob)) {
      mob->baby_time = max(0, mob->baby_time - delta);
    }

    if (mobTypeIsHostile(mob->type) && !night) {
      beginDawnRetreat(mob);
    }
    if (mob->state == MOB_RETREAT) {
      if (mob->state_time == 0) {
        mob->active = FALSE;
        mob_respawn_time = min(mob_respawn_time, 20.f);
        continue;
      }
      fleeDirection(mob);
      turn_rate = MOB_TURN_RATE_URGENT;
      speed = MOB_RETREAT_SPEED;
    } else if (mob->state == MOB_WINDUP) {
      nearest = windupTarget(mob, &player_distance, &vertical_distance);
      chaseDirection(mob, nearest, player_distance);
      attention = nearest;
      turn_rate = MOB_TURN_RATE_URGENT;
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
      attention = nearest;
      turn_rate = MOB_TURN_RATE_URGENT;
      if (mob->type == MOB_SPIDER) {
        spiderChaseDirection(mob, nearest, player_distance);
      } else {
        chaseDirection(mob, nearest, player_distance);
      }
      speed = mob->type == MOB_SPIDER ? MOB_SPIDER_SPEED :
        (mob->type == MOB_ZOMBIE ? MOB_ZOMBIE_SPEED : MOB_SLIME_SPEED);
      if (player_distance < contact * contact &&
          vertical_distance < BLOCK_SIZE && mob->attack_time == 0) {
        beginMobWindup(mob, nearest);
        speed = 0;
      }
    } else if (mob->state != MOB_FLEE && mob->love_time > 0 &&
        seekMate(mob)) {
      /* A fed animal has one thing on its mind and it is not the player.
         Ahead of the temptation branch on purpose: otherwise feeding the
         second of a pair would immediately pull both back to the hand that
         fed them and the two would never reach each other. */
      mob->state = MOB_CHASE;
      speed = MOB_TEMPT_SPEED;
    } else if (mob->state != MOB_FLEE && mobTypeIsFarmAnimal(mob->type) &&
        vertical_distance < BLOCK_SIZE * 2.f &&
        playerHoldsApple(nearest) &&
        player_distance < MOB_TEMPT_DISTANCE * MOB_TEMPT_DISTANCE) {
      /* Reusing CHASE rather than adding a state: the movement gate, the
         walk animation and the stuck-recovery all already read it, and a
         tempted animal wants every one of those behaviours.  Inside the
         spacing ring it holds position, watching the apple. */
      mob->state = MOB_CHASE;
      attention = nearest;
      chaseDirection(mob, nearest, player_distance);
      speed = player_distance < MOB_TEMPT_SPACING * MOB_TEMPT_SPACING ?
        0.f : MOB_TEMPT_SPEED;
    } else if (worldModOn(MOD_CRITTERS) && !mobTypeIsHostile(mob->type) &&
        mob->state != MOB_FLEE &&
        vertical_distance < BLOCK_SIZE * 2.f &&
        player_distance < MOB_FOLLOW_DISTANCE * MOB_FOLLOW_DISTANCE) {
      mob->state = MOB_CHASE;
      attention = nearest;
      chaseDirection(mob, nearest, player_distance);
      speed = player_distance < MOB_FOLLOW_SPACING * MOB_FOLLOW_SPACING ?
        0.f : MOB_FOLLOW_SPEED;
    } else if (MOB_IS_BABY(mob) && mob->state != MOB_FLEE &&
        followParent(mob, &speed)) {
      /* followParent has already aimed the calf at the nearest grown animal
         of its own kind; a lone calf falls through to wandering.  IDLE is
         what holds it still once it has caught up. */
      mob->state = speed > 0 ? MOB_CHASE : MOB_IDLE;
    } else if (mob->state == MOB_CHASE) {
      chooseMobAction(mob);
    } else if (mob->state == MOB_FLEE) {
      if (mob->state_time == 0) {
        chooseMobAction(mob);
      } else {
        fleeDirection(mob);
        turn_rate = MOB_TURN_RATE_URGENT;
        speed = MOB_FLEE_SPEED;
      }
    } else if (mob->decision_time <= 0) {
      chooseMobAction(mob);
    }

    /* Curiosity, applied after the behaviours have had their say: an animal
       with nothing else to look at still turns its head toward a player who
       walks past, which is the whole difference between scenery and a
       creature that has noticed you. */
    if (attention == NULL && !mobTypeIsHostile(mob->type) &&
        player_distance < MOB_LOOK_DISTANCE * MOB_LOOK_DISTANCE &&
        vertical_distance < BLOCK_SIZE * 3.f) {
      attention = nearest;
    }
    updateHeadYaw(mob, attention, delta);
    mob->yaw = approachAngle(mob->yaw, mob->goal_yaw, turn_rate * delta);

    if (mob->state != MOB_IDLE && mob->state != MOB_WINDUP) {
      motion = mul(mobFacing(mob), speed * delta);
    }
    motion = add(motion, mul(mob->knockback_velocity, delta));
    mob->knockback_velocity = mul(mob->knockback_velocity,
      max(0, 1.f - delta * .18f));

    if (moveMob(mob, motion)) {
      if (mob->state != MOB_IDLE && mob->state != MOB_WINDUP) {
        mob->walk_time += delta * mobStrideRate(mob);
      } else {
        /* Idle animals still breathe: the graze and peck cycles read from
           walk_time, so it has to keep advancing when the feet stop. */
        mob->walk_time += delta * .9f;
      }
      if (mob->walk_time >= MOB_WALK_TIME_WRAP) {
        mob->walk_time -= MOB_WALK_TIME_WRAP;
      }
    } else {
      mob->knockback_velocity.x = 0;
      mob->knockback_velocity.z = 0;
      /* Chasers recompute their heading next frame.  Wanderers use this turn
         to escape local corners without a pathfinding allocation. */
      if (mob->state != MOB_CHASE && mob->state != MOB_WINDUP) {
        mob->goal_yaw = mob->yaw + 120.f + random(121);
        if (mob->goal_yaw >= 360) mob->goal_yaw -= 360;
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

  /* A calf is not worth farming.  Without this, breeding would be a meat
     press rather than a reason to keep animals alive. */
  if (MOB_IS_BABY(target)) {
    return;
  }
  switch (target->type) {
    case MOB_PIG:
      spawnDroppedItem(RAW_PORK, 1 + random(3), x, y, z);
      break;
    case MOB_CHICKEN:
      spawnDroppedItem(RAW_CHICKEN, 1, x, y, z);
      if (random(2) == 0) {
        spawnDroppedItem(FEATHER, 1 + random(2), x, y, z);
      }
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
    /* Being hit ends the mood.  A struck animal that kept walking toward its
       mate would be reading as tame at the exact moment it is being hurt. */
    target->love_time = 0;
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
