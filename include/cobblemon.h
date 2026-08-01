#ifndef COBBLEMON_H
#define COBBLEMON_H

#include <nusys.h>
#include "math.h"
#include "player.h"

/*
 * COBBLEMON -- creature collecting inside the block world.
 *
 * The mod is switched on when a world is created (MOD_COBBLEMON) and read
 * while the world is played, so it changes nothing the generator produces: a
 * creature is an entity, never a block, and where one stands is never stored.
 * That is what lets it ride on top of the streaming world without a single
 * new save field or a second source of truth about terrain.
 *
 * Three deliberate budget decisions hold the whole feature inside an
 * unmodified console, and every later addition has to respect them:
 *
 * 1. THE ENTITY COUNT DOES NOT GROW.  Roamers do not add a pool beside the
 *    mob pool -- they take slots out of it (see cobblemonRoamerReserve).
 *    Turning the mod on trades farm animals for creatures; it never asks the
 *    RSP to transform more boxes than a vanilla world already does.
 *
 * 2. A BATTLE IS A PAUSE, NOT A SCENE.  It is a mode inside GAME rather than
 *    a screen of its own: the world is already meshed and the camera is
 *    already there, so a battle costs two creature models and a flat overlay
 *    while movement, mobs, items, trees and streaming all stop.  It is the
 *    cheapest frame the game ever draws with terrain on it.
 *
 * 3. EVERY BATTLE NUMBER IS AN INTEGER.  Levels, damage, catch odds and
 *    experience never touch the FPU.  The two hardware freezes this project
 *    has chased both ended in float edge cases, and a turn-based system has
 *    no reason to offer a third.
 *
 * Models are the game's own idiom: shaded boxes with vertex colours, no
 * texture, no atlas, no skeleton.  Eighteen species share six rigs and differ
 * by proportion and palette, so the roster costs a table rather than art.
 */

/* Species are addressed by index into cobble_species. */
#define COBBLE_SPECIES_COUNT 18
#define COBBLE_MOVE_COUNT 22
#define COBBLE_RIG_COUNT 6
#define COBBLE_NONE 0xFF

#define COBBLE_MAX_PARTS 8
#define COBBLE_PARTY_SIZE 6
#define COBBLE_MOVES 4
#define COBBLE_MAX_LEVEL 50

/*
 * Simulated roamers, and how many of them may be drawn in one viewport.
 * Four is the passive mob budget this reserve is taken from; two visible
 * matches the pair a battle puts on screen, so the model path has exactly one
 * worst case to be fast for.
 */
#define COBBLE_MAX_ROAMERS 4
#define COBBLE_RENDER_SLOTS 2

/* Elemental types.  Six is what a player can hold in their head and what a
   readable colour swatch can distinguish at 320x240. */
enum CobbleType {
  COBBLE_GRASS,
  COBBLE_FIRE,
  COBBLE_WATER,
  COBBLE_EARTH,
  COBBLE_SPARK,
  COBBLE_STONE,
  COBBLE_TYPE_COUNT
};

/*
 * The seventh column of the move table.  A plain move is always neutral, so
 * every creature has something to hit an immune-looking matchup with and the
 * chart itself stays a readable six by six.  It is a move property only: no
 * creature is ever of this type, which is why it lives past the enum rather
 * than inside it.
 */
#define COBBLE_TYPE_PLAIN COBBLE_TYPE_COUNT

/* Move side effects.  Each is a single rule the battle log can state in one
   short line, because a move whose effect cannot be explained on a CRT in
   thirty characters may as well not have one. */
enum CobbleMoveEffect {
  COBBLE_EFFECT_NONE,
  COBBLE_EFFECT_HEAL,      /* restores half of the user's missing health */
  COBBLE_EFFECT_BUFF_ATK,
  COBBLE_EFFECT_BUFF_DEF,
  COBBLE_EFFECT_DRAIN,     /* user recovers half the damage dealt */
  COBBLE_EFFECT_HIGH_CRIT,
  COBBLE_EFFECT_MULTI,     /* strikes twice */
  COBBLE_EFFECT_RECOIL     /* user takes a quarter of the damage dealt */
};

/* Where a species is found.  A bitmask over the block the creature is
   standing on plus one time-of-day flag, tested against the spawn pad the
   roamer pool has already proved walkable. */
