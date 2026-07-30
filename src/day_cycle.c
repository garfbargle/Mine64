#include <nusys.h>
#include "day_cycle.h"

#define DAY_CYCLE_TICK_USEC 50000UL

static u32 world_ticks = DAY_CYCLE_START_TICK;
static OSTime last_time;
static u32 fractional_usec;

static u8 mixChannel(u8 a, u8 b, float amount) {
  float value = a + (b - a) * amount;
  if (value < 0) return 0;
  if (value > 255) return 255;
  return value;
}

static SkyColor mixColor(SkyColor a, SkyColor b, float amount) {
  SkyColor result = {
    mixChannel(a.r, b.r, amount),
    mixChannel(a.g, b.g, amount),
    mixChannel(a.b, b.b, amount)
  };
  return result;
}

static SkyColor horizonColor(u32 time) {
  SkyColor night = {7, 13, 36};
  SkyColor dawn = {190, 105, 73};
  SkyColor day = {124, 190, 255};
  SkyColor sunset = {210, 98, 58};

  if (time < 1000) {
    return mixColor(dawn, day, time / 1000.f);
  }
  if (time < 11000) {
    return day;
  }
  if (time < 12000) {
    return mixColor(day, sunset, (time - 11000) / 1000.f);
  }
  if (time < 13000) {
    return mixColor(sunset, night, (time - 12000) / 1000.f);
  }
  if (time < 23000) {
    return night;
  }
  return mixColor(night, dawn, (time - 23000) / 1000.f);
}

void initDayCycle() {
  world_ticks = DAY_CYCLE_START_TICK;
  fractional_usec = 0;
  last_time = osGetTime();
}

void updateDayCycle() {
  OSTime now = osGetTime();
  u32 elapsed_usec;
  u32 elapsed_ticks;

  if (last_time == 0) {
    last_time = now;
    return;
  }
  elapsed_usec = (u32) OS_CYCLES_TO_USEC(now - last_time);
  last_time = now;
  fractional_usec += elapsed_usec;
  elapsed_ticks = fractional_usec / DAY_CYCLE_TICK_USEC;
  fractional_usec %= DAY_CYCLE_TICK_USEC;
  world_ticks += elapsed_ticks;
}

void pauseDayCycle() {
  last_time = osGetTime();
}

u32 dayCycleWorldTicks() {
  return world_ticks;
}

void setDayCycleWorldTicks(u32 ticks) {
  world_ticks = ticks;
  fractional_usec = 0;
  last_time = osGetTime();
}

u32 dayCycleTimeOfDay() {
  return world_ticks % DAY_CYCLE_TICKS;
}

u8 dayCycleMoonPhase() {
  return (world_ticks / DAY_CYCLE_TICKS) & 7;
}

Vector3 dayCycleSunDirection() {
  float angle = dayCycleTimeOfDay() * 360.f / DAY_CYCLE_TICKS;
  Vector3 direction = {cosf(angle * M_DTOR), sinf(angle * M_DTOR), 0};
  return direction;
}

Vector3 dayCycleLightDirection() {
  Vector3 direction = dayCycleSunDirection();
  if (direction.y < 0) {
    direction = mul(direction, -1.f);
  }
  return direction;
}

SkyColor dayCycleSkyColor(u8 horizon_amount) {
  SkyColor horizon = horizonColor(dayCycleTimeOfDay());
  SkyColor zenith = {4, 12, 38};

  /* The short, blue zenith-to-horizon gradient is a cheap substitute for a
     sky dome and avoids a large fill-rate cost on split-screen. */
  return mixColor(zenith, horizon, horizon_amount / 255.f);
}

SkyColor dayCycleAmbientLight() {
  Vector3 sun = dayCycleSunDirection();
  float daylight = sun.y > 0 ? sun.y : 0;
  SkyColor night = {19, 25, 50};
  /* This is the light on faces turned away from the Sun.  Keep enough
     daylight fill that terrain shadows stay readable instead of looking
     nearly black at midday. */
  SkyColor day = {142, 160, 182};
  return mixColor(night, day, daylight);
}

SkyColor dayCycleDirectLight() {
  Vector3 sun = dayCycleSunDirection();
  float daylight = sun.y > 0 ? sun.y : 0;
  SkyColor moon = {18, 28, 54};
  SkyColor dawn = {255, 146, 94};
  SkyColor noon = {255, 244, 210};

  if (daylight <= 0) {
    return moon;
  }
  /* Low sun is warm; it fades naturally toward pale midday illumination. */
  return mixColor(dawn, noon, daylight);
}
