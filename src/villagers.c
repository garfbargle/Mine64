#include "villagers.h"

#include <stddef.h>

#include "blocks.h"
#include "camera.h"
#include "day_cycle.h"
#include "graphics.h"
#include "humanoid.h"
#include "player.h"
#include "world.h"

/*
 * Villager simulation.  The why is in villagers.h; this is the pool.
 *
 * Everything below is frames at the 60 Hz delta updatePlayers hands out, and
 * units where a block is BLOCK_SIZE across.
 */

/* A person's stride is longer than a sheep's and shorter than a sprint. */
#define VILLAGER_WALK_SPEED 0.72f
#define VILLAGER_STRIDE_RATE .09f
#define VILLAGER_GAIT_EASE .3f
/* How far from their own doorstep a villager will wander.  Small: a village
   whose people scatter across the fields is not a village. */
#define VILLAGER_LEASH 9
#define VILLAGER_ARRIVED (BLOCK_SIZE * .9f)
#define VILLAGER_DECISION_MIN 90.f
#define VILLAGER_DECISION_VARIATION 150.f
/* They notice you from about the width of the hamlet's square. */
#define VILLAGER_NOTICE_RANGE (BLOCK_SIZE * 8.f)
#define VILLAGER_SPAWN_DELAY 45.f
#define VILLAGER_DESPAWN_DISTANCE (BLOCK_SIZE * 40.f)
/* A cottage further away than this is not worth waking anybody for; nearer
   than the other, and they would appear in front of the player. */
#define VILLAGER_SPAWN_NEAR (BLOCK_SIZE * 8.f)
#define VILLAGER_SPAWN_FAR (BLOCK_SIZE * 30.f)
#define VILLAGER_RENDER_DISTANCE (BLOCK_SIZE * 30.f)

#define VILLAGER_IDLE 0
#define VILLAGER_WANDER 1
#define VILLAGER_HOME 2

Villager villagers[MAX_VILLAGERS];

static float spawn_time;
/* What villagerReserve reports, recomputed each slice so mobs.c can ask it
   as often as it likes without a structure lookup of its own. */
static u8 reserved_slots;

void initVillagers(void) {
  u8 index;

  for (index = 0; index < MAX_VILLAGERS; index++) {
    villagers[index].active = FALSE;
  }
  spawn_time = VILLAGER_SPAWN_DELAY;
  reserved_slots = 0;
}

u8 villagerReserve(void) {
  return reserved_slots;
}

/*
 * Where a person can stand: solid under the feet and two blocks of air above.
 *
 * 64MON asks the same question of its roamers and mobs.c of its animals, and
 * all three keep their own copy on purpose -- each pool has its own idea of
 * what a floor is, and the day one of them gains a rule (a creature that will
 * not stand on leaves; an animal that will not stand on a path) it must not
 * silently acquire the others'.
 */
static u8 canStandAt(int x, int ground_y, int z) {
  u8 ground;

  if ((u32) ground_y >= (u32) (MAX_Y - 2)) {
    return FALSE;
  }
  ground = blockGet(x, ground_y, z);
  if (ground == BLOCK_NOT_RESIDENT || !BLOCK_IS_SOLID(ground)) {
    return FALSE;
  }
  return blockGet(x, ground_y + 1, z) == AIR &&
    blockGet(x, ground_y + 2, z) == AIR;
}

/* A walking villager only ever asks about the step in front of them. */
static u8 groundNear(int x, int z, int current_y, int *ground_y) {
  static const s8 offsets[3] = {0, 1, -1};
  u8 index;

  if (!windowColumnResident(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT)) {
    return FALSE;
  }
  for (index = 0; index < 3; index++) {
    int y = current_y + offsets[index];
    if (canStandAt(x, y, z)) {
      *ground_y = y;
      return TRUE;
    }
  }
  return FALSE;
}

/* The full-height scan is only ever paid at a spawn, and only for a doorstep
   the streaming window has already built. */