#define COBBLE_HAB_GRASS 0x01
#define COBBLE_HAB_SAND  0x02
#define COBBLE_HAB_STONE 0x04
#define COBBLE_HAB_SHORE 0x08  /* grass or sand with water within two blocks */
#define COBBLE_HAB_NIGHT 0x10  /* only after dusk */
#define COBBLE_HAB_DAY   0x20  /* only before dusk */

/* Which of a species' three colours a box takes. */
#define COBBLE_TONE_PRIMARY 0
#define COBBLE_TONE_SECONDARY 1
#define COBBLE_TONE_ACCENT 2

/*
 * Animation roles.  Parts animate by moving, never by rotating: the rig has
 * no joints, exactly like the sheep and the pig, so a pose costs an addition
 * per box instead of a matrix concatenation.
 */
#define COBBLE_ROLE_STATIC 0
#define COBBLE_ROLE_BODY 1     /* breathes, and carries the hurt shake */
#define COBBLE_ROLE_HEAD 2     /* bobs while idle, dips while grazing */
#define COBBLE_ROLE_LEG_A 3    /* front-left and back-right */
#define COBBLE_ROLE_LEG_B 4    /* the opposite diagonal */
#define COBBLE_ROLE_TAIL 5
#define COBBLE_ROLE_WING 6
#define COBBLE_ROLE_ARM_A 7
#define COBBLE_ROLE_ARM_B 8

/*
 * One box of a rig, in model units where a block is BLOCK_SIZE (64) across.
 * Offsets are from the creature's ground contact point; extents are halves,
 * so a part occupies offset +- extent before the species' scale is applied.
 */
typedef struct {
  s8 x;
  s8 y;
  s8 z;
  u8 sx;
  u8 sy;
  u8 sz;
  u8 tone;
  u8 role;
} CobblePart;

typedef struct {
  u8 part_count;
  /* Standing height in model units, for the camera framing and the ground
     shadow.  Authored rather than derived so a rig can lie about it (a
     serpent is longer than it is tall and should not be framed as if it
     towered). */
  u8 height;
  CobblePart parts[COBBLE_MAX_PARTS];
} CobbleRig;

typedef struct {
  /* Nine characters is what the battle panel can show beside a level. */
  const char *name;
  u8 rig;
  u8 type;
  /* Percent of the rig's authored size.  Evolution mostly means "the same
     creature, larger and more saturated", which reads instantly on a CRT. */
  u8 scale;
  u8 base_hp;
  u8 base_attack;
  u8 base_defense;
  u8 base_speed;
  /* 0..255.  Rarer and stronger creatures resist the gel. */
  u8 catch_rate;
  u8 xp_yield;
  /* Relative spawn weight within a habitat; zero never spawns wild. */
  u8 spawn_weight;
  u8 habitat;
  u8 evolves_to;
  u8 evolve_level;
  /* Starts a battle on sight after dark, unless the world is PEACEFUL. */
  u8 aggressive;
  u8 primary[3];
  u8 secondary[3];
  u8 accent[3];
  u8 moves[COBBLE_MOVES];
  u8 move_levels[COBBLE_MOVES];
} CobbleSpecies;

typedef struct {
  /* Eight characters, so four fit the move grid two across. */
  const char *name;
  u8 type;
  u8 power;      /* zero for a status move */
  u8 accuracy;   /* percent */
  u8 pp;
  u8 effect;
} CobbleMove;

/*
 * A creature the player owns.  Six bytes, because a party is per player and
 * this is the only cobblemon state a save would ever have to carry.
 * Everything else -- stats, moves, remaining PP -- is derived from these two
 * numbers and the species table, which is also what makes a rebalance safe:
 * retuning a base stat retunes every existing save.
 */
typedef struct {
  u8 species;
  u8 level;
  u16 xp;   /* progress toward the next level, not a lifetime total */
  u16 hp;   /* current; zero is fainted */
} CobbleMon;

/* Overworld creature kinds. */
#define COBBLE_ROAMER_WILD 0
#define COBBLE_ROAMER_TRAINER 1

typedef struct {
  Vector3 position;
  float yaw;
  float walk_time;
  float decision_time;
  float notice_time;
  u8 active;
  u8 kind;
  u8 species;
  u8 level;
  u8 state;
  /* Cached block beneath the feet, like the mob pool: movement checks three
     heights instead of scanning all 32 layers. */
  s8 ground_y;
  /* A trainer's team is a pure function of this, so it needs no storage and
     is the same team every time that trainer is met. */
  u32 seed;
} CobbleRoamer;

/* One side of a battle, expanded from a CobbleMon once when it is sent out.
   Nothing here is written back except hp and xp. */
