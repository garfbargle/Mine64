#ifndef DAY_CYCLE_H
#define DAY_CYCLE_H

#include <nusys.h>
#include "math.h"

/* Minecraft's clock has 24,000 ticks: 20 ticks per real second. */
#define DAY_CYCLE_TICKS 24000UL
#define DAY_CYCLE_START_TICK 1000UL

/*
 * Night, as one definition rather than as a threshold pair copied wherever it
 * is needed.  Monster spawning, the dawn retreat and sleeping all have to
 * agree about it exactly: a bed that ended the night a few hundred ticks
 * before the monsters accepted it had ended would hand the player a morning
 * with the previous night's hostiles still standing in it.
 */
#define DAY_CYCLE_NIGHT_START 13000UL
#define DAY_CYCLE_NIGHT_END 23000UL
/* Just past the end, so waking is unambiguously morning by every rule above
   rather than sitting on the boundary they all test against. */
#define DAY_CYCLE_DAWN_TICK 23200UL

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

/* The one answer to "is it night", for everything that has to agree. */
u8 dayCycleIsNight(void);

/*
 * Wind the clock forward to first light without ever winding it back.
 *
 * The world clock only counts up -- tree growth, hunger and the mob pool all
 * read it -- so this advances into the following day when it is already past
 * dawn rather than rewinding into one that has been.
 */
void dayCycleSkipToDawn(void);

/*
 * +X is east in Mine64's world space.  The Sun rises there at tick zero.
 *
 * The orbit is deliberately tilted out of the XY plane.  An untilted path
 * carries the Sun through the exact zenith, which means its direction never
 * has a Z component -- so north- and south-facing block faces sit at a
 * permanent 90 degrees to the light and are lit by ambient alone, all day,
 * forever.  A tilted orbit lights all four compass faces over a day and
 * throws shadows diagonally instead of along a single axis.
 */
Vector3 dayCycleSunDirection();
Vector3 dayCycleMoonDirection();

/*
 * Altitude of the Sun as a plain sine, -1 at solar midnight through +1 at
 * noon.  Every intensity in the model keys off this rather than off the
 * direction vector's Y, so the orbit tilt above can be retuned without
 * moving a single threshold.  The Moon's altitude is its negation.
 */
float dayCycleSunAltitude();

/* How much of the Moon's disc is lit, in the eighths the phase art draws:
   0 at new through 8 at full.  Anything deciding a rule rather than a
   brightness -- how many monsters a night is allowed, which of them it sends
   -- should read this rather than scaling the fraction below back up, so the
   rule stays whole numbers and cannot land differently on two consoles. */
u8 dayCycleMoonlitEighths(void);

/* Lit fraction of the Moon's visible disc, 0 at new through 1 at full.  It
   scales moonlight and the shadow the Moon casts, so the sky the player can
   see and the light they are standing in always agree. */
float dayCycleMoonIllumination();

/* Direction toward whichever body is currently lighting the world.  The RSP
   light and the ground shadows both point along this. */
Vector3 dayCycleLightDirection();

/*
 * How dark a cast shadow should be right now, 0..1.  It fades out near the
 * horizon -- a low light casts long, weak, diffuse shadows that a hard blob
 * would misrepresent -- and at a new Moon there is none at all.  A full Moon
 * overhead casts a real, faint shadow, which is the point of the whole
 * moonlight model.
 */
float dayCycleShadowStrength();

/* Vertex tint for the Sun and Moon sprites.  Both redden toward the horizon
   for the same atmospheric reason and both read white overhead. */
SkyColor dayCycleSunTint();
SkyColor dayCycleMoonTint();

SkyColor dayCycleSkyColor(u8 horizon_amount);
SkyColor dayCycleAmbientLight();
SkyColor dayCycleDirectLight();

#endif /* DAY_CYCLE_H */
