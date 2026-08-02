#include "mon64.h"

#include <stddef.h>

#include "audio.h"
#include "blocks.h"
#include "day_cycle.h"
#include "graphics.h"
#include "items.h"
#include "menu.h"
#include "mods.h"
#include "rules.h"
#include "world.h"

/*
 * Creature simulation, battles, and the party.
 *
 * The whole feature is one pool, one battle and one array of six-byte
 * records.  Nothing here allocates, nothing here waits, and nothing here
 * uses a float for a rule -- the presentation timers are the only floats in
 * the file, and a dropped frame can only make an animation shorter, never
 * change who won.
 */

/* Roamer behaviour.  Frames, at the 60 Hz delta updatePlayers hands out. */
#define ROAM_WALK_SPEED 0.62f
#define ROAM_FLEE_SPEED 1.34f
#define ROAM_APPROACH_SPEED 0.95f
/* Limb cycle while walking, and the slower drift that keeps breathing and
   tails alive at a standstill.  See MonRoamer::gait for why the second of
   these is not what stops the feet. */
#define ROAM_STRIDE_RATE .22f
#define ROAM_IDLE_RATE .05f
#define ROAM_GAIT_EASE .3f
#define ROAM_DECISION_MIN 60.f
#define ROAM_DECISION_VARIATION 120.f
#define ROAM_DESPAWN_DISTANCE (BLOCK_SIZE * 34.f)
#define ROAM_SPAWN_DELAY 90.f
#define ROAM_FAILED_SPAWN_DELAY 40.f
/* How close the player has to be for A to mean "battle this creature". */
#define ROAM_INTERACT_RANGE (BLOCK_SIZE * 3.4f)
/* An aggressive creature closes to this before it starts the fight itself. */
#define ROAM_AGGRO_RANGE (BLOCK_SIZE * 1.9f)
#define ROAM_NOTICE_RANGE (BLOCK_SIZE * 9.f)
/* A challenge between two players needs them face to face. */
#define PVP_CHALLENGE_RANGE (BLOCK_SIZE * 3.2f)

/* Degrees, in the player's 0..360 pitch space where 270..360 looks down.
   Eighteen degrees is what it takes to lift creatures standing on the ground
   a few blocks away out from behind the message box. */
#define BATTLE_VIEW_PITCH 342.f

#define ROAM_IDLE 0
#define ROAM_WANDER 1
#define ROAM_NOTICE 2
#define ROAM_FLEE 3
#define ROAM_APPROACH 4

/* One health point per party member every four seconds.  Slow enough that a
   hard loss costs real time, fast enough that nobody is ever stranded with a
   fainted team and nothing to do about it -- there is no healing building in
   a world the player may have walked a thousand blocks from. */
#define PARTY_REGEN_INTERVAL 240.f

/* Message pacing: characters revealed per frame, times four. */
#define MESSAGE_REVEAL_RATE 10
/* Frames a fully typed line waits before it will advance on its own.  A is
   always faster; this only stops an unattended battle from stopping dead. */
#define MESSAGE_AUTO_ADVANCE 150

PartyMon mon_party[MAX_PLAYERS][MON_PARTY_SIZE];
MonRoamer mon_roamers[MON_MAX_ROAMERS];
MonBattle mon_battle;

static float roamer_spawn_time;
static float party_regen_time;
static float message_hold;
/* The creature each player is currently close enough to act on, refreshed
   once per simulation step so the prompt and the A button cannot disagree. */
static u8 roamer_target[MAX_PLAYERS];
/* A pending player-versus-player challenge, and who it is waiting on. */
static u8 pvp_challenger = MON_NONE;
static u8 pvp_target = MON_NONE;
static float pvp_challenge_time;

u8 mon64Enabled(void) {
  return worldModActive(MOD_64MON);
}

u8 mon64RoamerReserve(void) {
  return mon64Enabled() ? MON_MAX_ROAMERS : 0;
}

u8 mon64BattleActive(void) {
  return mon_battle.active;
}

/*
 * The footprint a species stands on: how tall it is and how wide a shadow it
 * throws, both from the two numbers its model is already built from.
 *
 * A cub's blob shrinks with the cub.  An adult-sized shadow under a small
 * animal is the tell that gives a scaled model away, which is exactly the
 * trade the mob pool already makes for its calves and lambs.
 */
static void creatureFootprint(u8 species_id, float *height, float *radius) {
  const MonSpecies *species = &mon_species[species_id];

  *height = (float) mon_rigs[species->rig].height * species->scale / 100.f;
  /* Seven tenths of the standing height, widened by bulk: it puts a young
     bear on the same footprint the pig it is sharing a field with already
     has, which is the only calibration this number needs to pass. */
  *radius = *height * .7f * (float) species->bulk / 100.f;
}

/*
 * One creature's ground shadow, whether it is roaming or squared up in a
 * fight.
 *
 * Creatures were the one thing on the ground casting nothing: trees, players,
 * mobs and even dropped items all had a blob, and an animal standing among
 * them with none reads as pasted on rather than as standing there.  This is
 * what the rig's authored height has always been for -- written down six
 * times and never once read.
 *
 * A battle takes over the indices because a battle takes over the pool: the
 * roamer that started it is deactivated the moment it does, and the two
 * fighters are then the only creatures standing in the world.  Keeping both
 * cases behind one call is what lets the shadow pass stay a single loop that
 * knows nothing about either.
 *
 * FALSE for a slot with nothing standing in it.
 */
