#include <nusys.h>
#include "day_cycle.h"

#define DAY_CYCLE_TICK_USEC 50000UL

/*
 * How far the Sun's path leans out of the east-west plane.  See the header:
 * without it the light is coplanar with two of the four vertical block faces
 * for the entire day and half the world's walls never take a direct ray.
 * Kept modest -- a large tilt starts to read as the Sun rising in the
 * south-east rather than as a sky that simply is not on the equator.
 */
#define ORBIT_TILT .34f

static u32 world_ticks = DAY_CYCLE_START_TICK;
static OSTime last_time;
static u32 fractional_usec;

static float clamp01(float v) {
  if (v < 0) return 0;
  if (v > 1) return 1;
  return v;
}

/*
 * Hermite fade.  Every threshold in this file crosses the horizon, and a
 * linear ramp there puts a visible kink in the light at precisely the moment
 * the player is most likely to be watching the sky.
 */
static float smoothFade(float edge0, float edge1, float v) {
  float t = clamp01((v - edge0) / (edge1 - edge0));
  return t * t * (3.f - 2.f * t);
}

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

static u8 scaleChannel(u8 a, float amount) {
  float value = a * amount;
  if (value < 0) return 0;
  if (value > 255) return 255;
  return value;
}

static SkyColor scaleColor(SkyColor a, float amount) {
  SkyColor result = {
    scaleChannel(a.r, amount),
    scaleChannel(a.g, amount),
    scaleChannel(a.b, amount)
  };
  return result;
}

/*
 * How much of the model is on the day side of the terminator.  Everything
 * that has a daytime and a nighttime form crossfades on this single value,
 * so the two never disagree about what time it is.
 */
static float dayAmount(float altitude) {
  return smoothFade(-.12f, .12f, altitude);
}