typedef struct {
  u8 species;
  u8 level;
  u16 hp;
  u16 max_hp;
  u16 attack;
  u16 defense;
  u16 speed;
  u8 move[COBBLE_MOVES];
  u8 pp[COBBLE_MOVES];
  u8 move_count;
  /* Restored when the fighter leaves the field, so switching out is a real
     decision rather than a free reset. */
  s8 attack_stage;
  s8 defense_stage;
  /* Which party slot this came from; COBBLE_NONE for a wild creature. */
  u8 party_slot;
} CobbleFighter;

/* Battle kinds. */
#define COBBLE_BATTLE_WILD 0
#define COBBLE_BATTLE_TRAINER 1
#define COBBLE_BATTLE_PVP 2

/* Battle phases.  The whole encounter is one step function driven from
   cobblemonUpdate; nothing here ever blocks or waits. */
#define COBBLE_PHASE_NONE 0
#define COBBLE_PHASE_MESSAGE 1   /* typing a line, then waiting for A */
#define COBBLE_PHASE_COMMAND 2   /* FIGHT / TEAM / BAG / RUN */
#define COBBLE_PHASE_MOVE 3
#define COBBLE_PHASE_TEAM 4
#define COBBLE_PHASE_BAG 5
#define COBBLE_PHASE_RESOLVE 6   /* running the queued turn */
#define COBBLE_PHASE_CATCH 7
#define COBBLE_PHASE_END 8

/* Chosen actions, held until both sides have committed. */
#define COBBLE_ACTION_NONE 0
#define COBBLE_ACTION_MOVE 1
#define COBBLE_ACTION_SWAP 2
#define COBBLE_ACTION_ITEM 3
#define COBBLE_ACTION_RUN 4

#define COBBLE_MESSAGE_LENGTH 40
#define COBBLE_MESSAGE_QUEUE 6

typedef struct {
  u8 active;
  u8 kind;
  u8 phase;
  /* The phase to enter once the message queue drains. */
  u8 next_phase;
  /* 0 is the challenger's side, 1 the opponent's. */
  u8 side_is_player[2];
  u8 side_player[2];
  CobbleFighter fighter[2];
  /* Whose command the interface is currently taking. */
  u8 acting_side;
  /* Both sides' chosen actions for the turn being resolved. */
  u8 action[2];
  u8 action_arg[2];
  /*
   * A turn is a short script rather than a single function: one atomic step
   * runs each time the log drains, so every hit, faint and level-up gets a
   * line the player actually reads, and no frame ever does two of them at
   * once.  first_side is the speed order decided when the turn opened.
   */
  u8 resolve_index;
  u8 first_side;
  u8 escape_attempts;
  /* An NPC trainer's team.  A wild battle uses slot zero only. */
  CobbleMon npc_party[3];
  u8 npc_party_size;
  u8 npc_active;

  /* Interface cursors. */
  u8 command_cursor;
  u8 move_cursor;
  u8 team_cursor;
  u8 bag_cursor;

  /* Message queue: one line at a time, typed out, advanced with A. */
  char message[COBBLE_MESSAGE_QUEUE][COBBLE_MESSAGE_LENGTH + 1];
  u8 message_head;
  u8 message_count;
  u16 message_reveal;   /* characters revealed so far, times 4 */

  /* Presentation state.  All of it is transient and none of it changes a
     rule, so a dropped frame can never desync the battle from its display. */
  float scene_time;
  float lunge[2];
  float hurt[2];
  float faint[2];
  u8 catch_shakes;
  u8 catch_result;
  float catch_time;
  /* Where the two creatures stand, fixed when the battle opens so a slow
     frame cannot slide them. */
  Vector3 stand[2];
  float facing;
  /*
   * Each battling player's own view angle, tipped down while the fight runs
   * and put back afterwards.
   *
   * There is no battle camera: the player keeps their first-person view,
   * which is what makes the fight happen in the world they were standing in
   * rather than in a separate scene.  The cost is that a creature on the
   * ground a few blocks away sits below a level horizon and lands behind the
   * message box, so the view has to look down -- exactly as the player would
   * themselves.
   */
  float saved_pitch[2];
  /* Set when the battle is over and the world should resume next frame. */
  u8 finished;
  u16 pending_xp;
} CobbleBattle;

