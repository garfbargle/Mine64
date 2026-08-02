#ifndef PLAYER_H
#define PLAYER_H

#include <nusys.h>
#include "math.h"

/* The Nintendo 64 has four controller ports.  Keep player state indexed by
   port so a joining controller always owns the matching local player. */
#define MAX_PLAYERS 4

/*
 * A player's position marks the eye, this many blocks above the feet, while
 * every other entity's marks the ground it stands on.  Anything comparing the
 * two -- a mob deciding whether the player is on its level, a hit test --
 * has to subtract this first, or it is wrong by a block and a half on flat
 * ground and quietly never fires at all.
 */
#define PLAYER_EYE_HEIGHT 1.5f
#define INVENTORY_COLUMNS 9
#define INVENTORY_STORAGE_ROWS 3
#define INVENTORY_HOTBAR_ROWS 1
#define INVENTORY_SIZE (INVENTORY_COLUMNS * (INVENTORY_STORAGE_ROWS + INVENTORY_HOTBAR_ROWS))
#define INVENTORY_HOTBAR_START (INVENTORY_COLUMNS * INVENTORY_STORAGE_ROWS)
#define MAX_ITEM_STACK 64
#define WOOD_BREAK_TIME 36.f
#define CRAFTING_TABLE_COLUMNS 3
#define CRAFTING_TABLE_ROWS 3
#define CRAFTING_SIZE (CRAFTING_TABLE_COLUMNS * CRAFTING_TABLE_ROWS)
#define PLAYER_CRAFTING_COLUMNS 2
#define PLAYER_CRAFTING_ROWS 2
#define CAMERA_FIRST_PERSON 0
#define CAMERA_THIRD_PERSON 1
#define PLAYER_MAX_HEALTH 20
#define PLAYER_MAX_HUNGER 20
#define PLAYER_ATTACK_DURATION 12.f
#define PLAYER_VAULT_DURATION 18.f
/* Frames the death screen ignores its own button for.  Long enough that the
   A press that was already being mashed at the moment of death cannot spend
   the screen before the player has read it. */
#define PLAYER_RESPAWN_DELAY 40.f
#define PLAYER_OBJECTIVE_COUNT 8
#define CRAFT_RECIPE_COUNT 24
#define POCKET_RECIPE_COUNT 4

enum InventoryArea {
  INVENTORY_AREA_CRAFTING,
  INVENTORY_AREA_OUTPUT,
  INVENTORY_AREA_ITEMS
};

/* Item stacks use block IDs for placeable items and higher IDs for tools. */
typedef struct {
  u8 item;
  u8 count;
} ItemStack;

/* The menu presents recipes, not the implementation-facing 2x2/3x3 matrix.
   Every current recipe has at most two ingredient types. */
typedef struct {
  u8 result_item;
  u8 result_count;
  u8 ingredient_item[2];
  u8 ingredient_count[2];
} CraftRecipe;