static u8 spawnGroundAt(int x, int z, int *ground_y) {
  int y;

  if (!windowColumnResident(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT)) {
    return FALSE;
  }
  for (y = MAX_Y - 3; y >= 0; y--) {
    if (canStandAt(x, y, z)) {
      *ground_y = y;
      return TRUE;
    }
  }
  return FALSE;
}

/*
 * Who lives here.
 *
 * The cottage's own coordinates, hashed with the world seed.  Nothing about
 * the villager is written down because nothing needs to be: the same house in
 * the same world is always the same person.
 */
static u32 houseSeed(int house_x, int house_z) {
  return (u32) house_x * 0x9E3779B9u ^ ((u32) house_z * 0x85EBCA6Bu) ^
    world_seed;
}

static Player *nearestPlayer(Vector3 position, float *out_distance) {
  Player *nearest = NULL;
  float best = 1.0e18f;
  u8 player_num;

  for (player_num = 0; player_num < active_player_count; player_num++) {
    float dx;
    float dz;
    float distance;

    if (!players[player_num].active) {
      continue;
    }
    dx = players[player_num].position.x - position.x;
    dz = players[player_num].position.z - position.z;
    distance = dx * dx + dz * dz;
    if (distance < best) {
      best = distance;
      nearest = &players[player_num];
    }
  }
  *out_distance = best;
  return nearest;
}

static u8 houseTaken(int house_x, int house_z) {
  u8 index;

  for (index = 0; index < MAX_VILLAGERS; index++) {
    if (villagers[index].active && villagers[index].home_x == house_x &&
        villagers[index].home_z == house_z) {
      return TRUE;
    }
  }
  return FALSE;
}

/*
 * Where a villager stands when their cottage wakes up.
 *
 * Five blocks clear of the centre, which is one past the wall, and each side
 * tried in turn: a cottage may have the well, a path, another cottage or the
 * hillside it was cut into on any given side of it, and the generation
 * harness found exactly that -- one cottage in twenty-seven with nothing
 * standable to its east.  Trying one side and giving up would silently leave
 * that house empty.
 */
static const s8 doorstep_offsets[4][2] = {
  {5, 0}, {-5, 0}, {0, 5}, {0, -5}
};

static u8 doorstepFor(int house_x, int house_z, int *stand_x, int *stand_z,
    int *ground_y) {
  u8 side;

  for (side = 0; side < 4; side++) {
    int x = house_x + doorstep_offsets[side][0];
    int z = house_z + doorstep_offsets[side][1];

    if (spawnGroundAt(x, z, ground_y)) {
      *stand_x = x;
      *stand_z = z;
      return TRUE;
    }
  }
  return FALSE;
}

static void pickTarget(Villager *villager, u8 night) {
  int span = VILLAGER_LEASH * 2 + 1;

  villager->decision_time = VILLAGER_DECISION_MIN +
    (float) random((u32) VILLAGER_DECISION_VARIATION);
  /* After dark everybody goes back to their own door and stays there.  It is
     the cheapest schedule that reads as a day: a village that empties at
     dusk, and is busy again in the morning. */
  if (night) {
    villager->state = VILLAGER_HOME;
    villager->target_x = villager->home_x;
    villager->target_z = villager->home_z;
    return;
  }
  villager->state = random(4) == 0 ? VILLAGER_IDLE : VILLAGER_WANDER;
  villager->target_x = villager->home_x + (int) random((u32) span) -
    VILLAGER_LEASH;
  villager->target_z = villager->home_z + (int) random((u32) span) -
    VILLAGER_LEASH;
}

/*
 * One villager per cottage, and only for a cottage the player has walked
 * within sight of.  A hamlet is four houses at most, so the pool is never
 * asked for more than it has.
 */
