#ifndef MOBS_H
#define MOBS_H

#include <nusys.h>
#include "math.h"

#define MAX_SHEEP 4
#define SHEEP_MAX_HEALTH 8

/* Sheep are intentionally runtime-only in the first release.  This keeps
   saves compact while establishing the bounded entity update/render path. */
typedef struct {
  Vector3 position; /* Ground contact point, centered horizontally. */
  Vector3 knockback_velocity;
  float yaw;
  float walk_time;
  float turn_time;
  float hurt_time;
  u8 health;
  u8 active;
} Sheep;

extern Sheep sheep[MAX_SHEEP];

void initMobs();
void updateMobs(float delta);

/* Returns TRUE only when an in-range sheep received the sword hit. */
u8 swingSwordAtSheep(u8 attacker_num);

#endif /* MOBS_H */
