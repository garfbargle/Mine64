#ifndef MOBS_H
#define MOBS_H

#include <nusys.h>
#include "math.h"
#include "player.h"

#define MAX_MOBS 8
#define MOB_MAX_HEALTH 12

/* The draw path deliberately shows fewer mobs than the simulation owns.  The
   AI pool stays at eight so adding species never turns into an unbounded CPU
   or matrix cost on four-player frames. */
#define MOB_PASSIVE_BUDGET 5
#define MOB_NIGHT_HOSTILE_BUDGET 3
#define MOB_SPECIAL_EFFECT_DURATION 24.f

#if MOB_PASSIVE_BUDGET + MOB_NIGHT_HOSTILE_BUDGET > MAX_MOBS
#error "Mob population budgets exceed the fixed entity pool"
#endif

enum MobType {
  MOB_SHEEP,
  MOB_PIG,
  MOB_SLIME,
  MOB_ZOMBIE,
  MOB_SPIDER,
  MOB_TYPE_COUNT
};

enum MobState {
  MOB_IDLE,
  MOB_WANDER,
  MOB_FLEE,
  MOB_CHASE,
  /* Hostile contact damage only lands after this visible anticipation. */
  MOB_WINDUP,
  /* Sunrise sends monsters away before their bounded despawn timer expires. */
  MOB_RETREAT
};

enum MobWeaponSpecial {
  MOB_SPECIAL_NONE,
  MOB_SPECIAL_WOOD_RUSH,
  MOB_SPECIAL_STONE_CLEAVE,
  MOB_SPECIAL_IRON_SHOCKWAVE
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
  /* Cached block beneath the feet.  Movement checks only this height and the
     adjacent step heights instead of scanning all 32 layers every frame. */
  s8 ground_y;
  /* Wind-ups lock one local player so split-screen movement cannot redirect a
     telegraphed strike to somebody else on its final frame. */
  u8 target_player;
} Mob;

/* A tiny transient event lets the renderer sell each move without owning any
   combat rules.  `time` counts down from MOB_SPECIAL_EFFECT_DURATION and
   hit_count is bounded by MAX_MOBS. */
typedef struct {
  Vector3 origin;
  float yaw;
  float time;
  u8 type;
  u8 hit_count;
} MobSpecialEffect;

extern Mob mobs[MAX_MOBS];
extern MobSpecialEffect mob_special_effects[MAX_PLAYERS];

void initMobs();
void updateMobs(float delta);
u8 mobTypeIsHostile(u8 type);

/* Returns TRUE only when an in-range mob received a punch or tool hit.
   Swords retain their higher damage, but an empty hand can always fight. */
u8 punchMob(u8 attacker_num);

/* Trigger the held sword's tier special.  A successful start returns TRUE
   even on a whiff, so controls can consume the press and play its animation.
   Wood rushes forward, stone cleaves a broad arc, and iron emits a short
   wall-occluded shockwave.  Every attack scans only the fixed mob pool. */
u8 useMobWeaponSpecial(u8 attacker_num);

/* Remaining simulation frames; zero means the player's special is ready. */
float mobWeaponSpecialCooldown(u8 attacker_num);

#endif /* MOBS_H */