typedef struct {
  Vector3 position;
  float pitch;
  float yaw;
  /* Transient pose state; it is intentionally not stored in save files. */
  float body_yaw;
  /* Left and right steer rather than step sideways, so the view always points
     where the player is headed.  Off restores strafing. */
  u8 stick_turns;
  /* Live turn rates in degrees per 60 Hz frame, which the stick sets a target
     for rather than writing directly.  Carrying the rate across frames is what
     rounds off the start and stop of every turn; aiming with Z and steering
     share the yaw rate so releasing Z hands a turn over instead of cutting it.
     Transient like the rest of the pose. */
  float look_rate_yaw;
  float look_rate_pitch;
  float walk_time;
  float walk_swing;
  float y_velocity;
  float fall_distance;
  float vault_time;
  float camera_y_offset;
  /* Combat state is transient, so loading a world never resumes a player at
     one hit point or halfway through an animation. */
  Vector3 knockback_velocity;
  float attack_time;
  float hurt_time;
  /* Death is a state the player sits in rather than an instant teleport: the
     body stops where it fell and that player's own viewport goes black until
     they ask for a new life.  Transient like the rest of combat, so no save
     ever loads a corpse.  death_time is how long they have been dead. */
  u8 dead;
  float death_time;
  float objective_time;
  /* Hunger is deliberately cheap state: one visible byte plus two transient
     accumulators.  The accumulator lets tiny per-frame movement costs remain
     smooth without ticking or scanning an inventory every frame. */
  float hunger_progress;
  float survival_time;
  /*
   * Where this player wakes up, once they have slept somewhere.
   *
   * Transient, exactly like the bed that sets it: details are not carried by
   * any save format yet, so a respawn point that survived a reload would
   * point at a bed that had reverted to the crafting table proxying it.  Both
   * belong in the same version bump.
   */
  int spawn_x;
  int spawn_z;
  u8 spawn_set;
  /* Lying down, waiting for the rest of the party.  Cleared by moving, by
     being hurt, and by the morning itself. */
  u8 sleeping;
  u8 health;
  u8 hunger;
  u8 objective_stage;
  u8 camera_mode;
  int held_block;
  ItemStack inventory[INVENTORY_SIZE];
  ItemStack crafting[CRAFTING_SIZE];
  ItemStack carried_item;
  u8 selected_hotbar_slot;
  u8 inventory_cursor;
  u8 crafting_cursor;
  u8 inventory_area;
  u8 crafting_table_open;
  u8 active;

  /* x and z are unbounded world block coordinates; y still spans 0..31. */
  int target_x;
  u8 target_y;
  int target_z;
  s8 build_offset_x;
  s8 build_offset_y;
  s8 build_offset_z;
  u8 target_present;

  /* Breaking is transient interaction state, separate from the world and
     save data.  It lets a held punch visibly take time instead of deleting a
     block on a single button press.  x and z match target_x/z: unbounded. */
  u8 breaking;
  int breaking_x;
  u8 breaking_y;
  int breaking_z;
  float break_progress;
  float break_time;
} Player;

/*
 * A dead player still owns their slot, their inventory and their quarter of
 * the screen, so `active` stays true and every per-player loop keeps its
 * shape.  What changes is that nothing in the world may see them: this is
 * the question mobs, creatures and the renderer ask about a body before
 * chasing, hurting or drawing it.
 */
static __inline__ __attribute__((unused)) u8 playerAlive(const Player *player) {
  return player->active && !player->dead;
}

/* Why the last sleep attempt did what it did, and how many frames of HUD line
   it has left.  Indexed by player so a split screen can disagree. */
#define SLEEP_REASON_DAYTIME 0
#define SLEEP_REASON_MONSTERS 1
#define SLEEP_REASON_WAITING 2
#define SLEEP_REASON_MORNING 3
extern u8 sleep_message[MAX_PLAYERS];
extern u8 sleep_reason[MAX_PLAYERS];

extern Player players[MAX_PLAYERS];
extern u8 active_player_count;
extern u8 inventory_player;
extern const CraftRecipe craft_recipes[CRAFT_RECIPE_COUNT];

void initPlayers();
void updatePlayers();
/* Spend the buttons the front end's picker is holding, so a card that ends
   with one down does not hand the press straight to the screen underneath.
   See the definition. */
void resetMenuPressLatch(void);
void updateTargetBlock(u8 player_num);
void resetPlayerInventory(Player *player);
void resetStartingInventories(void);
u8 addItemToInventory(Player *player, u8 item, u8 count);
u8 playerRecipeCount(Player *player);
u8 recipeCraftableCount(Player *player, u8 recipe);
u8 craftSelectedRecipe(Player *player, u8 craft_all);
void damagePlayer(u8 player_num, u8 damage, Vector3 source);
/* Called when the survival rule changes; see the definition for why turning
   it off has to fill the meters rather than just stop them. */
void applySurvivalRule(void);
const char *playerObjectiveTitle(Player *player);
const char *playerObjectiveHint(Player *player);

/* Used by the local join flow and save-game compatibility code. */
void activatePlayer(u8 player_num);

#endif /* PLAYER_H */
