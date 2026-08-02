#ifndef RULES_H
#define RULES_H

#include <nusys.h>

/*
 * World rules: the few choices about a world that can still be changed after
 * it has been made.
 *
 * The mods in mods.h are the ones that cannot.  Generation is a pure function
 * of (coordinate, seed, mask), and terrain outside the saved extent is thrown
 * away and regenerated from those three whenever the player walks back to it
 * -- so a mod that moved would grow a different world onto the edge of this
 * one, permanently, with the old columns already gone.  That is the whole
 * reason the setup card locks.
 *
 * A rule is the other kind of choice.  Nothing here is read by the generator;
 * every one of them is read while the world is played, on the frame it
 * matters, so changing one changes the world the player is standing in and
 * nothing that has already been written down.  That is the test for anything
 * that wants to join this struct -- not "is it gameplay", but "would a column
 * built before the change and a column built after it disagree".
 */

/*
 * How much of the night's monster budget a world is willing to spend.
 *
 * The Moon already decides how full a given night is (see nightHostileBudget
 * in mobs.c); this scales whatever it decided rather than replacing it, so
 * FEW keeps the eight-day shape -- a full Moon is still the worse night --
 * instead of flattening every night to the same count.
 */
#define RULE_MONSTERS_NONE 0
#define RULE_MONSTERS_FEW 1
#define RULE_MONSTERS_MANY 2
#define RULE_MONSTERS_COUNT 3

typedef struct {
  u8 monsters;
  /*
   * Hunger drains, and things can hurt you.  Off is the building rule: it
   * stops those two costs and nothing else.  Blocks still have to be found,
   * mined and carried, because that is the game rather than a difficulty --
   * see docs, and the note on the world tab, before this grows into a flight
   * key and an infinite palette.
   */
  u8 survival;
} WorldRules;

extern WorldRules world_rules;

/*
 * Back to what this world was created asking for.  MOD_PEACEFUL is the
 * starting value of the monster rule rather than a second switch beside it,
 * so a world built peaceful comes up peaceful and can then be argued with.
 * Call after setWorldMods, which it reads.
 */
void resetWorldRules(void);

/* The pair as one word for the save header, and back.  Unknown values clamp
   rather than being trusted: this arrives from a file. */
u32 packedWorldRules(void);
void setWorldRules(u32 packed);

/* Short enough for a value column, upper case for the font atlas. */
const char *monsterRuleName(u8 level);

#endif /* RULES_H */
