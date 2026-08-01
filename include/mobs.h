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
  MOB_CHICKEN,
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

/* Held food turns a herd from scenery into something that reacts to the
   player, so the timers below are gameplay-visible rather than cosmetic.
   All are measured in the same 60 Hz simulation frames as the rest of the
   mob module. */
#define MOB_LOVE_DURATION 420.f
#define MOB_BREED_COOLDOWN 1500.f
#define MOB_BABY_DURATION 3200.f
/* Uniform shrink applied to a baby's geometry and to every part offset, so
   one number keeps a calf's proportions identical to its parent's. */
#define MOB_BABY_SCALE .52f

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
  /* The heading the body is turning toward.  Every behaviour writes this and
     a single bounded turn per frame moves `yaw` toward it, which is what
     keeps a change of mind visible instead of instantaneous. */
  float goal_yaw;
  /* Phase of the limb cycle, in radians, advanced by ground actually covered
     rather than by time.  See MOB_STRIDE_CYCLE. */
  float walk_time;
  /* How much of a full walking pace the body is really making, eased toward
     0 at a standstill and 1 at a walk.  The renderer scales limb swing by it,
     which is what stops an animal that is holding its spacing -- or pressing
     against a wall, or waiting out an attack cooldown -- from walking on the
     spot.  State cannot answer this: a tempted animal holding position and a
     tempted animal walking in are both MOB_CHASE. */
  float gait;
  float decision_time;
  float state_time;
  float hurt_time;
  float attack_time;
  /* Where the head points relative to the body, in degrees, clamped to a
     neck's worth of travel.  Tracking the player here rather than in the
     renderer means one solve per frame instead of one per viewport, and every
     split-screen view agrees about which way the animal is looking. */
  float head_yaw;
  /* Counts down while the animal is seeking a mate; zero otherwise. */
  float love_time;
  /* Counts down after breeding.  Also set on a newborn, so a baby cannot
     breed the instant it grows up. */
  float breed_time;
  /* Counts down to adulthood; zero means a grown animal.  Babies are drawn
     at MOB_BABY_SCALE and never drop meat. */
  float baby_time;
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
  /* Which player this animal belongs to, or MOB_NO_OWNER.  A tamed animal
     follows its owner and, crucially, is remembered rather than deleted when
     it falls out of the simulated radius -- see the livestock roster. */
  u8 owner;
} Mob;

#define MOB_NO_OWNER MAX_PLAYERS
#define MOB_IS_TAMED(mob) ((mob)->owner != MOB_NO_OWNER)

/*
 * The livestock roster: tamed animals that are not currently simulated.
 *
 * The mob pool is ambient.  Anything more than MOB_DESPAWN_DISTANCE from a
 * player is deactivated outright, which is exactly right for the sheep that
 * wander past and exactly wrong for the sheep a player walked home, penned
 * and named theirs -- go mining and the pen would be empty on the way back.
 *
 * So a tamed animal that leaves the radius is not destroyed, it is written
 * down: species, owner, where it was standing, and whether it was a calf.
 * Coming back within range materialises it into a pool slot again.  This is
 * the same bargain villagers and trees already make with this world -- the
 * entity is transient, the record is what persists -- and it costs about a
 * hundred bytes rather than a second entity system.
 *
 * The roster length is also the herd cap, deliberately.  Livestock competes
 * with wild animals for the same eight slots, so an uncapped herd would be a
 * way to empty the world of everything else.
 */
#define MAX_LIVESTOCK 4

typedef struct {
  s32 x;
  s32 z;
  u8 y;
  u8 type;
  u8 owner;
  /* Remaining calf time, quantised to whole seconds so the record stays
     small; a calf put down and picked up again keeps growing. */
  u8 baby_seconds;
  u8 active;
} LivestockRecord;

extern LivestockRecord livestock[MAX_LIVESTOCK];

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

#define MOB_IS_BABY(mob) ((mob)->baby_time > 0)

extern Mob mobs[MAX_MOBS];
extern MobSpecialEffect mob_special_effects[MAX_PLAYERS];

void initMobs();
void updateMobs(float delta);
u8 mobTypeIsHostile(u8 type);

/* The animal this player's held apple is currently offered to, or MAX_MOBS.
   updateMobs resolves it once a frame so the HUD prompt, the A button, and
   the animal's own attention all name the same creature. */
u8 mobFeedTarget(u8 player_num);

/* Spend one held apple on that animal: a baby grows faster, an adult that is
   not on its breeding cooldown starts looking for a mate.  Returns TRUE only
   when an apple was actually consumed. */
u8 feedMob(u8 player_num);

/*
 * TRUE when a hostile is close enough to this player to make lying down a bad
 * idea.  Sleeping is the one action that takes the night away wholesale, so it
 * is the one action that has to check whether the night is currently standing
 * next to the bed.  Scans only the fixed pool.
 */
u8 mobHostileNear(u8 player_num);

/*
 * Take ownership of the animal this player's apple is offered to.
 *
 * The first apple an untamed adult accepts tames it; every apple after that
 * breeds, which is what feeding already did.  One button, one item, and the
 * order the player discovers them in is the order they need them: an animal
 * that follows you home is worth more than a calf you cannot move.
 *
 * FALSE when there was nothing to tame, when the herd is already at
 * MAX_LIVESTOCK, or when the animal is somebody else's.
 */
u8 tameMob(u8 player_num);

/* TRUE when A on the current feed target would claim it rather than feed it,
   so the prompt can name the action the button is actually about to take. */
u8 mobFeedIsTame(u8 player_num);

/* How many animals this player owns, counting both the simulated ones and the
   ones currently folded into the roster.  The HUD uses it for the herd
   readout, and taming uses it as the cap. */
u8 mobOwnedCount(u8 player_num);

/* Returns TRUE only when an in-range mob received a punch or tool hit.
   Swords retain their higher damage, but an empty hand can always fight. */
u8 punchMob(u8 attacker_num);

/* Trigger the held sword's tier special.  A successful start returns TRUE
   even on a whiff, so controls can consume the press and play its animation.
   Wood rushes forward, stone cleaves a broad arc, and iron emits a short
   wall-occluded shockwave.  Every attack scans only the fixed mob pool.
   Bound to L + B: see the swing handler in player.c. */
u8 useMobWeaponSpecial(u8 attacker_num);

/* How far the player's special has recharged, 0 through MOB_SPECIAL_CHARGE_FULL.
   The HUD asks for this rather than for the raw cooldown so the three tiers'
   very different recharge times stay inside mobs.c -- a bar that read the
   frames directly would have to know them to scale itself. */
#define MOB_SPECIAL_CHARGE_FULL 32
u8 mobWeaponSpecialCharge(u8 attacker_num);

#endif /* MOBS_H */