static u8 trySpawn(void) {
  u8 player_num;

  for (player_num = 0; player_num < active_player_count; player_num++) {
    Player *player = &players[player_num];
    int block_x;
    int block_z;
    u8 house;

    if (!player->active) {
      continue;
    }
    block_x = floor(player->position.x / BLOCK_SIZE);
    block_z = floor(player->position.z / BLOCK_SIZE);

    for (house = 0; house < WORLD_HAMLET_HOUSES; house++) {
      int house_x;
      int house_z;
      int stand_x;
      int stand_z;
      int ground_y;
      float dx;
      float dz;
      float distance;
      u8 index;

      if (!worldHamletHouse(block_x, block_z, house, &house_x, &house_z)) {
        continue;
      }
      if (houseTaken(house_x, house_z)) {
        continue;
      }
      dx = (house_x + .5f) * BLOCK_SIZE - player->position.x;
      dz = (house_z + .5f) * BLOCK_SIZE - player->position.z;
      distance = dx * dx + dz * dz;
      if (distance < VILLAGER_SPAWN_NEAR * VILLAGER_SPAWN_NEAR ||
          distance > VILLAGER_SPAWN_FAR * VILLAGER_SPAWN_FAR) {
        continue;
      }
      if (!doorstepFor(house_x, house_z, &stand_x, &stand_z, &ground_y)) {
        continue;
      }
      for (index = 0; index < MAX_VILLAGERS; index++) {
        Villager *villager = &villagers[index];

        if (villager->active) {
          continue;
        }
        villager->position.x = (stand_x + .5f) * BLOCK_SIZE;
        villager->position.y = (float) (ground_y + 1) * BLOCK_SIZE;
        villager->position.z = (stand_z + .5f) * BLOCK_SIZE;
        villager->yaw = (float) random(360);
        villager->walk_time = 0;
        villager->gait = 0;
        villager->ground_y = (s8) ground_y;
        villager->home_x = house_x;
        villager->home_z = house_z;
        villager->active = TRUE;
        pickTarget(villager, dayCycleSunAltitude() < 0);
        return TRUE;
      }
      return FALSE;
    }
  }
  return FALSE;
}

static void updateVillager(Villager *villager, float delta, u8 night) {
  float distance;
  Player *nearest = nearestPlayer(villager->position, &distance);
  float pace = 0;
  float travelled = 0;
  float dx;
  float dz;

  if (nearest == NULL) {
    return;
  }
  if (distance > VILLAGER_DESPAWN_DISTANCE * VILLAGER_DESPAWN_DISTANCE) {
    villager->active = FALSE;
    return;
  }

  villager->decision_time -= delta;
  if (villager->decision_time <= 0) {
    pickTarget(villager, night);
  } else if (night && villager->state != VILLAGER_HOME) {
    pickTarget(villager, TRUE);
  }

  dx = (villager->target_x + .5f) * BLOCK_SIZE - villager->position.x;
  dz = (villager->target_z + .5f) * BLOCK_SIZE - villager->position.z;
  if (villager->state != VILLAGER_IDLE &&
      dx * dx + dz * dz > VILLAGER_ARRIVED * VILLAGER_ARRIVED) {
    pace = VILLAGER_WALK_SPEED;
    villager->yaw = directionYaw(dx, dz);
  } else if (villager->state == VILLAGER_HOME) {
    /* Home and staying there until morning. */
    villager->decision_time = VILLAGER_DECISION_MIN;
  }

  /*
   * Being looked at is the whole of the interaction so far, and it is worth
   * more than it sounds: a person who turns their head as you pass is the
   * difference between a village and a set of props.  It costs a comparison
   * and a heading.
   */
  if (distance < VILLAGER_NOTICE_RANGE * VILLAGER_NOTICE_RANGE &&
      pace == 0) {
    float to_x = nearest->position.x - villager->position.x;
    float to_z = nearest->position.z - villager->position.z;

    if (to_x != 0 || to_z != 0) {
      villager->yaw = directionYaw(to_x, to_z);
    }
  }

  if (pace > 0) {
    float radians = villager->yaw * M_DTOR;
    float step_x = -sinf(radians) * pace * delta;
    float step_z = -cosf(radians) * pace * delta;
    float next_x = villager->position.x + step_x;
    float next_z = villager->position.z + step_z;
    int block_x = floor(next_x / BLOCK_SIZE);
    int block_z = floor(next_z / BLOCK_SIZE);
    int ground_y;

    if (groundNear(block_x, block_z, villager->ground_y, &ground_y)) {
      villager->position.x = next_x;
      villager->position.z = next_z;
      villager->ground_y = (s8) ground_y;
      villager->position.y = (float) (ground_y + 1) * BLOCK_SIZE;
      travelled = pace * delta;
    } else {
      /* A wall, a fence or the cottage itself.  Pick somewhere else rather
         than lean on it. */
      pickTarget(villager, night);
    }
  }

  /* Gait eases toward what the feet actually did, so a villager who has
     stopped to look at you stops walking on the spot.  The same signal the
     animals and the roamers carry. */
  villager->gait += ((pace > 0 ? min(1.f, travelled / (pace * delta)) : 0) -
    villager->gait) * min(1.f, delta * VILLAGER_GAIT_EASE);
  villager->walk_time += delta * VILLAGER_STRIDE_RATE * villager->gait;
}

