#ifndef VILLAGERS_H
#define VILLAGERS_H

#include <nusys.h>
#include "math.h"

/*
 * The people who live in the hamlets.
 *
 * A cottage the generator stamps is a house nobody lives in, which reads as a
 * ruin rather than a home.  These are the difference, and they are deliberately
 * the cheapest inhabitants that still read as inhabitants:
 *
 * 1. THEY ARE NOT STORED.  A villager is a pure function of the cottage they
 *    belong to -- which person, wearing what, standing where -- exactly as a
 *    64MON trainer is a function of their spawn coordinate.  Walking away and
 *    coming back meets the same villager at the same house, and the save file
 *    never hears about it.
 *
 * 2. THEY DO NOT GROW THE ENTITY COUNT.  The pool below is taken out of the
 *    passive mob budget while a hamlet is in range, the way roamers are: a
 *    village trades sheep for villagers rather than adding to what four-player
 *    frames were sized around.  Away from a hamlet the reserve is zero and
 *    nothing changes.
 *
 * 3. THEY ARE PEOPLE, NOT A SPECIES.  The body, the gait and the matrices are
 *    the player's own, out of humanoid.c.  This file owns only where they are
 *    and what they are doing.
 *
 * What they are not, yet, is anybody you can talk to or trade with.  That is a
 * design question rather than a modelling one, and inventing an economy is not
 * something to do by accident.
 */

#define MAX_VILLAGERS 4

typedef struct {
  Vector3 position;
  float yaw;
  float walk_time;
  /* How much of a walking pace the body is making, for the limb swing; the
     same signal the mobs and the roamers carry. */
  float gait;
  float decision_time;
  /* Where they are headed, in block coordinates. */
  int target_x;
  int target_z;
  /* The cottage they belong to.  Identity comes from this, so it is also
     their name and their clothes. */
  int home_x;
  int home_z;
  s8 ground_y;
  u8 active;
  u8 state;
} Villager;

extern Villager villagers[MAX_VILLAGERS];

void initVillagers(void);
/* One simulation slice: spawning, wandering, and going home after dark.
   Called beside updateMobs. */
void updateVillagers(float delta);
/* How many mob slots the village is borrowing.  Zero when no hamlet is near
   any player, which is almost everywhere. */
u8 villagerReserve(void);
/* Draws whoever is close enough to be worth a slot, nearest first.  Leaves
   the entity pass configured as it found it. */
void villagersDrawForPlayer(u8 viewer_num);

#endif /* VILLAGERS_H */
