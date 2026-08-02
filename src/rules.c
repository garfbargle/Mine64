#include "rules.h"
#include "mods.h"

WorldRules world_rules = {RULE_MONSTERS_MANY, TRUE};

void resetWorldRules(void) {
  world_rules.monsters = worldModOn(MOD_PEACEFUL) ?
    RULE_MONSTERS_NONE : RULE_MONSTERS_MANY;
  world_rules.survival = TRUE;
}

u32 packedWorldRules(void) {
  return (u32) world_rules.monsters | ((u32) (world_rules.survival ? 1 : 0) << 8);
}

void setWorldRules(u32 packed) {
  u8 monsters = (u8) (packed & 0xFF);

  world_rules.monsters = monsters < RULE_MONSTERS_COUNT ?
    monsters : RULE_MONSTERS_MANY;
  world_rules.survival = ((packed >> 8) & 1) != 0;
}

const char *monsterRuleName(u8 level) {
  switch (level) {
    case RULE_MONSTERS_NONE:
      return "NONE";
    case RULE_MONSTERS_FEW:
      return "FEW";
    default:
      return "MANY";
  }
}