void updateVillagers(float delta) {
  u8 night = dayCycleSunAltitude() < 0;
  u8 index;
  u8 active = 0;

  for (index = 0; index < MAX_VILLAGERS; index++) {
    if (villagers[index].active) {
      updateVillager(&villagers[index], delta, night);
    }
    if (villagers[index].active) {
      active++;
    }
  }

  spawn_time -= delta;
  if (spawn_time <= 0) {
    spawn_time = VILLAGER_SPAWN_DELAY;
    if (trySpawn()) {
      active++;
    }
  }
  /* The village borrows exactly what it is using.  Away from a hamlet that is
     nothing, which is why this costs the animals nothing anywhere else. */
  reserved_slots = active;
}

/* ------------------------------------------------------------------ */
/* Drawing.                                                                 */
/* ------------------------------------------------------------------ */

static u8 villagerVisible(u8 viewer_num, Vector3 point) {
  Vector3 offset = add(point, mul(players[viewer_num].position, -1.f));
  int cx = floor(point.x / (BLOCK_SIZE * CHUNK_SIZE));
  int cz = floor(point.z / (BLOCK_SIZE * CHUNK_SIZE));

  if (dot(offset, offset) >
      VILLAGER_RENDER_DISTANCE * VILLAGER_RENDER_DISTANCE) {
    return FALSE;
  }
  return windowColumnResident(cx, cz) &&
    visible_columns[viewer_num][WINDOW_SLOT(cx, cz)];
}

void villagersDrawForPlayer(u8 viewer_num) {
  u8 drawn[MAX_VILLAGERS];
  u8 index;
  u8 pick;

  for (index = 0; index < MAX_VILLAGERS; index++) {
    drawn[index] = FALSE;
  }

  /* Nearest first out of whatever the shared pool has left, which is the same
     rule the creature pass uses: whoever the player is walking toward is
     worth a slot, and whoever they have walked past is not. */
  for (pick = 0; pick < MAX_VILLAGERS; pick++) {
    u8 best = MAX_VILLAGERS;
    float best_distance = 0;
    const HumanoidPerson *who;
    HumanoidPose pose;
    u8 slot;

    for (index = 0; index < MAX_VILLAGERS; index++) {
      Villager *villager = &villagers[index];
      Vector3 offset;
      float distance;

      if (drawn[index] || !villager->active ||
          !villagerVisible(viewer_num, villager->position)) {
        continue;
      }
      offset = add(villager->position, mul(players[viewer_num].position, -1.f));
      distance = dot(offset, offset);
      if (best == MAX_VILLAGERS || distance < best_distance) {
        best = index;
        best_distance = distance;
      }
    }
    if (best == MAX_VILLAGERS) {
      break;
    }
    drawn[best] = TRUE;
    slot = humanoidClaimSlot();
    if (slot == HUMANOID_NO_SLOT) {
      break;
    }
    who = humanoidPersonFromSeed(houseSeed(villagers[best].home_x,
      villagers[best].home_z));
    humanoidWalkPose(&pose, villagers[best].position, villagers[best].yaw,
      villagers[best].yaw, villagers[best].walk_time, villagers[best].gait);
    humanoidDraw(slot, &who->look, &pose);
  }
}
