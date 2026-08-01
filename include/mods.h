#ifndef MODS_H
#define MODS_H

#include <nusys.h>

/*
 * World mods: the handful of choices a world is made with.
 *
 * They are chosen on the setup card, previewed as the real world before the
 * player commits to a name, written into the save header, and never change
 * again.  Locking them at creation is not a simplification -- it is what
 * makes the world reproducible.  Terrain outside the saved extent is
 * regenerated from (coordinate, seed) whenever the player walks back to it,
 * so every generation hook a mod touches must be a pure function of those
 * two plus this mask.  A mod that could be flipped mid-world would make a
 * column disagree with the one that was meshed and saved.
 *
 * The mask is a bitfield rather than an enum because the two kinds of choice
 * behave differently and want one mechanism: a terrain shape is one-of-four,
 * while the rest are independent switches.  Each table entry names the group
 * it belongs to; selecting inside a group clears the group's other bits, and
 * group zero is "no group", which is what a plain toggle is.
 */

/* Terrain shapes.  Exactly one of these is set. */
#define MOD_CLASSIC   0x0001
#define MOD_ISLANDS   0x0002
#define MOD_SKYLANDS  0x0004
#define MOD_FLAT      0x0008
#define MOD_TERRAIN_MASK 0x000F

/* Independent switches. */
#define MOD_CAVES     0x0010
#define MOD_RUINS     0x0020
#define MOD_FORESTS   0x0040
#define MOD_CRITTERS  0x0080
#define MOD_PEACEFUL  0x0100
#define MOD_BONUS_KIT 0x0200
/*
 * Creature collecting.  It reads as a behaviour switch rather than a terrain
 * one -- it places no blocks and changes no noise -- so a world can be built
 * with or without it from the same seed and the terrain is identical.  What
 * it changes is the ecology: four of the passive mob slots become wild
 * creatures instead of animals, so the entity count does not move either.
 */
#define MOD_COBBLEMON 0x0400

#define MOD_DEFAULTS \
  (MOD_CLASSIC | MOD_CAVES | MOD_RUINS | MOD_FORESTS | MOD_CRITTERS)

/* The group every terrain shape shares.  Any non-zero value would do; the
   table is what assigns it. */
#define MOD_GROUP_TERRAIN 1

typedef struct {
  u16 bit;
  /* 0 for an independent toggle, otherwise the one-of group it belongs to. */
  u8 group;
  /* TRUE when flipping this changes the blocks the generator produces, and
     the preview therefore has to be built again.  The behaviour mods are
     free: they are read while the world is played, not while it is made. */
  u8 regenerates;
  const char *name;
  /*
   * One line, shown under the card while the row is highlighted.
   *
   * Upper case and no punctuation beyond spaces: the menu font atlas holds
   * capitals and digits only, and anything else comes out as a wrong glyph or
   * a hole in the middle of a word.  Keep these inside 40 characters or they
   * run off a 320-pixel screen.
   */
  const char *blurb;
} WorldMod;

#define WORLD_MOD_COUNT 11
/* Terrain shapes occupy the first rows, so the card can draw its two sections
   without a second table. */
#define WORLD_MOD_TERRAIN_COUNT 4

extern const WorldMod world_mod_table[WORLD_MOD_COUNT];

/* The live mask.  Read directly by the generation hooks; written only by
   setWorldMods, which the setup card and the save loader are the two callers
   of. */
extern u16 world_mods;

void setWorldMods(u16 mods);

/*
 * Whether a mod is switched on *and* means anything alongside the rest of the
 * mask.  Skylands has no ground to bury a cave in or stand a ruin on, so it
 * makes those two rows unavailable rather than silently ignoring them -- the
 * card draws an unavailable row dimmed, and the generator asks this rather
 * than the raw bit.  The stored mask keeps whatever the player set, so
 * choosing skylands and changing back does not lose their preferences.
 */
u8 worldModAvailable(u16 bit);
u8 worldModActive(u16 bit);

/* Flip row `index`, applying its group rule.  FALSE when the row is
   unavailable and nothing changed. */
u8 toggleWorldMod(u8 index);
/* TRUE when row `index` is currently on. */
u8 worldModRowOn(u8 index);
/* The name of the selected terrain shape, for the picker's caption. */
const char *worldTerrainName(void);

static __inline__ __attribute__((unused)) u8 worldModOn(u16 bit) {
  return (world_mods & bit) != 0;
}

#endif /* MODS_H */
