#include "mods.h"

u16 world_mods = MOD_DEFAULTS;

/*
 * Row order is screen order: the four terrain shapes first, then the
 * switches.  The blurbs are the only documentation the game has for any of
 * this, so they say what the player will see rather than what the code does.
 */
const WorldMod world_mod_table[WORLD_MOD_COUNT] = {
  {MOD_CLASSIC,   MOD_GROUP_TERRAIN, TRUE, "CLASSIC",
    "HILLS LAKES AND WINDING RIVERS"},
  {MOD_ISLANDS,   MOD_GROUP_TERRAIN, TRUE, "ISLANDS",
    "AN OCEAN SCATTERED WITH ISLANDS"},
  {MOD_SKYLANDS,  MOD_GROUP_TERRAIN, TRUE, "SKYLANDS",
    "ISLANDS ADRIFT ABOVE THE SEA"},
  {MOD_FLAT,      MOD_GROUP_TERRAIN, TRUE, "FLAT",
    "A LEVEL CANVAS TO BUILD ON"},
  {MOD_CAVES,     0, TRUE,  "CAVES",
    "TUNNELS AND CAVERNS UNDERGROUND"},
  {MOD_RUINS,     0, TRUE,  "RUINS",
    "HAMLETS AND RUINS TO DISCOVER"},
  {MOD_FORESTS,   0, TRUE,  "FORESTS",
    "TREES GROW ACROSS THE LAND"},
  {MOD_CRITTERS,  0, FALSE, "CRITTERS",
    "CREATURES GATHER AND FOLLOW YOU"},
  {MOD_PEACEFUL,  0, FALSE, "PEACEFUL",
    "NOTHING HOSTILE AFTER DARK"},
  {MOD_BONUS_KIT, 0, FALSE, "BONUS KIT",
    "START WITH TOOLS AND TORCHES"},
  /* Behaviour, not terrain: it spawns creatures where the animals would have
     been, so the world it is switched on in is block-for-block the world it
     is switched off in. */
  {MOD_64MON, 0, FALSE, "64MON",
    "CATCH RAISE AND BATTLE CREATURES"}
};

void setWorldMods(u16 mods) {
  /* A mask with no terrain bit -- an older save, or a corrupt header --
     would make every shape test false and generate nothing at all. */
  if ((mods & MOD_TERRAIN_MASK) == 0) {
    mods |= MOD_CLASSIC;
  }
  world_mods = mods;
}

u8 worldModAvailable(u16 bit) {
  if (bit == MOD_CAVES || bit == MOD_RUINS) {
    /* Sky islands are a few blocks thick and stand in open air: there is
       nothing to tunnel through and nowhere to found a hamlet. */
    return !worldModOn(MOD_SKYLANDS);
  }
  return TRUE;
}

u8 worldModActive(u16 bit) {
  return worldModOn(bit) && worldModAvailable(bit);
}

u8 worldModRowOn(u8 index) {
  if (index >= WORLD_MOD_COUNT) {
    return FALSE;
  }
  return worldModOn(world_mod_table[index].bit);
}

u8 toggleWorldMod(u8 index) {
  const WorldMod *mod;

  if (index >= WORLD_MOD_COUNT) {
    return FALSE;
  }
  mod = &world_mod_table[index];
  if (!worldModAvailable(mod->bit)) {
    return FALSE;
  }
  if (mod->group == 0) {
    world_mods ^= mod->bit;
    return TRUE;
  }

  /* One of a group.  Re-picking the shape already selected is not an error
     worth a beep -- it just leaves the mask alone, and the caller sees
     nothing changed so the preview is not rebuilt for it. */
  if (worldModOn(mod->bit)) {
    return FALSE;
  }
  {
    u8 other;

    for (other = 0; other < WORLD_MOD_COUNT; other++) {
      if (world_mod_table[other].group == mod->group) {
        world_mods &= ~world_mod_table[other].bit;
      }
    }
  }
  world_mods |= mod->bit;
  return TRUE;
}

const char *worldTerrainName(void) {
  u8 index;

  for (index = 0; index < WORLD_MOD_TERRAIN_COUNT; index++) {
    if (worldModOn(world_mod_table[index].bit)) {
      return world_mod_table[index].name;
    }
  }
  return world_mod_table[0].name;
}