extern const CobbleSpecies cobble_species[COBBLE_SPECIES_COUNT];
extern const CobbleMove cobble_moves[COBBLE_MOVE_COUNT];
extern const CobbleRig cobble_rigs[COBBLE_RIG_COUNT];
/* Effectiveness as quarters: 8 is double, 4 is neutral, 2 is halved. */
extern const u8 cobble_type_chart[COBBLE_TYPE_COUNT][COBBLE_TYPE_COUNT];
/* Panel and swatch colour for each type, plus the plain column. */
extern const u8 cobble_type_color[COBBLE_TYPE_COUNT + 1][3];
extern const char *cobble_type_name[COBBLE_TYPE_COUNT + 1];

extern CobbleMon cobble_party[MAX_PLAYERS][COBBLE_PARTY_SIZE];
extern CobbleRoamer cobble_roamers[COBBLE_MAX_ROAMERS];
extern CobbleBattle cobble_battle;

/* Reset every creature, party and roamer.  Called when a world is entered,
   beside initMobs. */
void initCobblemon(void);

/*
 * How many mob slots the creature pool is borrowing.  Zero unless the mod is
 * on.  mobs.c subtracts this from its passive budget, which is what keeps the
 * simulated entity count identical to a vanilla world.
 */
u8 cobblemonRoamerReserve(void);

/* TRUE when the mod is on for this world. */
u8 cobblemonEnabled(void);

/* One simulation slice: roamer AI and spawning, party regeneration, and the
   battle step when one is running.  Called from updatePlayers beside
   updateMobs, and safe to call when the mod is off (it returns at once). */
void cobblemonUpdate(float delta);

/*
 * The A button, before block placement.  Returns TRUE when a creature or a
 * challenged player took the press, which is also the signal for the caller
 * to stop processing that frame's actions.
 *
 * A creature in front of the player takes a plain A -- the HUD prompt has
 * already said it would.  Challenging another player needs Z held as well,
 * because standing next to a friend and placing a block has to keep working
 * in a game that is mostly about placing blocks.
 */
u8 cobblemonTryInteract(u8 player_num, u8 challenge_held);

/*
 * TRUE while a battle owns every pad, which is when updatePlayers must not
 * run movement, mobs, items or trees: a battle is a full pause.
 *
 * The pad snapshot is passed in rather than read here, because updatePlayers
 * owns the one nuContDataGetExAll of the frame and a second sample would give
 * a different trigger edge to whoever asked second.
 */
u8 cobblemonBattleActive(void);
void cobblemonBattleInput(NUContData *pads);

/*
 * Derived numbers.  All integer, all pure, all safe to call from the
 * renderer: the battle panel and the party card read stats through these
 * rather than caching them.
 */
u16 cobbleMaxHealth(u8 species, u8 level);
u16 cobbleAttack(u8 species, u8 level);
u16 cobbleDefense(u8 species, u8 level);
u16 cobbleSpeed(u8 species, u8 level);
u16 cobbleXpForLevel(u8 level);
u8 cobbleKnownMoves(u8 species, u8 level, u8 *out);

/* The party slot a player would lead with, or COBBLE_NONE when every
   creature has fainted. */
u8 cobblePartyLead(u8 player_num);
u8 cobblePartyCount(u8 player_num);

/*
 * The creature a player is close enough to act on, or COBBLE_NONE.  The HUD
 * prompt and the interact hook ask the same question so they can never
 * disagree about what pressing A would do.
 */
u8 cobblemonTargetRoamer(u8 player_num);

/* Rendering, from graphics.c.  Draws the battle pair when a battle is
   running and the nearby roamers otherwise; both are capped at
   COBBLE_RENDER_SLOTS boxsets per viewport. */
void cobblemonDrawForPlayer(u8 viewer_num);
/* The full-screen battle interface, and the small overworld prompt.  Both
   leave the RDP configured for text, like every other panel in the game. */
void cobblemonDrawBattleInterface(void);
/* `center_x` and `bottom_y` are the player's viewport, so the prompt lands
   above their own hotbar in split-screen instead of somebody else's. */
void cobblemonDrawPrompt(u8 player_num, u32 center_x, u32 bottom_y);

/*
 * The party as a flat blob, for whenever the save format grows the per-world
 * section it belongs in (see docs/cobblemon.md).  Deliberately not wired into
 * storage.c: that file is the one place a mistake destroys worlds, and the
 * per-chunk diff save it is waiting on will rewrite its layout anyway.
 */
u32 cobblemonSaveSize(void);
void cobblemonSaveBlob(u8 *out);
void cobblemonLoadBlob(const u8 *in, u32 length);

#endif /* COBBLEMON_H */
