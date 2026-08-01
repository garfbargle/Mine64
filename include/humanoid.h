#ifndef HUMANOID_H
#define HUMANOID_H

#include <nusys.h>
#include "math.h"
#include "player.h"

/*
 * People.
 *
 * One body, one gait and one draw path for everybody in the world who is a
 * person: the players, 64MON's trainers, and the villagers in the hamlets.
 * Before this the player was seven boxes with their colours baked into the
 * vertices, and a trainer was drawSteve's measurements copied by hand into
 * the creature rig table -- two representations of the same body, kept in
 * agreement by nothing at all.
 *
 * Three things make one path serve all three:
 *
 * 1. GEOMETRY IS SHARED AND COLOURLESS.  The boxes below carry shading only
 *    -- white on the lit face, grey on the far one -- and the garment colour
 *    arrives as the primitive colour, which the entity combiner already
 *    multiplies against shade.  So a hundred people would cost one set of
 *    vertices, and the per-person cost of looking different is one RDP
 *    colour per part rather than eight vertex writes.
 *
 * 2. POSE IS DATA, NOT A PLAYER.  humanoidDraw takes angles, so whatever
 *    computes them -- a controller, a wandering roamer, a villager on its way
 *    home -- gets the same swinging limbs and the same head that turns
 *    independently of the shoulders.
 *
 * 3. MATRICES COME FROM ONE POOL.  Players hold the first slots because they
 *    must always be drawn; everyone else claims what is left, nearest first,
 *    the way the creature pass already picks which two roamers to draw.  The
 *    pool is the entire budget of this feature: the count below is how many
 *    people can share a viewport, and it is the only number that has to grow
 *    for a busier world.
 */

/* Anchors.  Seven are the body; the eighth is the held tool, and the ninth
   is hair that is long enough to have its own motion. */
#define HUMANOID_BODY 0
#define HUMANOID_HEAD 1
#define HUMANOID_LEFT_ARM 2
#define HUMANOID_RIGHT_ARM 3
#define HUMANOID_LEFT_LEG 4
#define HUMANOID_RIGHT_LEG 5
#define HUMANOID_HAND 6
#define HUMANOID_HAIR 7
#define HUMANOID_PART_COUNT 8

/*
 * Hair.
 *
 * The face sheet paints a hairline and sideburns flat onto the front of the
 * head; on its own that is a fringe drawn on a bald skull, so every style
 * here starts with a box above the crown and every box overlaps its
 * neighbours.  A style that stopped level with the head would leave a line of
 * bare scalp between the crown and the painted fringe -- invisible from
 * straight on, and the first thing the eye finds from anywhere else.
 */
#define HUMANOID_HAIR_SHORT 0
#define HUMANOID_HAIR_LONG 1
#define HUMANOID_HAIR_STYLES 2

typedef struct {
  u8 shirt[3];
  u8 trousers[3];
  u8 skin[3];
  u8 hair[3];
  u8 style;
} HumanoidLook;

/*
 * Somebody, rather than some body: a name to put over their head and what
 * they look like.  Trainers and villagers both draw from this table, so the
 * same person can be met in a hamlet in one world and on the road with a team
 * of creatures in another.
 */
typedef struct {
  /* Five characters.  A badge sizes itself to its contents and still has to
     fit inside a quarter-screen viewport in four-player. */
  const char *name;
  HumanoidLook look;
} HumanoidPerson;

#define HUMANOID_PEOPLE 8
extern const HumanoidPerson humanoid_people[HUMANOID_PEOPLE];
/* Everyone the world spawns is a pure function of a seed, so nobody's
   appearance or name is ever written down: the same coordinate meets the same
   person, in the same clothes, every time. */
const HumanoidPerson *humanoidPersonFromSeed(u32 seed);

/* What the player wears.  The one look that is not chosen by a seed. */
extern const HumanoidLook humanoid_player_look;

/*
 * A pose, in the units the model is authored in: `position` is the ground the
 * feet stand on, not the eye.
 *
 * Every angle is degrees, and the body and head are separate on purpose --
 * a person looks at you by turning their head, and turns their shoulders only
 * when they walk.
 */
typedef struct {
  Vector3 position;
  float body_yaw;
  float head_yaw;
  float head_pitch;
  float left_arm_pitch;
  float right_arm_pitch;
  float left_leg_pitch;
  float right_leg_pitch;
  /* Sideways shove, for being hit. */
  float bob;
  /* Drives the hair's swing; the limb angles are passed in already swung. */
  float walk_time;
  /* AIR for empty hands. */
  u8 held_item;
} HumanoidPose;

/* The ordinary walking pose: arms and legs swinging in opposition, scaled by
   how fast the walker is actually moving.  `swing` is 0 for a standing
   person and 1 for a walking one. */
void humanoidWalkPose(HumanoidPose *pose, Vector3 ground, float body_yaw,
  float head_yaw, float walk_time, float swing);

/*
 * The slot pool.  Players are reserved the low slots and are never displaced;
 * everybody else claims from what is left, per viewport, nearest first.
 *
 * Two NPC slots, and the count is RDRAM rather than frame time: a slot is
 * eight anchors of two matrices, double-buffered, which is two kilobytes of
 * the hundred-odd the whole game has spare, and the audio variant has none of
 * those to give.  Two is also what the creature pass allows itself, so a
 * person and an animal cost the same to have standing in front of you.
 *
 * Raising it is the one dial that makes a busier world: see docs/ram-budget.md
 * before turning it.
 */
#define HUMANOID_NPC_SLOTS 2
#define HUMANOID_SLOTS (MAX_PLAYERS + HUMANOID_NPC_SLOTS)
#define HUMANOID_NO_SLOT 0xFF

/* Hand the NPC slots back at the top of a viewport's entity pass. */
void humanoidBeginViewport(void);
/* The next free NPC slot, or HUMANOID_NO_SLOT once they are all spoken for.
   Callers claim in priority order, so whoever asks first is whoever the
   player most needs to see. */
u8 humanoidClaimSlot(void);

/*
 * One person into the display list.
 *
 * Callers must already have set the entity combiner and the day tint, which
 * this restores on the way out: a person is drawn inside the same pass as the
 * mobs and the creatures, and leaves it as they found it.
 */
void humanoidDraw(u8 slot, const HumanoidLook *look, const HumanoidPose *pose);

#endif /* HUMANOID_H */