static SkyColor horizonColor(u32 time, float moonlight) {
  SkyColor night = {7, 13, 36};
  SkyColor dawn = {190, 105, 73};
  SkyColor day = {124, 190, 255};
  SkyColor sunset = {210, 98, 58};

  /* A full Moon lifts the night sky enough that the player can tell it is
     up without looking for it.  A new-Moon night stays properly black. */
  night.r += (u8) (14.f * moonlight);
  night.g += (u8) (18.f * moonlight);
  night.b += (u8) (26.f * moonlight);

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

u8 dayCycleIsNight(void) {
  u32 time = dayCycleTimeOfDay();
  return time >= DAY_CYCLE_NIGHT_START && time <= DAY_CYCLE_NIGHT_END;
}

void dayCycleSkipToDawn(void) {
  u32 day_start = world_ticks - dayCycleTimeOfDay();
  u32 dawn = day_start + DAY_CYCLE_DAWN_TICK;

  /* Already past first light: the next one is tomorrow's.  Going backwards
     here would rerun a stretch of the clock that trees and hunger have
     already been advanced through. */
  if (dawn <= world_ticks) {
    dawn += DAY_CYCLE_TICKS;
  }
  setDayCycleWorldTicks(dawn);
}

u8 dayCycleMoonPhase() {
  return (world_ticks / DAY_CYCLE_TICKS) & 7;
}

float dayCycleSunAltitude() {
  float angle = dayCycleTimeOfDay() * 360.f / DAY_CYCLE_TICKS;
  return sinf(angle * M_DTOR);
}

float dayCycleMoonIllumination() {
  /* The lit width the phase art actually draws, in eighths of the disc.
     Reading it from the same table the tiles came from keeps the light and
     the sprite from drifting apart if the art is ever retuned. */
  static const u8 lit_eighths[8] = {8, 6, 4, 2, 0, 2, 4, 6};
  return lit_eighths[dayCycleMoonPhase()] / 8.f;
}

/* 1 / sqrt(1 + ORBIT_TILT^2), so the tilted direction stays a unit vector and
   the RSP light keeps its full 127-count range.  Precomputed rather than
   derived: this runs several times a frame and libultra has no sqrtf. */
#define ORBIT_SCALE .946771f

Vector3 dayCycleSunDirection() {
  float angle = dayCycleTimeOfDay() * 360.f / DAY_CYCLE_TICKS;
  Vector3 direction = {cosf(angle * M_DTOR) * ORBIT_SCALE,
    sinf(angle * M_DTOR) * ORBIT_SCALE, ORBIT_TILT * ORBIT_SCALE};
  return direction;
}

Vector3 dayCycleMoonDirection() {
  return mul(dayCycleSunDirection(), -1.f);
}

Vector3 dayCycleLightDirection() {
  Vector3 direction = dayCycleSunDirection();

  /* Below the horizon the Sun is not the light source -- the Moon is, and it
     is at the antipode.  Negating the whole vector (rather than folding Y as
     this used to) keeps the tilt pointing at the body actually in the sky. */
  if (dayCycleSunAltitude() < 0) {
    direction = mul(direction, -1.f);
  }
  return direction;
}

float dayCycleShadowStrength() {
  float altitude = dayCycleSunAltitude();
  float day = dayAmount(altitude);
  /* Both fade in above the horizon rather than at it: the first minutes of
     sunlight are grazing and diffuse, and a hard blob under a rising Sun
     reads as a bug. */
  float sun_shadow = smoothFade(.05f, .34f, altitude) * .40f;
  float moon_shadow = smoothFade(.12f, .48f, -altitude) *
    dayCycleMoonIllumination() * .17f;

  return sun_shadow * day + moon_shadow * (1.f - day);
}

SkyColor dayCycleSunTint() {
  SkyColor horizon = {255, 132, 78};
  SkyColor high = {255, 255, 255};

  return mixColor(horizon, high, smoothFade(.02f, .36f, dayCycleSunAltitude()));
}

SkyColor dayCycleMoonTint() {
  SkyColor horizon = {255, 186, 150};
  SkyColor high = {236, 242, 255};

  return mixColor(horizon, high, smoothFade(.02f, .36f, -dayCycleSunAltitude()));
}

SkyColor dayCycleSkyColor(u8 horizon_amount) {
  float altitude = dayCycleSunAltitude();
  float moonlight = smoothFade(0.f, .35f, -altitude) * dayCycleMoonIllumination();
  SkyColor horizon = horizonColor(dayCycleTimeOfDay(), moonlight);
  SkyColor zenith = {4, 12, 38};

  /* The short, blue zenith-to-horizon gradient is a cheap substitute for a
     sky dome and avoids a large fill-rate cost on split-screen. */
  return mixColor(zenith, horizon, horizon_amount / 255.f);
}

SkyColor dayCycleAmbientLight() {
  float altitude = dayCycleSunAltitude();
  float day = dayAmount(altitude);
  float moonlight = smoothFade(0.f, .40f, -altitude) * dayCycleMoonIllumination();
  /*
   * Deliberately lower than the light that opposes it.  Ambient is the fill
   * on faces turned away from the light, so it is the single number that
   * decides whether the world has relief or looks like flat pixel art; the
   * old value was high enough to wash the directional light out at noon.
   */
  SkyColor day_fill = {104, 124, 152};
  /*
   * Night is meant to be dark, not absent.  The old floor of 15 put a grass
   * block at six percent of its texture and the ground simply stopped
   * existing until the Moon came up; this reads as dusk that never quite
   * finishes, which is what a player standing outside at night should see.
   */
  SkyColor night_fill = {52, 59, 82};
  SkyColor moon_fill = {78, 92, 130};
  /*
   * The daytime floor is matched to the night one on purpose.  Ambient is the
   * only term left at the moment the Sun crosses the horizon, so a low
   * daylight fill and a high night fill would step the world's brightness the
   * wrong way across dusk -- faces turned away from the Sun would brighten as
   * it set.  Noon is untouched, which is where washing out the key light
   * would have shown.
   */
  SkyColor lit = scaleColor(day_fill, .55f + .45f * clamp01(altitude));
  SkyColor dark = mixColor(night_fill, moon_fill, moonlight);

  return mixColor(dark, lit, day);
}

SkyColor dayCycleDirectLight() {
  float altitude = dayCycleSunAltitude();
  float day = dayAmount(altitude);
  SkyColor dawn = {255, 146, 94};
  SkyColor noon = {255, 244, 210};
  /* Cool, and far stronger than the flat near-black this used to return: a
     full Moon overhead is meant to actually light the terrain it is over. */
  SkyColor moonlight = {138, 166, 220};
  /* Low Sun is warm; it fades naturally toward pale midday illumination. */
  SkyColor sunlight = mixColor(dawn, noon, smoothFade(.03f, .40f, altitude));
  SkyColor lit = scaleColor(sunlight, .28f + .72f * clamp01(altitude / .5f));
  /*
   * Starlight: a floor under the Moon's share, so a new-Moon night still has
   * a direction to it.  Lit by ambient alone every face takes exactly the
   * same colour and the world flattens into one field of blue -- a wall
   * becomes indistinguishable from the floor it meets, which is a worse way
   * to lose the terrain than the black it used to go.  It is not physically a
   * key light; it is the cheapest thing that keeps edges readable at the
   * darkest hour.  The .30 below (down from .40) keeps a full Moon clearly
   * brighter than this rather than merely different.
   */
  float moon_share = .40f + .60f * dayCycleMoonIllumination();
  SkyColor dark = scaleColor(moonlight,
    clamp01(-altitude) * moon_share * .30f);

  return mixColor(dark, lit, day);
}