u8 mon64ShadowCaster(u8 index, Vector3 *position, float *height,
    float *radius) {
  const MonRoamer *roamer;

  if (index >= MON_MAX_ROAMERS || !mon64Enabled()) {
    return FALSE;
  }
  if (mon_battle.active) {
    const MonFighter *fighter;

    if (index >= 2) {
      return FALSE;
    }
    fighter = &mon_battle.fighter[index];
    /* A creature on its way down has already sunk into the floor; its blob
       should go with it rather than outlive it. */
    if (fighter->species >= MON_SPECIES_COUNT || fighter->hp == 0) {
      return FALSE;
    }
    *position = mon_battle.stand[index];
    creatureFootprint(fighter->species, height, radius);
    return TRUE;
  }
  roamer = &mon_roamers[index];
  if (!roamer->active || roamer->species >= MON_SPECIES_COUNT) {
    return FALSE;
  }
  *position = roamer->position;
  if (roamer->kind == MON_ROAMER_TRAINER) {
    /* A trainer is drawn as a person and casts a person's blob.  The species
       they spawned beside is never on screen, so it must not be on the
       ground either. */
    *height = BLOCK_SIZE * .9f;
    *radius = BLOCK_SIZE * .42f;
    return TRUE;
  }
  creatureFootprint(roamer->species, height, radius);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Derived statistics.  Integer, pure, and safe to call from the renderer.   */
/* ------------------------------------------------------------------ */

/*
 * Nothing about a creature is stored except its species, level, experience
 * and current health.  Everything else is recomputed here on demand, which
 * is what makes a balance change safe: retuning a base stat retunes every
 * creature in every existing save, with no migration and no drift between
 * what a party card shows and what a battle uses.
 */
u16 monMaxHealth(u8 species, u8 level) {
  const MonSpecies *s;

  if (species >= MON_SPECIES_COUNT) {
    return 1;
  }
  s = &mon_species[species];
  return (u16) (12 + ((u32) s->base_hp * level) / 10);
}

static u16 statValue(u8 base, u8 level) {
  return (u16) (4 + ((u32) base * level) / 20);
}

u16 monAttack(u8 species, u8 level) {
  return species < MON_SPECIES_COUNT ?
    statValue(mon_species[species].base_attack, level) : 1;
}

u16 monDefense(u8 species, u8 level) {
  return species < MON_SPECIES_COUNT ?
    statValue(mon_species[species].base_defense, level) : 1;
}

u16 monSpeed(u8 species, u8 level) {
  return species < MON_SPECIES_COUNT ?
    statValue(mon_species[species].base_speed, level) : 1;
}

/*
 * Experience needed to leave `level`.  Quadratic, so early levels arrive
 * quickly and the last few are a real investment, and bounded well inside a
 * u16 at level 50 (5600) so the stored progress never has to widen.
 */
u16 monXpForLevel(u8 level) {
  return (u16) (2 * (u32) level * level + 12 * (u32) level);
}

u8 monKnownMoves(u8 species, u8 level, u8 *out) {
  const MonSpecies *s;
  u8 index;
  u8 count = 0;

  if (species >= MON_SPECIES_COUNT) {
    out[0] = 0;
    return 1;
  }
  s = &mon_species[species];
  for (index = 0; index < MON_MOVES; index++) {
    if (s->move_levels[index] <= level) {
      out[count++] = s->moves[index];
    }
  }
  /* A creature always has something to do on its turn.  A learnset whose
     first entry was mis-levelled would otherwise produce a turn the player
     cannot take. */
  if (count == 0) {
    out[0] = s->moves[0];
    count = 1;
  }
  return count;
}

/*
 * The species a family member grew out of, and the level it did so at.
 *
 * Both are walked out of the roster rather than stored in it.  A `min_level`
 * column would be a second place the same fact lives, and eighteen rows is
 * short enough that the scan costs less than the byte would: this runs on a
 * spawn attempt and when a trainer's team is built, never in a frame that is
 * drawing anything.
 */
static u8 monParentSpecies(u8 species) {
  u8 index;

  for (index = 0; index < MON_SPECIES_COUNT; index++) {
    if (mon_species[index].evolves_to == species) {
      return index;
    }
  }
  return MON_NONE;
}

/* The lowest level a creature of this species can honestly be: zero for a
   young form, and its parent's evolution threshold for anything later. */
static u8 monMinLevel(u8 species) {
  u8 parent;

  if (species >= MON_SPECIES_COUNT) {
    return 0;
  }
  parent = monParentSpecies(species);
  return parent == MON_NONE ? 0 : mon_species[parent].evolve_level;
}

u8 monPartyCount(u8 player_num) {
  u8 index;
  u8 count = 0;

  for (index = 0; index < MON_PARTY_SIZE; index++) {
    if (mon_party[player_num][index].species != MON_NONE) {
      count++;
    }
  }
  return count;
}

u8 monPartyLead(u8 player_num) {
  u8 index;

  for (index = 0; index < MON_PARTY_SIZE; index++) {
    PartyMon *mon = &mon_party[player_num][index];
    if (mon->species != MON_NONE && mon->hp > 0) {
      return index;
    }
  }
  return MON_NONE;
}

static u8 addToParty(u8 player_num, u8 species, u8 level) {
  u8 index;

  for (index = 0; index < MON_PARTY_SIZE; index++) {
    PartyMon *mon = &mon_party[player_num][index];
    if (mon->species != MON_NONE) {
      continue;
    }
    mon->species = species;
    mon->level = level;
    mon->xp = 0;
    mon->hp = monMaxHealth(species, level);
    return index;
  }
  return MON_NONE;
}

void initMon64(void) {
  u8 player_num;
  u8 index;

  for (player_num = 0; player_num < MAX_PLAYERS; player_num++) {
    for (index = 0; index < MON_PARTY_SIZE; index++) {
      mon_party[player_num][index].species = MON_NONE;
      mon_party[player_num][index].level = 0;
      mon_party[player_num][index].xp = 0;
      mon_party[player_num][index].hp = 0;
    }
    roamer_target[player_num] = MON_NONE;
  }
  for (index = 0; index < MON_MAX_ROAMERS; index++) {
    mon_roamers[index].active = FALSE;
  }
  mon_battle.active = FALSE;
  mon_battle.phase = MON_PHASE_NONE;
  roamer_spawn_time = ROAM_SPAWN_DELAY;
  party_regen_time = 0;
  pvp_challenger = MON_NONE;
  pvp_target = MON_NONE;
}

/* ------------------------------------------------------------------ */
/* The battle log.                                                          */
/* ------------------------------------------------------------------ */

static char *appendText(char *out, const char *text) {
  while (*text) {
    *out++ = *text++;
  }
  return out;
}

static char *appendNumber(char *out, u32 value) {
  char digits[10];
  u8 count = 0;

  do {
    digits[count++] = (char) ('0' + (value % 10u));
    value /= 10u;
  } while (value > 0 && count < 10);
  while (count > 0) {
    *out++ = digits[--count];
  }
  return out;
}

/*
 * Queue one line.  The caller writes into a scratch buffer and hands it over,
 * because every message in the game is built from at most a name, a number
 * and two fragments -- a formatter would be more machinery than the whole log
 * is worth.  Overflow drops the oldest line rather than the newest: the most
 * recent thing that happened is always the one worth reading.
 */
static void pushMessage(const char *text) {
  u8 slot;
  u8 index = 0;

  if (mon_battle.message_count >= MON_MESSAGE_QUEUE) {
    mon_battle.message_head =
      (u8) ((mon_battle.message_head + 1) % MON_MESSAGE_QUEUE);
    mon_battle.message_count--;
  }
  slot = (u8) ((mon_battle.message_head + mon_battle.message_count) %
    MON_MESSAGE_QUEUE);
  while (text[index] && index < MON_MESSAGE_LENGTH) {
    mon_battle.message[slot][index] = text[index];
    index++;
  }
  mon_battle.message[slot][index] = 0;
  mon_battle.message_count++;
  if (mon_battle.phase != MON_PHASE_MESSAGE) {
    mon_battle.next_phase = mon_battle.phase;
    mon_battle.phase = MON_PHASE_MESSAGE;
    mon_battle.message_reveal = 0;
    message_hold = 0;
  }
}

/* "<name> used <move>!" and friends, built in one shared scratch line. */
static char message_scratch[MON_MESSAGE_LENGTH + 1];

static void pushNamed(const char *name, const char *tail) {
  char *out = message_scratch;

  out = appendText(out, name);
  out = appendText(out, tail);
  *out = 0;
  pushMessage(message_scratch);
}

/*
 * Move to a phase without stepping on a line the player has not read yet.
 *
 * Every transition goes through here.  Assigning mon_battle.phase directly
 * beside a pushMessage is the one mistake this system can make that a player
 * would experience as "the game skipped something" -- and it is invisible in
 * an emulator, because the message would still be in the queue.
 */
static void setPhase(u8 phase) {
  mon_battle.next_phase = phase;
  if (mon_battle.phase != MON_PHASE_MESSAGE) {
    mon_battle.phase = phase;
  }
}

/* ------------------------------------------------------------------ */
/* Fighters.                                                                */
/* ------------------------------------------------------------------ */

static const char *fighterName(u8 side) {
  u8 species = mon_battle.fighter[side].species;

  return species < MON_SPECIES_COUNT ?
    mon_species[species].name : "???";
}

static void loadFighter(u8 side, u8 species, u8 level, u16 hp,
    u8 party_slot) {
  MonFighter *f = &mon_battle.fighter[side];
  u8 index;

  f->species = species;
  f->level = level;
  f->max_hp = monMaxHealth(species, level);
  f->hp = hp > f->max_hp ? f->max_hp : hp;
  f->attack = monAttack(species, level);
  f->defense = monDefense(species, level);
  f->speed = monSpeed(species, level);
  f->move_count = monKnownMoves(species, level, f->move);
  for (index = 0; index < f->move_count; index++) {
    f->pp[index] = mon_moves[f->move[index]].pp;
  }
  for (; index < MON_MOVES; index++) {
    f->pp[index] = 0;
  }
  /* Stat changes belong to the fighter on the field, not to the creature.
     Switching out is meant to cost the buffs it built up. */
  f->attack_stage = 0;
  f->defense_stage = 0;
  f->party_slot = party_slot;
  mon_battle.lunge[side] = 0;
  mon_battle.hurt[side] = 0;
  mon_battle.faint[side] = 0;
}

/*
 * Re-derive a fighter that has just levelled up or evolved, in place.
 *
 * Deliberately not loadFighter.  That rebuilds a fighter from nothing, which
 * across a knockout hands back every point of power the fight has cost and
 * wipes the stat changes it earned -- a free refill after every faint in a
 * trainer battle, and a CHARGE thrown away for winning with it.  A knockout is
 * not a rest, so only the numbers the new level actually changed move here.
 *
 * A move carried over keeps what is left of it; one that arrives with the new
 * level or the new species arrives full.  Matching is by move id rather than
 * by slot, because an evolution reorders the learnset.
 */
static void refreshFighter(u8 side, u8 species, u8 level, u16 hp) {
  MonFighter *f = &mon_battle.fighter[side];
  u8 move[MON_MOVES];
  u8 pp[MON_MOVES];
  u8 count;
  u8 index;
  u8 old;

  count = monKnownMoves(species, level, move);
  for (index = 0; index < count; index++) {
    pp[index] = mon_moves[move[index]].pp;
    for (old = 0; old < f->move_count; old++) {
      if (f->move[old] == move[index]) {
        pp[index] = f->pp[old];
        break;
      }
    }
  }
  for (index = 0; index < MON_MOVES; index++) {
    f->move[index] = index < count ? move[index] : 0;
    f->pp[index] = index < count ? pp[index] : 0;
  }
  f->move_count = count;
  f->species = species;
  f->level = level;
  f->max_hp = monMaxHealth(species, level);
  f->hp = hp > f->max_hp ? f->max_hp : hp;
  f->attack = monAttack(species, level);
  f->defense = monDefense(species, level);
  f->speed = monSpeed(species, level);
}

/* Write a player side's remaining health back to the party record.  Called
   whenever a fighter leaves the field, so the party is never a turn behind
   what the battle shows. */
static void storeFighter(u8 side) {
  MonFighter *f = &mon_battle.fighter[side];

  if (!mon_battle.side_is_player[side] ||
      f->party_slot >= MON_PARTY_SIZE) {
    return;
  }
  mon_party[mon_battle.side_player[side]][f->party_slot].hp = f->hp;
}

static u16 stagedStat(u16 base, s8 stage) {
  if (stage > 0) {
    return (u16) (((u32) base * (2 + stage)) / 2);
  }
  if (stage < 0) {
    return (u16) (((u32) base * 2) / (2 - stage));
  }
  return base;
}

static u8 typeMultiplier(u8 move_type, u8 defender_species) {
  u8 defender_type;

  if (move_type >= MON_TYPE_COUNT ||
      defender_species >= MON_SPECIES_COUNT) {
    return 4;
  }
  defender_type = mon_species[defender_species].type;
  return mon_type_chart[move_type][defender_type];
}

/* ------------------------------------------------------------------ */
/* Encounters.                                                              */
/* ------------------------------------------------------------------ */

static u8 groundBlockAt(int x, int y, int z) {
  return blockGet(x, y, z);
}

/*
 * Facing, as the mob pool defines it: yaw 0 looks toward -Z and increases
 * anticlockwise, so a model's nose -- local -Z in every rig -- ends up along
 * (-sin yaw, -cos yaw), which is also the direction the player walks in.
 * Four-way is all a box creature can express and it costs no arc tangent.
 */
static float facingYaw(float dx, float dz) {
  float ax = dx < 0 ? -dx : dx;
  float az = dz < 0 ? -dz : dz;

  if (ax > az) {
    return dx > 0 ? 270.f : 90.f;
  }
  return dz > 0 ? 180.f : 0.f;
}

/* Drop a battle position onto the ground beneath it, so a creature standing
   just off a ledge or on a slope does not float or sink into it. */
static float groundHeightNear(float world_x, float world_z, float around_y) {
  int block_x = floor(world_x / BLOCK_SIZE);
  int block_z = floor(world_z / BLOCK_SIZE);
  int start = floor(around_y / BLOCK_SIZE) + 2;
  int y;

  if (start >= MAX_Y) {
    start = MAX_Y - 1;
  }
  for (y = start; y >= 0; y--) {
    u8 block = groundBlockAt(block_x, y, block_z);
    if (block != BLOCK_NOT_RESIDENT && BLOCK_IS_SOLID(block)) {
      return (float) (y + 1) * BLOCK_SIZE;
    }
  }
  return around_y;
}

static u8 roamerCanStandAt(int x, int ground_y, int z) {
  u8 ground;

  if ((u32) ground_y >= (u32) (MAX_Y - 2)) {
    return FALSE;
  }
  ground = groundBlockAt(x, ground_y, z);
  if (!BLOCK_IS_SOLID(ground) || ground == LEAVES) {
    return FALSE;
  }
  return groundBlockAt(x, ground_y + 1, z) == AIR &&
    groundBlockAt(x, ground_y + 2, z) == AIR;
}

static u8 roamerGroundNear(int x, int z, int current_y, int *ground_y) {
  static const s8 offsets[3] = {0, 1, -1};
  u8 index;

  if (!windowColumnResident(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT)) {
    return FALSE;
  }
  for (index = 0; index < 3; index++) {
    int y = current_y + offsets[index];
    if (roamerCanStandAt(x, y, z)) {
      *ground_y = y;
      return TRUE;
    }
  }
  return FALSE;
}

/* The full-height scan is reserved for the occasional spawn attempt; a live
   roamer only ever asks the three-cell question above. */
static u8 roamerSpawnGroundAt(int x, int z, int *ground_y) {
  int y;

  if (!windowColumnResident(x >> CHUNK_SHIFT, z >> CHUNK_SHIFT)) {
    return FALSE;
  }
  for (y = MAX_Y - 3; y >= 0; y--) {
    if (roamerCanStandAt(x, y, z)) {
      *ground_y = y;
      return TRUE;
    }
  }
  return FALSE;
}

/* Which habitats a given spawn pad satisfies.  Water within two blocks makes
   a pad a shore, which is the only habitat that needs to look past its own
   cell -- and it looks at four cells, not a radius. */
static u8 habitatAt(int x, int ground_y, int z) {
  u8 ground = groundBlockAt(x, ground_y, z);
  u8 mask = 0;

  if (ground == GRASS || ground == DIRT) {
    mask |= MON_HAB_GRASS;
  } else if (ground == SAND) {
    mask |= MON_HAB_SAND;
  } else if (ground == STONE || ground == COBBLESTONE ||
      ground == MOSSY_COBBLESTONE) {
    mask |= MON_HAB_STONE;
  }
  if (mask != 0) {
    if (groundBlockAt(x + 2, ground_y, z) == WATER ||
        groundBlockAt(x - 2, ground_y, z) == WATER ||
        groundBlockAt(x, ground_y, z + 2) == WATER ||
        groundBlockAt(x, ground_y, z - 2) == WATER) {
      mask |= MON_HAB_SHORE;
    }
  }
  mask |= dayCycleSunAltitude() < 0 ? MON_HAB_NIGHT : MON_HAB_DAY;
  return mask;
}

/*
 * Is this species something a pad at this level may produce?
 *
 * The habitat half is rarity: a beach and a cave floor draw from genuinely
 * different rosters without a biome map existing anywhere.  The level half is
 * the evolution table told back to the world -- a creature the player could
 * only reach by raising one to fourteen has no business standing in a meadow
 * at level four, whatever its weight says.  Both loops below ask this one
 * question so the weighted total can never disagree with the draw.
 */
static u8 speciesFitsPad(const MonSpecies *s, u8 habitat, u8 night, u8 level) {
  if (s->spawn_weight == 0) {
    return FALSE;
  }
  if ((s->habitat & habitat & 0x0F) == 0) {
    return FALSE;
  }
  if ((s->habitat & MON_HAB_DAY) && night) {
    return FALSE;
  }
  if ((s->habitat & MON_HAB_NIGHT) && !night) {
    return FALSE;
  }
  return level >= monMinLevel((u8) (s - mon_species));
}

/* Pick a species for a pad that has already rolled its level. */
static u8 pickSpecies(u8 habitat, u8 level) {
  u32 total = 0;
  u32 roll;
  u8 index;
  u8 night = (habitat & MON_HAB_NIGHT) != 0;

  for (index = 0; index < MON_SPECIES_COUNT; index++) {
    if (speciesFitsPad(&mon_species[index], habitat, night, level)) {
      total += mon_species[index].spawn_weight;
    }
  }
  if (total == 0) {
    return MON_NONE;
  }
  roll = random((u32) total);
  for (index = 0; index < MON_SPECIES_COUNT; index++) {
    const MonSpecies *s = &mon_species[index];
    if (!speciesFitsPad(s, habitat, night, level)) {
      continue;
    }
    if (roll < s->spawn_weight) {
      return index;
    }
    roll -= s->spawn_weight;
  }
  return MON_NONE;
}

/*
 * How strong wild creatures are here.
 *
 * Distance from the world's origin, not a stored progression flag: the world
 * has no edges and no chapters, so the only honest difficulty curve is "the
 * further you walk, the older the things you meet".  It costs nothing to
 * store and it makes a direction of travel a decision.  The player's own
 * lead then bounds it from both sides, so a fresh party near spawn is never
 * mauled and a veteran is never farming level threes.
 */
static u8 encounterLevel(u8 player_num, int block_x, int block_z) {
  int distance = block_x < 0 ? -block_x : block_x;
  int depth = block_z < 0 ? -block_z : block_z;
  int level;
  u8 lead = monPartyLead(player_num);
  int lead_level = lead == MON_NONE ? 5 :
    mon_party[player_num][lead].level;

  if (depth > distance) {
    distance = depth;
  }
  level = 3 + distance / 45;
  if (level > lead_level + 3) {
    level = lead_level + 3;
  }
  if (level < lead_level - 4) {
    level = lead_level - 4;
  }
  level += (int) random(3) - 1;
  if (level < 2) {
    level = 2;
  }
  if (level > MON_MAX_LEVEL) {
    level = MON_MAX_LEVEL;
  }
  return (u8) level;
}

static Player *spawnAnchorPlayer(u8 *player_num) {
  u8 offset;
  u8 start;

  if (active_player_count == 0) {
    return NULL;
  }
  start = (u8) random(active_player_count);
  for (offset = 0; offset < active_player_count; offset++) {
    u8 index = (u8) ((start + offset) % active_player_count);
    if (players[index].active) {
      *player_num = index;
      return &players[index];
    }
  }
  return NULL;
}

static u8 roamerTooClose(float x, float z) {
  u8 index;

  for (index = 0; index < MON_MAX_ROAMERS; index++) {
    MonRoamer *other = &mon_roamers[index];
    float dx;
    float dz;
    if (!other->active) {
      continue;
    }
    dx = other->position.x - x;
    dz = other->position.z - z;
    if (dx * dx + dz * dz < (BLOCK_SIZE * 6.f) * (BLOCK_SIZE * 6.f)) {
      return TRUE;
    }
  }
  return FALSE;
}

static u8 trySpawnRoamer(void) {
  u8 anchor_num = 0;
  Player *anchor = spawnAnchorPlayer(&anchor_num);
  int block_x;
  int block_z;
  int ground_y;
  u8 habitat;
  u8 species;
  u8 level;
  u8 index;
  float x;
  float z;

  if (anchor == NULL) {
    return FALSE;
  }
  for (index = 0; index < MON_MAX_ROAMERS; index++) {
    if (!mon_roamers[index].active) {
      break;
    }
  }
  if (index >= MON_MAX_ROAMERS) {
    return FALSE;
  }

  /* Ten to twenty-five blocks out: far enough not to appear in front of the
     player, near enough to be inside the meshed ring so it is not spawned
     into terrain that has not streamed in. */
  block_x = floor(anchor->position.x / BLOCK_SIZE) +
    (int) random(31) - 15;
  block_z = floor(anchor->position.z / BLOCK_SIZE) +
    (int) random(31) - 15;
  x = (block_x + .5f) * BLOCK_SIZE;
  z = (block_z + .5f) * BLOCK_SIZE;
  {
    float dx = x - anchor->position.x;
    float dz = z - anchor->position.z;
    float distance = dx * dx + dz * dz;
    if (distance < (BLOCK_SIZE * 10.f) * (BLOCK_SIZE * 10.f) ||
        distance > (BLOCK_SIZE * 25.f) * (BLOCK_SIZE * 25.f)) {
      return FALSE;
    }
  }
  if (roamerTooClose(x, z)) {
    return FALSE;
  }
  if (!roamerSpawnGroundAt(block_x, block_z, &ground_y)) {
    return FALSE;
  }
  habitat = habitatAt(block_x, ground_y, block_z);
  /* Level first: it is an input to which species may stand here, not a
     decoration applied to whichever one was drawn.  Nothing about the roll
     depends on the species, so the order costs nothing to swap. */
  level = encounterLevel(anchor_num, block_x, block_z);
  species = pickSpecies(habitat, level);
  if (species == MON_NONE) {
    return FALSE;
  }

  {
    MonRoamer *roamer = &mon_roamers[index];

    roamer->position.x = x;
    roamer->position.y = (float) (ground_y + 1) * BLOCK_SIZE;
    roamer->position.z = z;
    roamer->yaw = (float) random(360);
    roamer->walk_time = 0;
    roamer->gait = 0;
    roamer->decision_time = ROAM_DECISION_MIN;
    roamer->notice_time = 0;
    roamer->ground_y = (s8) ground_y;
    roamer->species = species;
    roamer->level = level;
    roamer->state = ROAM_WANDER;
    roamer->seed = (u32) block_x * 0x9E3779B9u ^
      ((u32) block_z * 0x85EBCA6Bu) ^ world_seed;
    /*
     * One spawn in seven is a wandering trainer rather than a wild creature.
     * They are the only source of a real team fight outside co-op, and
     * because their party is a pure function of the seed above, meeting the
     * same trainer twice means meeting the same team twice without a byte
     * being written down.
     */
    roamer->kind = random(7) == 0 ? MON_ROAMER_TRAINER :
      MON_ROAMER_WILD;
    roamer->active = TRUE;
  }
  return TRUE;
}

static void updateRoamer(MonRoamer *roamer, float delta) {
  const MonSpecies *species = &mon_species[roamer->species];
  Player *nearest = NULL;
  float nearest_distance = 1.0e18f;
  u8 player_num;
  float speed = 0;
  u8 night = dayCycleSunAltitude() < 0;
  u8 moved;

  for (player_num = 0; player_num < active_player_count; player_num++) {
    float dx;
    float dz;
    float distance;
    if (!players[player_num].active) {
      continue;
    }
    dx = players[player_num].position.x - roamer->position.x;
    dz = players[player_num].position.z - roamer->position.z;
    distance = dx * dx + dz * dz;
    if (distance < nearest_distance) {
      nearest_distance = distance;
      nearest = &players[player_num];
    }
  }
  if (nearest == NULL) {
    return;
  }
  if (nearest_distance > ROAM_DESPAWN_DISTANCE * ROAM_DESPAWN_DISTANCE) {
    roamer->active = FALSE;
    return;
  }
  /* A body on its death screen keeps the creature loaded but is not someone
     to notice, turn toward or walk up to.  Same trade as the mobs make. */
  if (nearest->dead) {
    nearest_distance = 1.0e18f;
  }

  roamer->decision_time -= delta;
  if (roamer->decision_time <= 0) {
    roamer->decision_time = ROAM_DECISION_MIN + (float) random(
      (u32) ROAM_DECISION_VARIATION);
    roamer->state = random(3) == 0 ? ROAM_IDLE : ROAM_WANDER;
    roamer->yaw = (float) random(360);
  }

  /*
   * Creatures look at whoever is near them.  That is the entire "encounter
   * telegraph": the game never grabs the player out of what they were doing,
   * it turns a creature toward them and puts one line on the HUD, and the
   * player decides.  Forced encounters are the wrong trade on a console this
   * slow to return to what you were building.
   */
  if (nearest_distance < ROAM_NOTICE_RANGE * ROAM_NOTICE_RANGE) {
    float dx = nearest->position.x - roamer->position.x;
    float dz = nearest->position.z - roamer->position.z;

    if (dx != 0 || dz != 0) {
      roamer->yaw = facingYaw(dx, dz);
    }
    if (roamer->state != ROAM_FLEE) {
      roamer->state = ROAM_NOTICE;
      roamer->notice_time += delta;
    }
    /* Aggressive middle stages take the initiative after dark, and only in a
       world that is currently willing to hold monsters -- the same rule the
       night's own budget reads, so peace means peace from both ecologies. */
    if (species->aggressive && night &&
        world_rules.monsters != RULE_MONSTERS_NONE &&
        !mon_battle.active &&
        nearest_distance > ROAM_AGGRO_RANGE * ROAM_AGGRO_RANGE) {
      roamer->state = ROAM_APPROACH;
    }
  } else {
    roamer->notice_time = 0;
  }

  switch (roamer->state) {
    case ROAM_FLEE:
      speed = ROAM_FLEE_SPEED;
      break;
    case ROAM_APPROACH:
      speed = ROAM_APPROACH_SPEED;
      break;
    case ROAM_WANDER:
      speed = ROAM_WALK_SPEED;
      break;
    default:
      speed = 0;
      break;
  }

  moved = FALSE;
  if (speed > 0) {
    float radians = roamer->yaw * M_DTOR;
    float step_x = -sinf(radians) * speed * delta;
    float step_z = -cosf(radians) * speed * delta;
    float next_x = roamer->position.x + step_x;
    float next_z = roamer->position.z + step_z;
    int block_x = floor(next_x / BLOCK_SIZE);
    int block_z = floor(next_z / BLOCK_SIZE);
    int ground_y;

    if (roamerGroundNear(block_x, block_z, roamer->ground_y, &ground_y)) {
      roamer->position.x = next_x;
      roamer->position.z = next_z;
      roamer->ground_y = (s8) ground_y;
      roamer->position.y = (float) (ground_y + 1) * BLOCK_SIZE;
      roamer->walk_time += delta * ROAM_STRIDE_RATE;
      moved = TRUE;
    } else {
      /* Blocked.  Turn rather than stall against the wall. */
      roamer->yaw = (float) random(360);
    }
  }
  if (!moved) {
    /* Standing still is not being switched off -- the breathe and tail cycles
       read from walk_time too.  It is gait, not this clock, that keeps the
       legs from walking on the spot. */
    roamer->walk_time += delta * ROAM_IDLE_RATE;
  }
  roamer->gait += ((moved ? 1.f : 0.f) - roamer->gait) *
    min(1.f, delta * ROAM_GAIT_EASE);
}

/* ------------------------------------------------------------------ */
/* Battle setup.                                                            */
/* ------------------------------------------------------------------ */

/*
 * Where the two creatures stand, fixed once when the battle opens.
 *
 * The scene is laid out along the direction the player is already looking,
 * not along the line to whatever started the fight.  There is no battle
 * camera and no cut -- the player keeps their own view -- so the only
 * placement that reliably puts both creatures on screen is one measured from
 * where that view already points.  It also means an ambush from behind
 * becomes a fight in front of you rather than a fight you cannot see.
 */
static void placeBattleScene(u8 player_num) {
  Player *player = &players[player_num];
  float radians = player->yaw * M_DTOR;
  float forward_x = -sinf(radians);
  float forward_z = -cosf(radians);
  /* The player's right, for the stagger that keeps the near creature from
     standing exactly in front of the far one. */
  float right_x = -forward_z;
  float right_z = forward_x;

  /*
   * The distances are set by where the pair lands on screen, not by what
   * would feel like a duelling range.  With the view tipped down by
   * BATTLE_VIEW_PITCH, three and a half blocks puts the near creature in the
   * clear band between the status panels and the message box, and six and a
   * half puts the far one just above it.  Standing them closer -- which
   * looks better written down -- puts both under the interface.
   */
  mon_battle.stand[1].x = player->position.x +
    forward_x * (BLOCK_SIZE * 6.6f) + right_x * (BLOCK_SIZE * .9f);
  mon_battle.stand[1].z = player->position.z +
    forward_z * (BLOCK_SIZE * 6.6f) + right_z * (BLOCK_SIZE * .9f);
  mon_battle.stand[0].x = player->position.x +
    forward_x * (BLOCK_SIZE * 3.4f) - right_x * (BLOCK_SIZE * 1.2f);
  mon_battle.stand[0].z = player->position.z +
    forward_z * (BLOCK_SIZE * 3.4f) - right_z * (BLOCK_SIZE * 1.2f);
  mon_battle.stand[0].y = groundHeightNear(mon_battle.stand[0].x,
    mon_battle.stand[0].z, player->position.y);
  mon_battle.stand[1].y = groundHeightNear(mon_battle.stand[1].x,
    mon_battle.stand[1].z, player->position.y);
  /* The battle axis.  Side zero faces along it and side one faces back, so
     the pair always squares up whatever direction the fight started in. */
  mon_battle.facing = player->yaw;
}

/* Tip the battling players' views down onto the scene, remembering what they
   were looking at so the world is handed back exactly as it was found. */
static void takeBattleView(void) {
  u8 side;

  for (side = 0; side < 2; side++) {
    u8 player_num = mon_battle.side_player[side];

    mon_battle.saved_pitch[side] = 0;
    if (!mon_battle.side_is_player[side] || player_num >= MAX_PLAYERS) {
      continue;
    }
    mon_battle.saved_pitch[side] = players[player_num].pitch;
    players[player_num].pitch = BATTLE_VIEW_PITCH;
  }
}

static void releaseBattleView(void) {
  u8 side;

  for (side = 0; side < 2; side++) {
    u8 player_num = mon_battle.side_player[side];

    if (!mon_battle.side_is_player[side] || player_num >= MAX_PLAYERS) {
      continue;
    }
    players[player_num].pitch = mon_battle.saved_pitch[side];
  }
}

static void resetBattleCursors(void) {
  mon_battle.command_cursor = 0;
  mon_battle.move_cursor = 0;
  mon_battle.team_cursor = 0;
  mon_battle.bag_cursor = 0;
}

static void beginBattleCommon(u8 kind, u8 challenger) {
  u8 index;

  mon_battle.active = TRUE;
  mon_battle.kind = kind;
  mon_battle.finished = FALSE;
  mon_battle.message_head = 0;
  mon_battle.message_count = 0;
  mon_battle.message_reveal = 0;
  mon_battle.scene_time = 0;
  mon_battle.escape_attempts = 0;
  mon_battle.pending_xp = 0;
  mon_battle.catch_shakes = 0;
  mon_battle.acting_side = 0;
  mon_battle.resolve_index = 0;
  mon_battle.side_is_player[0] = TRUE;
  mon_battle.side_player[0] = challenger;
  mon_battle.side_is_player[1] = FALSE;
  mon_battle.side_player[1] = 0;
  mon_battle.npc_party_size = 0;
  mon_battle.npc_active = 0;
  for (index = 0; index < 2; index++) {
    mon_battle.action[index] = MON_ACTION_NONE;
    mon_battle.action_arg[index] = 0;
    mon_battle.lunge[index] = 0;
    mon_battle.hurt[index] = 0;
    mon_battle.faint[index] = 0;
  }
  resetBattleCursors();
  message_hold = 0;
}

static void sendOutPlayerLead(u8 side) {
  u8 player_num = mon_battle.side_player[side];
  u8 slot = monPartyLead(player_num);
  PartyMon *mon;

  if (slot == MON_NONE) {
    return;
  }
  mon = &mon_party[player_num][slot];
  loadFighter(side, mon->species, mon->level, mon->hp, slot);
  pushNamed(mon_species[mon->species].name, " GO!");
}

/*
 * A trainer's team, derived from the seed their roamer was spawned with.
 * Nothing about a trainer is stored: the same coordinates and world seed
 * always produce the same opponent, which is what lets them exist at all in
 * a world whose entities are never saved.
 */
static void buildTrainerTeam(u32 seed, u8 level) {
  u8 count = (u8) (2 + (seed >> 3) % 2u);
  u8 index;

  mon_battle.npc_party_size = count;
  for (index = 0; index < count; index++) {
    u32 draw = seed ^ (0x9E3779B9u * (index + 1));
    u8 species = (u8) (draw % MON_SPECIES_COUNT);
    u8 mon_level;

    mon_level = (u8) (level - 1 + (draw >> 8) % 3u);
    if (mon_level < 2) {
      mon_level = 2;
    }
    if (mon_level > MON_MAX_LEVEL) {
      mon_level = MON_MAX_LEVEL;
    }
    /*
     * Trainers field nothing the player could not be holding at the same
     * level, so a drawn species walks back down its family until its own
     * threshold is one this level has passed.
     *
     * A single step used to do this, gated on level 25.  One step off a final
     * stage lands on a middle stage, which is exactly the thing a low-level
     * trainer should not have either -- two thirds of an early team came out
     * a stage too old.  The loop runs at most twice: a family is three deep.
     */
    while (monMinLevel(species) > mon_level) {
      u8 parent = monParentSpecies(species);
      if (parent == MON_NONE) {
        break;
      }
      species = parent;
    }
    mon_battle.npc_party[index].species = species;
    mon_battle.npc_party[index].level = mon_level;
    mon_battle.npc_party[index].xp = 0;
    mon_battle.npc_party[index].hp = monMaxHealth(species, mon_level);
  }
}

static void sendOutNpc(void) {
  PartyMon *mon = &mon_battle.npc_party[mon_battle.npc_active];

  loadFighter(1, mon->species, mon->level, mon->hp, MON_NONE);
  pushNamed(mon_species[mon->species].name, " STEPS UP!");
}

/*
 * The first creature a player ever meets joins them instead of fighting.
 *
 * A collection game whose first ten minutes are "you cannot play yet" is a
 * bad trade on a console people put on for twenty minutes at a time, and
 * every alternative -- a starter menu, a gift chest, a tutorial NPC -- costs
 * a screen and takes the choice away.  Walking up to the one you like is
 * both the shortest path in and the most player-owned.
 */
static u8 grantStarter(u8 player_num, MonRoamer *roamer) {
  u8 level = roamer->level < 5 ? 5 : roamer->level;
  u8 slot;

  if (level > 8) {
    level = 8;
  }
  slot = addToParty(player_num, roamer->species, level);
  if (slot == MON_NONE) {
    return FALSE;
  }
  roamer->active = FALSE;
  playSound(SOUND_PICKUP);
  return TRUE;
}

static void beginWildBattle(u8 player_num, MonRoamer *roamer) {
  beginBattleCommon(MON_BATTLE_WILD, player_num);
  placeBattleScene(player_num);
  loadFighter(1, roamer->species, roamer->level,
    monMaxHealth(roamer->species, roamer->level), MON_NONE);
  setPhase(MON_PHASE_COMMAND);
  {
    char *out = message_scratch;
    out = appendText(out, "A WILD ");
    out = appendText(out, mon_species[roamer->species].name);
    out = appendText(out, " APPEARED!");
    *out = 0;
    pushMessage(message_scratch);
  }
  sendOutPlayerLead(0);
  takeBattleView();
  roamer->active = FALSE;
  playSound(SOUND_PUNCH);
}

static void beginTrainerBattle(u8 player_num, MonRoamer *roamer) {
  beginBattleCommon(MON_BATTLE_TRAINER, player_num);
  placeBattleScene(player_num);
  buildTrainerTeam(roamer->seed, roamer->level);
  mon_battle.npc_active = 0;
  setPhase(MON_PHASE_COMMAND);
  /* By name, because the badge the player just read was a name.  It comes
     from the same seed the team above does, so the person who challenges is
     the person who was standing there. */
  {
    char *out = message_scratch;
    out = appendText(out, humanoidPersonFromSeed(roamer->seed)->name);
    out = appendText(out, " WANTS TO BATTLE!");
    *out = 0;
    pushMessage(message_scratch);
  }
  sendOutNpc();
  sendOutPlayerLead(0);
  takeBattleView();
  roamer->active = FALSE;
  playSound(SOUND_PUNCH);
}

static void beginPvpBattle(u8 challenger, u8 opponent) {
  beginBattleCommon(MON_BATTLE_PVP, challenger);
  placeBattleScene(challenger);
  mon_battle.side_is_player[1] = TRUE;
  mon_battle.side_player[1] = opponent;
  setPhase(MON_PHASE_COMMAND);
  {
    char *out = message_scratch;
    out = appendText(out, "PLAYER ");
    out = appendNumber(out, challenger + 1u);
    out = appendText(out, " VS PLAYER ");
    out = appendNumber(out, opponent + 1u);
    out = appendText(out, "!");
    *out = 0;
    pushMessage(message_scratch);
  }
  sendOutPlayerLead(0);
  sendOutPlayerLead(1);
  takeBattleView();
  playSound(SOUND_PUNCH);
}

/* ------------------------------------------------------------------ */
/* Interaction.                                                             */
/* ------------------------------------------------------------------ */

/* The creature in front of a player, or MON_NONE.  Distance plus a
   generous facing cone: on one stick, demanding precise aim at a wandering
   target is the difference between charming and annoying. */
static u8 findRoamerTarget(u8 player_num) {
  Player *player = &players[player_num];
  float best = ROAM_INTERACT_RANGE * ROAM_INTERACT_RANGE;
  u8 best_index = MON_NONE;
  u8 index;
  float radians = player->yaw * M_DTOR;
  float forward_x = -sinf(radians);
  float forward_z = -cosf(radians);

  for (index = 0; index < MON_MAX_ROAMERS; index++) {
    MonRoamer *roamer = &mon_roamers[index];
    float dx;
    float dz;
    float distance;

    if (!roamer->active) {
      continue;
    }
    dx = roamer->position.x - player->position.x;
    dz = roamer->position.z - player->position.z;
    distance = dx * dx + dz * dz;
    if (distance > best) {
      continue;
    }
    /* Anything genuinely underfoot counts however the player is facing. */
    if (distance > (BLOCK_SIZE * 1.4f) * (BLOCK_SIZE * 1.4f) &&
        dx * forward_x + dz * forward_z <= 0) {
      continue;
    }
    best = distance;
    best_index = index;
  }
  return best_index;
}

u8 mon64TargetRoamer(u8 player_num) {
  return player_num < MAX_PLAYERS ? roamer_target[player_num] : MON_NONE;
}

/* A player standing in front of another, for the challenge. */
static u8 findPlayerTarget(u8 player_num) {
  Player *player = &players[player_num];
  float radians = player->yaw * M_DTOR;
  float forward_x = -sinf(radians);
  float forward_z = -cosf(radians);
  u8 index;

  for (index = 0; index < active_player_count; index++) {
    float dx;
    float dz;

    if (index == player_num || !playerAlive(&players[index])) {
      continue;
    }
    dx = players[index].position.x - player->position.x;
    dz = players[index].position.z - player->position.z;
    if (dx * dx + dz * dz < PVP_CHALLENGE_RANGE * PVP_CHALLENGE_RANGE &&
        dx * forward_x + dz * forward_z > 0) {
      return index;
    }
  }
  return MON_NONE;
}

u8 mon64TryInteract(u8 player_num, u8 challenge_held) {
  u8 target;

  if (!mon64Enabled() || mon_battle.active) {
    return FALSE;
  }

  /* Accepting a challenge comes first: the prompt is on screen and A is what
     it says to press. */
  if (pvp_target == player_num && pvp_challenger != MON_NONE) {
    u8 challenger = pvp_challenger;
    pvp_challenger = MON_NONE;
    pvp_target = MON_NONE;
    if (monPartyLead(player_num) == MON_NONE ||
        monPartyLead(challenger) == MON_NONE) {
      return TRUE;
    }
    beginPvpBattle(challenger, player_num);
    return TRUE;
  }

  target = roamer_target[player_num];
  if (target != MON_NONE) {
    MonRoamer *roamer = &mon_roamers[target];

    if (roamer->kind == MON_ROAMER_TRAINER) {
      if (monPartyLead(player_num) == MON_NONE) {
        return TRUE;
      }
      beginTrainerBattle(player_num, roamer);
      return TRUE;
    }
    if (monPartyCount(player_num) == 0) {
      return grantStarter(player_num, roamer);
    }
    if (monPartyLead(player_num) == MON_NONE) {
      return TRUE;
    }
    beginWildBattle(player_num, roamer);
    return TRUE;
  }

  /* Nothing to battle in front of them.  Z + A challenges the player they
     are looking at instead: the chord keeps a plain A free for the block a
     co-op partner is almost certainly standing next to. */
  if (challenge_held) {
    u8 other = findPlayerTarget(player_num);
    if (other != MON_NONE && monPartyLead(player_num) != MON_NONE) {
      pvp_challenger = player_num;
      pvp_target = other;
      pvp_challenge_time = 300.f;
      return TRUE;
    }
  }
  return FALSE;
}

/* ------------------------------------------------------------------ */
/* Turn resolution.                                                         */
/* ------------------------------------------------------------------ */

static void applyDamage(u8 side, u16 amount) {
  MonFighter *f = &mon_battle.fighter[side];

  if (amount >= f->hp) {
    f->hp = 0;
  } else {
    f->hp = (u16) (f->hp - amount);
  }
  mon_battle.hurt[side] = 14.f;
  storeFighter(side);
}

static void healFighter(u8 side, u16 amount) {
  MonFighter *f = &mon_battle.fighter[side];

  if (f->hp + amount > f->max_hp) {
    f->hp = f->max_hp;
  } else {
    f->hp = (u16) (f->hp + amount);
  }
  storeFighter(side);
}

/*
 * One move, start to finish: accuracy, damage, type message, side effect.
 *
 * Written as a single step because that is exactly what one line of the log
 * describes.  Everything is integer; the only division that could ever see a
 * zero is by defence, which statValue floors at four.
 */
static void performMove(u8 attacker, u8 defender) {
  MonFighter *att = &mon_battle.fighter[attacker];
  MonFighter *def = &mon_battle.fighter[defender];
  u8 slot = mon_battle.action_arg[attacker];
  const MonMove *move;
  u32 damage;
  u8 multiplier;
  u8 critical = FALSE;
  u8 hits = 1;
  u8 hit;
  /* Swings that actually landed, which is not `hits`: the loop stops early on
     a knockout, and announcing a second blow that never fell is the log
     describing a fight the player did not watch. */
  u8 landed = 0;
  u32 total = 0;

  if (slot >= att->move_count) {
    slot = 0;
  }
  move = &mon_moves[att->move[slot]];
  if (att->pp[slot] > 0) {
    att->pp[slot]--;
  }
  mon_battle.lunge[attacker] = 16.f;

  {
    char *out = message_scratch;
    out = appendText(out, fighterName(attacker));
    out = appendText(out, " USED ");
    out = appendText(out, move->name);
    out = appendText(out, "!");
    *out = 0;
    pushMessage(message_scratch);
  }

  if (random(100) >= move->accuracy) {
    pushMessage("IT MISSED!");
    return;
  }

  if (move->power == 0) {
    switch (move->effect) {
      case MON_EFFECT_HEAL:
        healFighter(attacker, (u16) ((att->max_hp - att->hp + 1) / 2));
        pushNamed(fighterName(attacker), " RECOVERED.");
        break;
      case MON_EFFECT_BUFF_ATK:
        if (att->attack_stage < 4) {
          att->attack_stage++;
        }
        pushNamed(fighterName(attacker), "'S ATTACK ROSE!");
        break;
      case MON_EFFECT_BUFF_DEF:
        if (att->defense_stage < 4) {
          att->defense_stage++;
        }
        pushNamed(fighterName(attacker), "'S DEFENCE ROSE!");
        break;
      default:
        break;
    }
    return;
  }

  multiplier = typeMultiplier(move->type, def->species);
  critical = random(move->effect == MON_EFFECT_HIGH_CRIT ? 4u : 16u) == 0;
  if (move->effect == MON_EFFECT_MULTI) {
    hits = 2;
  }

  for (hit = 0; hit < hits; hit++) {
    u32 attack_stat = stagedStat(att->attack, att->attack_stage);
    u32 defense_stat = critical ? def->defense :
      stagedStat(def->defense, def->defense_stage);

    if (defense_stat == 0) {
      defense_stat = 1;
    }
    damage = ((2u * att->level) / 5u + 2u) * move->power;
    damage = (damage * attack_stat) / defense_stat;
    damage = damage / 50u + 2u;
    /* Same-type bonus: three halves, the traditional and legible amount. */
    if (move->type < MON_TYPE_COUNT &&
        att->species < MON_SPECIES_COUNT &&
        mon_species[att->species].type == move->type) {
      damage = (damage * 3u) / 2u;
    }
    damage = (damage * multiplier) / 4u;
    if (critical) {
      damage *= 2u;
    }
    /* 85..100 percent, so identical turns are not identical. */
    damage = (damage * (85u + random(16))) / 100u;
    if (damage == 0) {
      damage = 1;
    }
    total += damage;
    landed++;
    applyDamage(defender, (u16) damage);
    if (def->hp == 0) {
      break;
    }
  }

  if (landed > 1) {
    pushMessage("IT HIT TWICE!");
  }
  if (critical) {
    pushMessage("A CRITICAL HIT!");
  }
  if (multiplier > 4) {
    pushMessage("IT IS SUPER EFFECTIVE!");
  } else if (multiplier < 4) {
    pushMessage("IT IS NOT VERY EFFECTIVE.");
  }

  if (move->effect == MON_EFFECT_DRAIN) {
    healFighter(attacker, (u16) ((total + 1) / 2));
    pushNamed(fighterName(attacker), " DRAINED HEALTH.");
  } else if (move->effect == MON_EFFECT_RECOIL) {
    applyDamage(attacker, (u16) ((total + 3) / 4));
    pushNamed(fighterName(attacker), " TOOK RECOIL.");
  }
}

/*
 * A random move among the ones that still have power behind them.
 *
 * The player is stopped at the move menu from picking an empty one, so an
 * opponent that could pick it anyway is playing a different game.  Scanning
 * from a random start keeps the choice uniform over what is left without
 * building a candidate list.  If nothing has power the first slot comes back
 * regardless, on the same grounds monKnownMoves guarantees a first move:
 * a turn a creature cannot take is worse than a rule bent once.
 */
static u8 randomAbleMove(const MonFighter *f) {
  u8 start = (u8) random(f->move_count);
  u8 offset;

  for (offset = 0; offset < f->move_count; offset++) {
    u8 index = (u8) ((start + offset) % f->move_count);
    if (f->pp[index] > 0) {
      return index;
    }
  }
  return start;
}

/*
 * The opponent's choice.
 *
 * Trainers score every move they know by what it would actually do -- power
 * times effectiveness, with a bonus for the one that would finish the fight
 * -- and take the best.  Wild creatures roll instead.  That difference is
 * the entire "trainers are harder" design, and it costs four multiplies.
 */
static u8 chooseNpcMove(void) {
  MonFighter *npc = &mon_battle.fighter[1];
  MonFighter *foe = &mon_battle.fighter[0];
  u8 best = 0;
  u32 best_score = 0;
  u8 index;

  if (mon_battle.kind == MON_BATTLE_WILD || random(5) == 0) {
    return randomAbleMove(npc);
  }
  for (index = 0; index < npc->move_count; index++) {
    const MonMove *move = &mon_moves[npc->move[index]];
    u32 score;

    if (npc->pp[index] == 0) {
      continue;
    }
    if (move->power == 0) {
      /* Support only when it would plainly help: healing while hurt, or
         buffing early while there is time to use it. */
      score = (move->effect == MON_EFFECT_HEAL &&
        npc->hp * 3u < npc->max_hp) ? 90u : 25u;
    } else {
      score = (u32) move->power * typeMultiplier(move->type, foe->species);
      score = (score * move->accuracy) / 100u;
      if (move->type < MON_TYPE_COUNT &&
          mon_species[npc->species].type == move->type) {
        score = (score * 3u) / 2u;
      }
    }
    if (score > best_score) {
      best_score = score;
      best = index;
    }
  }
  /* Every move empty: nothing was scored, so fall through to the same last
     resort the random branch uses rather than defaulting to slot zero. */
  return best_score == 0 ? randomAbleMove(npc) : best;
}

static u8 firstAbleSlot(u8 player_num, u8 except) {
  u8 index;

  for (index = 0; index < MON_PARTY_SIZE; index++) {
    PartyMon *mon = &mon_party[player_num][index];
    if (index != except && mon->species != MON_NONE && mon->hp > 0) {
      return index;
    }
  }
  return MON_NONE;
}

static void awardExperience(u8 side) {
  u8 player_num = mon_battle.side_player[side];
  MonFighter *winner = &mon_battle.fighter[side];
  MonFighter *loser = &mon_battle.fighter[side ^ 1];
  PartyMon *mon;
  u32 gain;

  if (!mon_battle.side_is_player[side] ||
      winner->party_slot >= MON_PARTY_SIZE) {
    return;
  }
  mon = &mon_party[player_num][winner->party_slot];
  if (mon->level >= MON_MAX_LEVEL) {
    /* Nothing left to earn, but a knockout that says nothing at all reads as
       a dropped line rather than as a creature that is already finished. */
    pushNamed(fighterName(side), " HAS NOTHING LEFT TO LEARN.");
    return;
  }
  gain = ((u32) mon_species[loser->species].xp_yield * loser->level) / 4u;
  if (mon_battle.kind != MON_BATTLE_WILD) {
    /* A team that fights back is worth more than one that wandered into you. */
    gain = (gain * 3u) / 2u;
  }
  if (gain == 0) {
    gain = 1;
  }
  {
    char *out = message_scratch;
    out = appendText(out, mon_species[mon->species].name);
    out = appendText(out, " GAINED ");
    out = appendNumber(out, gain);
    out = appendText(out, " XP.");
    *out = 0;
    pushMessage(message_scratch);
  }
  mon->xp = (u16) (mon->xp + gain);

  /* Level ups, then evolution.  Both are announced; a change the player did
     not see happen is a change they will assume is a bug. */
  while (mon->level < MON_MAX_LEVEL &&
      mon->xp >= monXpForLevel(mon->level)) {
    u16 old_max = monMaxHealth(mon->species, mon->level);
    mon->xp = (u16) (mon->xp - monXpForLevel(mon->level));
    mon->level++;
    /* A level up heals by the health it added, so growing stronger is never
       also a reason to have to stop and wait. */
    mon->hp = (u16) (mon->hp + monMaxHealth(mon->species, mon->level) -
      old_max);
    {
      char *out = message_scratch;
      out = appendText(out, mon_species[mon->species].name);
      out = appendText(out, " REACHED LEVEL ");
      out = appendNumber(out, mon->level);
      out = appendText(out, "!");
      *out = 0;
      pushMessage(message_scratch);
    }
    if (mon_species[mon->species].evolves_to != MON_NONE &&
        mon->level >= mon_species[mon->species].evolve_level) {
      u8 from = mon->species;
      u8 to = mon_species[from].evolves_to;
      u16 missing = (u16) (monMaxHealth(from, mon->level) - mon->hp);

      mon->species = to;
      mon->hp = (u16) (monMaxHealth(to, mon->level) - missing);
      {
        char *out = message_scratch;
        out = appendText(out, mon_species[from].name);
        out = appendText(out, " BECAME ");
        out = appendText(out, mon_species[to].name);
        out = appendText(out, "!");
        *out = 0;
        pushMessage(message_scratch);
      }
      playSound(SOUND_PICKUP);
    }
  }
  /* Re-derive the fighter so the panel and the next turn use the new numbers,
     without giving back the power and the stat changes the fight has spent. */
  refreshFighter(side, mon->species, mon->level, mon->hp);
}

static void endBattle(const char *line) {
  if (line != NULL) {
    pushMessage(line);
  }
  mon_battle.finished = TRUE;
  setPhase(MON_PHASE_END);
}

/* Every party member back to one health, so a loss costs time rather than
   the save.  Regeneration does the rest while the player walks home. */
static void reviveParty(u8 player_num) {
  u8 index;

  for (index = 0; index < MON_PARTY_SIZE; index++) {
    PartyMon *mon = &mon_party[player_num][index];
    if (mon->species != MON_NONE && mon->hp == 0) {
      mon->hp = 1;
    }
  }
}

static void attemptCatch(void) {
  MonFighter *target = &mon_battle.fighter[1];
  u32 odds;
  u8 shakes;

  /*
   * Weakening matters and level matters, in that order.  At full health the
   * gel is worth about a third of the species' rate; at a sliver it is worth
   * all of it, less a penalty for a creature far above the thrower's own.
   */
  odds = ((u32) mon_species[target->species].catch_rate *
    (3u * target->max_hp - 2u * target->hp)) / (3u * target->max_hp);
  if (target->level > 20) {
    odds = (odds * 20u) / target->level;
  }
  if (odds > 250u) {
    odds = 250u;
  }

  for (shakes = 0; shakes < 3; shakes++) {
    if (random(256) >= odds) {
      break;
    }
  }
  mon_battle.catch_shakes = shakes;
  mon_battle.catch_result = shakes >= 3;
  mon_battle.catch_time = 0;
  setPhase(MON_PHASE_CATCH);
}

/*
 * One atomic step of the turn being resolved, run each time the log drains.
 *
 * The indices are a script, not a state machine with transitions: 0 and 2 are
 * the two sides acting in speed order, 1 and 3 check whether that killed
 * anybody, and 4 closes the turn.  Keeping it linear is what makes "who acts
 * next" answerable by reading four lines.
 */
static void resolveStep(void) {
  u8 first = mon_battle.first_side;
  u8 second = (u8) (first ^ 1);
  u8 acting;
  u8 index = mon_battle.resolve_index;

  mon_battle.resolve_index++;
  switch (index) {
    case 0:
    case 2:
      acting = index == 0 ? first : second;
      if (mon_battle.fighter[acting].hp == 0 ||
          mon_battle.fighter[acting ^ 1].hp == 0) {
        return;
      }
      if (mon_battle.action[acting] == MON_ACTION_MOVE) {
        performMove(acting, (u8) (acting ^ 1));
      }
      return;

    case 1:
    case 3: {
      u8 fainted = mon_battle.fighter[0].hp == 0 ? 0 :
        (mon_battle.fighter[1].hp == 0 ? 1 : MON_NONE);

      if (fainted == MON_NONE) {
        return;
      }
      mon_battle.faint[fainted] = 1.f;
      pushNamed(fighterName(fainted), " FAINTED!");
      /* Skip straight to the end of the turn; nothing else happens after a
         knockout. */
      mon_battle.resolve_index = 4;
      return;
    }

    default:
      break;
  }

  /* End of turn.  Deal with a knockout, or hand control back. */
  if (mon_battle.fighter[1].hp == 0) {
    awardExperience(0);
    if (mon_battle.kind == MON_BATTLE_TRAINER) {
      mon_battle.npc_party[mon_battle.npc_active].hp = 0;
      mon_battle.npc_active++;
      if (mon_battle.npc_active < mon_battle.npc_party_size) {
        sendOutNpc();
        mon_battle.resolve_index = 0;
        mon_battle.acting_side = 0;
        setPhase(MON_PHASE_COMMAND);
        resetBattleCursors();
        return;
      }
      endBattle("THE TRAINER IS OUT OF CREATURES!");
      return;
    }
    if (mon_battle.kind == MON_BATTLE_PVP) {
      u8 replacement = firstAbleSlot(mon_battle.side_player[1],
        MON_PARTY_SIZE);
      if (replacement != MON_NONE) {
        PartyMon *mon = &mon_party[mon_battle.side_player[1]][replacement];
        loadFighter(1, mon->species, mon->level, mon->hp, replacement);
        pushNamed(mon_species[mon->species].name, " GO!");
        mon_battle.resolve_index = 0;
        mon_battle.acting_side = 0;
        setPhase(MON_PHASE_COMMAND);
        resetBattleCursors();
        return;
      }
      reviveParty(mon_battle.side_player[1]);
      endBattle("PLAYER ONE WINS!");
      return;
    }
    endBattle(NULL);
    return;
  }

  if (mon_battle.fighter[0].hp == 0) {
    u8 player_num = mon_battle.side_player[0];
    u8 replacement = firstAbleSlot(player_num, MON_PARTY_SIZE);

    /* The mirror of the branch above.  Only a player side ever collects, so
       this is a no-op in a wild or trainer fight and the whole of player
       two's experience in a PVP one -- which used to be nothing at all. */
    awardExperience(1);
    if (replacement != MON_NONE) {
      PartyMon *mon = &mon_party[player_num][replacement];
      loadFighter(0, mon->species, mon->level, mon->hp, replacement);
      pushNamed(mon_species[mon->species].name, " GO!");
      mon_battle.resolve_index = 0;
      mon_battle.acting_side = 0;
      setPhase(MON_PHASE_COMMAND);
      resetBattleCursors();
      return;
    }
    reviveParty(player_num);
    endBattle("YOUR TEAM CANNOT FIGHT ON.");
    return;
  }

  mon_battle.resolve_index = 0;
  mon_battle.acting_side = 0;
  setPhase(MON_PHASE_COMMAND);
  resetBattleCursors();
}

/* Both sides have committed.  Decide the order and start the script. */
static void beginResolve(void) {
  u16 speed_a = mon_battle.fighter[0].speed;
  u16 speed_b = mon_battle.fighter[1].speed;

  /* Running and items resolve before anything else, exactly as a player
     expects: fleeing after being hit is not fleeing. */
  if (mon_battle.action[0] == MON_ACTION_RUN) {
    u32 chance = 40u + (speed_a > speed_b ? 40u : 10u) +
      mon_battle.escape_attempts * 15u;
    mon_battle.escape_attempts++;
    if (mon_battle.kind != MON_BATTLE_WILD) {
      pushMessage("NO RUNNING FROM A TRAINER!");
    } else if (random(100) < chance) {
      endBattle("GOT AWAY SAFELY!");
      return;
    } else {
      pushMessage("COULD NOT GET AWAY!");
    }
    mon_battle.action[0] = MON_ACTION_NONE;
  }

  mon_battle.first_side = speed_a > speed_b ? 0 :
    (speed_b > speed_a ? 1 : (u8) random(2));
  mon_battle.resolve_index = 0;
  /* The panels go back to the challenger's point of view for the turn
     itself.  In a two-player battle they would otherwise stay flipped to
     whoever committed last, so the same health bar would change sides
     halfway through reading about the hit that emptied it. */
  mon_battle.acting_side = 0;
  setPhase(MON_PHASE_RESOLVE);
}

/* The opponent commits, then the turn starts.  A PvP battle waits for the
   second player instead. */
static void commitSide(u8 side) {
  if (side == 0 && mon_battle.kind == MON_BATTLE_PVP) {
    mon_battle.acting_side = 1;
    resetBattleCursors();
    setPhase(MON_PHASE_COMMAND);
    return;
  }
  if (!mon_battle.side_is_player[1]) {
    mon_battle.action[1] = MON_ACTION_MOVE;
    mon_battle.action_arg[1] = chooseNpcMove();
  }
  beginResolve();
}

/* ------------------------------------------------------------------ */
/* Battle input.                                                            */
/* ------------------------------------------------------------------ */

/* Stick and D-pad, with the same edge-triggered feel the rest of the game's
   menus have. */
#define NAV_REPEAT_DELAY 14

static u8 navigationEdge(NUContData *cont, u8 *held, s8 *dx, s8 *dy) {
  static u8 repeat;
  u8 direction = 0;

  *dx = 0;
  *dy = 0;
  if (cont->stick_x > 45 || (cont->trigger & R_JPAD)) {
    direction = 1;
    *dx = 1;
  } else if (cont->stick_x < -45 || (cont->trigger & L_JPAD)) {
    direction = 2;
    *dx = -1;
  } else if (cont->stick_y > 45 || (cont->trigger & U_JPAD)) {
    direction = 3;
    *dy = -1;
  } else if (cont->stick_y < -45 || (cont->trigger & D_JPAD)) {
    direction = 4;
    *dy = 1;
  }
  if (direction == 0) {
    *held = 0;
    repeat = 0;
    return FALSE;
  }
  if (*held == direction) {
    if (++repeat < NAV_REPEAT_DELAY) {
      return FALSE;
    }
    repeat = 0;
    return TRUE;
  }
  *held = direction;
  repeat = 0;
  return TRUE;
}

static u8 battleNavigationHeld;

static u8 usableBagItem(Player *player, u8 slot_index) {
  return slot_index == 0 ? SLIME_GEL : APPLE;
}

static u8 countInventory(Player *player, u8 item) {
  u8 index;
  u16 total = 0;

  for (index = 0; index < INVENTORY_SIZE; index++) {
    if (player->inventory[index].item == item) {
      total = (u16) (total + player->inventory[index].count);
    }
  }
  return total > 255 ? 255 : (u8) total;
}

static u8 consumeInventory(Player *player, u8 item) {
  u8 index;

  for (index = 0; index < INVENTORY_SIZE; index++) {
    if (player->inventory[index].item == item &&
        player->inventory[index].count > 0) {
      player->inventory[index].count--;
      if (player->inventory[index].count == 0) {
        player->inventory[index].item = 0;
      }
      return TRUE;
    }
  }
  return FALSE;
}

void mon64BattleInput(NUContData *pads) {
  u8 side = mon_battle.acting_side;
  u8 player_num = mon_battle.side_player[side];
  NUContData *cont;
  s8 dx;
  s8 dy;
  u8 moved;

  /* Any player at the couch may push the log along.  Making the reader wait
     for whoever happens to own the current turn is the kind of rule that only
     makes sense to the person who wrote it. */
  if (mon_battle.phase == MON_PHASE_MESSAGE) {
    u8 index;

    for (index = 0; index < active_player_count; index++) {
      if (!(pads[index].trigger & (A_BUTTON | START_BUTTON | B_BUTTON))) {
        continue;
      }
      /* First press finishes the line, second advances it: skipping a line
         unread is the one thing a mashing player must not be able to do. */
      if (mon_battle.message_reveal < (u16) (MON_MESSAGE_LENGTH * 4)) {
        mon_battle.message_reveal = MON_MESSAGE_LENGTH * 4;
      } else {
        message_hold = MESSAGE_AUTO_ADVANCE;
      }
      break;
    }
    return;
  }

  if (!mon_battle.side_is_player[side] || player_num >= MAX_PLAYERS) {
    return;
  }
  cont = &pads[player_num];
  moved = navigationEdge(cont, &battleNavigationHeld, &dx, &dy);

  switch (mon_battle.phase) {

    case MON_PHASE_COMMAND:
      if (moved) {
        u8 cursor = mon_battle.command_cursor;
        if (dx != 0) {
          cursor = (u8) (cursor ^ 1);
        } else if (dy != 0) {
          cursor = (u8) (cursor ^ 2);
        }
        mon_battle.command_cursor = cursor;
      }
      if (cont->trigger & A_BUTTON) {
        switch (mon_battle.command_cursor) {
          case 0:
            mon_battle.phase = MON_PHASE_MOVE;
            mon_battle.move_cursor = 0;
            break;
          case 1:
            mon_battle.phase = MON_PHASE_TEAM;
            mon_battle.team_cursor = 0;
            break;
          case 2:
            mon_battle.phase = MON_PHASE_BAG;
            mon_battle.bag_cursor = 0;
            break;
          default:
            mon_battle.action[side] = MON_ACTION_RUN;
            commitSide(side);
            break;
        }
      }
      return;

    case MON_PHASE_MOVE: {
      MonFighter *f = &mon_battle.fighter[side];

      if (moved && f->move_count > 0) {
        u8 cursor = mon_battle.move_cursor;
        if (dx != 0) {
          cursor = (u8) (cursor ^ 1);
        } else if (dy != 0) {
          cursor = (u8) (cursor ^ 2);
        }
        if (cursor < f->move_count) {
          mon_battle.move_cursor = cursor;
        }
      }
      if (cont->trigger & B_BUTTON) {
        mon_battle.phase = MON_PHASE_COMMAND;
        return;
      }
      if (cont->trigger & A_BUTTON) {
        if (f->pp[mon_battle.move_cursor] == 0) {
          pushMessage("NO POWER LEFT FOR THAT MOVE.");
          return;
        }
        mon_battle.action[side] = MON_ACTION_MOVE;
        mon_battle.action_arg[side] = mon_battle.move_cursor;
        commitSide(side);
      }
      return;
    }

    case MON_PHASE_TEAM:
      if (moved && dy != 0) {
        mon_battle.team_cursor = (u8) ((mon_battle.team_cursor +
          (dy > 0 ? 1 : MON_PARTY_SIZE - 1)) % MON_PARTY_SIZE);
      }
      if (cont->trigger & B_BUTTON) {
        mon_battle.phase = MON_PHASE_COMMAND;
        return;
      }
      if (cont->trigger & A_BUTTON) {
        PartyMon *mon = &mon_party[player_num][mon_battle.team_cursor];
        if (mon->species == MON_NONE || mon->hp == 0 ||
            mon_battle.team_cursor ==
              mon_battle.fighter[side].party_slot) {
          return;
        }
        storeFighter(side);
        pushNamed(fighterName(side), ", COME BACK!");
        loadFighter(side, mon->species, mon->level, mon->hp,
          mon_battle.team_cursor);
        pushNamed(mon_species[mon->species].name, " GO!");
        /* Switching gives up the turn, which is what makes it a decision. */
        mon_battle.action[side] = MON_ACTION_NONE;
        commitSide(side);
      }
      return;

    case MON_PHASE_BAG:
      if (moved && dy != 0) {
        mon_battle.bag_cursor = (u8) (mon_battle.bag_cursor ^ 1);
      }
      if (cont->trigger & B_BUTTON) {
        mon_battle.phase = MON_PHASE_COMMAND;
        return;
      }
      if (cont->trigger & A_BUTTON) {
        Player *player = &players[player_num];
        u8 item = usableBagItem(player, mon_battle.bag_cursor);

        if (countInventory(player, item) == 0) {
          return;
        }
        if (item == SLIME_GEL) {
          if (mon_battle.kind != MON_BATTLE_WILD) {
            setPhase(MON_PHASE_COMMAND);
            pushMessage("THAT IS NOT YOURS TO CATCH!");
            return;
          }
          if (monPartyCount(player_num) >= MON_PARTY_SIZE) {
            setPhase(MON_PHASE_COMMAND);
            pushMessage("YOUR TEAM IS FULL.");
            return;
          }
          consumeInventory(player, item);
          pushMessage("YOU THREW SOME GEL!");
          attemptCatch();
          return;
        }
        consumeInventory(player, item);
        healFighter(side, 20);
        pushNamed(fighterName(side), " ATE AN APPLE.");
        mon_battle.action[side] = MON_ACTION_NONE;
        commitSide(side);
      }
      return;

    default:
      return;
  }
}

/* ------------------------------------------------------------------ */
/* Per-frame step.                                                          */
/* ------------------------------------------------------------------ */

static void stepMessages(float delta) {
  if (mon_battle.message_count == 0) {
    /* The queue is empty: go where the phase that queued it wanted to go. */
    if (mon_battle.phase == MON_PHASE_MESSAGE) {
      mon_battle.phase = mon_battle.next_phase;
      if (mon_battle.phase == MON_PHASE_MESSAGE) {
        mon_battle.phase = MON_PHASE_COMMAND;
      }
    }
    return;
  }
  mon_battle.message_reveal =
    (u16) (mon_battle.message_reveal + MESSAGE_REVEAL_RATE);
  if (mon_battle.message_reveal < (u16) (MON_MESSAGE_LENGTH * 4)) {
    return;
  }
  message_hold += delta;
  if (message_hold < MESSAGE_AUTO_ADVANCE) {
    return;
  }
  message_hold = 0;
  mon_battle.message_reveal = 0;
  mon_battle.message_head =
    (u8) ((mon_battle.message_head + 1) % MON_MESSAGE_QUEUE);
  mon_battle.message_count--;
}

static void stepCatch(float delta) {
  mon_battle.catch_time += delta;
  /* One shake every twenty frames, then the verdict.  The wobble is the
     whole drama of a catch and it costs a counter. */
  if (mon_battle.catch_time < 20.f * (mon_battle.catch_shakes + 1)) {
    return;
  }
  if (mon_battle.catch_result) {
    u8 player_num = mon_battle.side_player[0];
    MonFighter *target = &mon_battle.fighter[1];

    addToParty(player_num, target->species, target->level);
    playSound(SOUND_PICKUP);
    pushNamed(mon_species[target->species].name, " WAS CAUGHT!");
    endBattle(NULL);
    return;
  }
  pushMessage("IT BROKE FREE!");
  /* A failed throw still costs the turn: the creature gets its move. */
  mon_battle.action[0] = MON_ACTION_NONE;
  commitSide(0);
}

static void stepBattle(float delta) {
  u8 index;

  mon_battle.scene_time += delta;
  for (index = 0; index < 2; index++) {
    if (mon_battle.lunge[index] > 0) {
      mon_battle.lunge[index] -= delta;
    }
    if (mon_battle.hurt[index] > 0) {
      mon_battle.hurt[index] -= delta;
    }
    if (mon_battle.faint[index] > 0 && mon_battle.faint[index] < 30.f) {
      mon_battle.faint[index] += delta;
    }
  }

  /*
   * The log always goes first, and nothing else runs until it is empty.
   * That single rule is what keeps the battle readable: no turn can advance
   * past a line the player has not been shown, however many events one move
   * produced.
   */
  stepMessages(delta);
  if (mon_battle.message_count > 0) {
    return;
  }

  switch (mon_battle.phase) {
    case MON_PHASE_CATCH:
      stepCatch(delta);
      break;
    case MON_PHASE_RESOLVE:
      resolveStep();
      break;
    case MON_PHASE_END:
      /* Everything the battle changed is already written back through
         storeFighter and the party record; closing it is a flag and the
         view the players lent it. */
      releaseBattleView();
      mon_battle.active = FALSE;
      mon_battle.phase = MON_PHASE_NONE;
      break;
    default:
      break;
  }
}

void mon64Update(float delta) {
  u8 index;

  if (!mon64Enabled()) {
    return;
  }

  if (mon_battle.active) {
    stepBattle(delta);
    return;
  }

  /* Party health comes back on its own.  There is nowhere to rest in a world
     the player may be a thousand blocks from spawn in, and a collection game
     that strands you is a collection game you stop playing. */
  party_regen_time += delta;
  if (party_regen_time >= PARTY_REGEN_INTERVAL) {
    u8 player_num;

    party_regen_time = 0;
    for (player_num = 0; player_num < MAX_PLAYERS; player_num++) {
      for (index = 0; index < MON_PARTY_SIZE; index++) {
        PartyMon *mon = &mon_party[player_num][index];
        u16 max_hp;
        if (mon->species == MON_NONE) {
          continue;
        }
        max_hp = monMaxHealth(mon->species, mon->level);
        if (mon->hp < max_hp) {
          mon->hp++;
        }
      }
    }
  }

  if (pvp_challenger != MON_NONE) {
    pvp_challenge_time -= delta;
    if (pvp_challenge_time <= 0) {
      pvp_challenger = MON_NONE;
      pvp_target = MON_NONE;
    }
  }

  for (index = 0; index < MON_MAX_ROAMERS; index++) {
    if (mon_roamers[index].active) {
      updateRoamer(&mon_roamers[index], delta);
    }
  }

  roamer_spawn_time -= delta;
  if (roamer_spawn_time <= 0) {
    roamer_spawn_time = trySpawnRoamer() ? ROAM_SPAWN_DELAY :
      ROAM_FAILED_SPAWN_DELAY;
  }

  /* Refresh what each player could act on, once, so the prompt the HUD draws
     and the creature the A button takes are the same creature. */
  for (index = 0; index < MAX_PLAYERS; index++) {
    roamer_target[index] = index < active_player_count &&
      playerAlive(&players[index]) ? findRoamerTarget(index) : MON_NONE;
  }

  /* An aggressive creature that has closed the distance starts the fight
     itself; the player still had the whole approach to walk away. */
  for (index = 0; index < MON_MAX_ROAMERS; index++) {
    MonRoamer *roamer = &mon_roamers[index];
    u8 player_num;

    if (!roamer->active || roamer->state != ROAM_APPROACH ||
        roamer->kind != MON_ROAMER_WILD) {
      continue;
    }
    for (player_num = 0; player_num < active_player_count; player_num++) {
      float dx;
      float dz;
      /* Never start a fight with a player who is on their death screen: the
         battle takes the whole console, and they cannot even see it. */
      if (!playerAlive(&players[player_num]) ||
          monPartyLead(player_num) == MON_NONE) {
        continue;
      }
      dx = players[player_num].position.x - roamer->position.x;
      dz = players[player_num].position.z - roamer->position.z;
      if (dx * dx + dz * dz < ROAM_AGGRO_RANGE * ROAM_AGGRO_RANGE) {
        beginWildBattle(player_num, roamer);
        return;
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/* Persistence.                                                             */
/* ------------------------------------------------------------------ */

/*
 * The party as a flat blob.
 *
 * Deliberately not called from storage.c yet.  Saves still write the original
 * fixed footprint and are refused away from spawn, and the per-chunk diff
 * format that fixes that will move every offset in the file -- adding a
 * section to the format twice is how save formats acquire the bug that eats
 * worlds.  The blob is ready for whichever version bump lands first; the two
 * lines it needs are in docs/mon64.md.
 */
u32 mon64SaveSize(void) {
  return (u32) (MAX_PLAYERS * MON_PARTY_SIZE * 6);
}

void mon64SaveBlob(u8 *out) {
  u8 player_num;
  u8 index;

  for (player_num = 0; player_num < MAX_PLAYERS; player_num++) {
    for (index = 0; index < MON_PARTY_SIZE; index++) {
      PartyMon *mon = &mon_party[player_num][index];
      *out++ = mon->species;
      *out++ = mon->level;
      *out++ = (u8) (mon->xp >> 8);
      *out++ = (u8) (mon->xp & 0xFF);
      *out++ = (u8) (mon->hp >> 8);
      *out++ = (u8) (mon->hp & 0xFF);
    }
  }
}

void mon64LoadBlob(const u8 *in, u32 length) {
  u8 player_num;
  u8 index;

  if (length < mon64SaveSize()) {
    return;
  }
  for (player_num = 0; player_num < MAX_PLAYERS; player_num++) {
    for (index = 0; index < MON_PARTY_SIZE; index++) {
      PartyMon *mon = &mon_party[player_num][index];
      u8 species = *in++;
      u8 level = *in++;
      u16 xp = (u16) (*in++ << 8);
      xp = (u16) (xp | *in++);
      mon->hp = (u16) (*in++ << 8);
      mon->hp = (u16) (mon->hp | *in++);
      /* A species or level outside the tables means a file from a build with
         a different roster.  Drop that slot rather than index off the end of
         mon_species with it. */
      if (species >= MON_SPECIES_COUNT || level == 0 ||
          level > MON_MAX_LEVEL) {
        mon->species = MON_NONE;
        mon->level = 0;
        mon->xp = 0;
        mon->hp = 0;
        continue;
      }
      mon->species = species;
      mon->level = level;
      mon->xp = xp;
      if (mon->hp > monMaxHealth(species, level)) {
        mon->hp = monMaxHealth(species, level);
      }
    }
  }
}
