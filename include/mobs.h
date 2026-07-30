#ifndef MOBS_H
#define MOBS_H

#include <nusys.h>
#include "math.h"

#define MAX_MOBS 8
#define MOB_MAX_HEALTH 10

enum MobType {
  MOB_SHEEP,
  MOB_PIG,
  MOB_SLIME,
  MOB_TYPE_COUNT
};

enum MobState {
  MOB_IDLE,
  MOB_WANDER,
  MOB_FLEE,
  MOB_CHASE
};

/* A single bounded pool keeps AI and rendering cost predictable as the
   ecology grows.  Species differ through compact type/state bytes rather
   than parallel entity systems. */
typedef struct {
  Vector3 position; /* Ground contact point, centered horizontally. */
  Vector3 knockback_velocity;
  float yaw;
  float walk_time;
  float decision_time;
  float state_time;
  float hurt_time;
  float attack_time;
  u8 health;
  u8 type;
  u8 state;
  u8 active;
} Mob;

extern Mob mobs[MAX_MOBS];

void initMobs();
void updateMobs(float delta);

/* Returns TRUE only when an in-range mob received a punch or tool hit.
   Swords retain their higher damage, but an empty hand can always fight. */
u8 punchMob(u8 attacker_num);

#endif /* MOBS_H */
