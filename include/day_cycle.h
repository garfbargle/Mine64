#ifndef DAY_CYCLE_H
#define DAY_CYCLE_H

#include <nusys.h>
#include "math.h"

/* Minecraft's clock has 24,000 ticks: 20 ticks per real second. */
#define DAY_CYCLE_TICKS 24000UL
#define DAY_CYCLE_START_TICK 1000UL

typedef struct {
  u8 r;
  u8 g;
  u8 b;
} SkyColor;

void initDayCycle();
void updateDayCycle();
void pauseDayCycle();

u32 dayCycleWorldTicks();
void setDayCycleWorldTicks(u32 ticks);
u32 dayCycleTimeOfDay();
u8 dayCycleMoonPhase();

/* +X is east in Mine64's world space.  The Sun rises there at tick zero. */
Vector3 dayCycleSunDirection();
Vector3 dayCycleLightDirection();
SkyColor dayCycleSkyColor(u8 horizon_amount);
SkyColor dayCycleAmbientLight();
SkyColor dayCycleDirectLight();

#endif /* DAY_CYCLE_H */
