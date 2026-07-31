#ifndef PLAYER_H
#define PLAYER_H

#include <nusys.h>
#include "math.h"

/* The Nintendo 64 has four controller ports.  Keep player state indexed by
   port so a joining controller always owns the matching local player. */
#define MAX_PLAYERS 4
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
#define PLAYER_ATTACK_DURATION 12.f
#define PLAYER_VAULT_DURATION 18.f
#define PLAYER_OBJECTIVE_COUNT 8
#define CRAFT_RECIPE_COUNT 12
#define POCKET_RECIPE_COUNT 3

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
  float objective_time;
  u8 health;
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

  u8 target_x;
  u8 target_y;
  u8 target_z;
  s8 build_offset_x;
  s8 build_offset_y;
  s8 build_offset_z;
  u8 target_present;

  /* Breaking is transient interaction state, separate from the world and
     save data.  It lets a held punch visibly take time instead of deleting a
     block on a single button press. */
  u8 breaking;
  u8 breaking_x;
  u8 breaking_y;
  u8 breaking_z;
  float break_progress;
  float break_time;
} Player;

extern Player players[MAX_PLAYERS];
extern u8 active_player_count;
extern u8 inventory_player;
extern const CraftRecipe craft_recipes[CRAFT_RECIPE_COUNT];

void initPlayers();
void updatePlayers();
void updateTargetBlock(u8 player_num);
void resetPlayerInventory(Player *player);
u8 addItemToInventory(Player *player, u8 item, u8 count);
u8 playerRecipeCount(Player *player);
u8 recipeCraftableCount(Player *player, u8 recipe);
u8 craftSelectedRecipe(Player *player, u8 craft_all);
void damagePlayer(u8 player_num, u8 damage, Vector3 source);
const char *playerObjectiveTitle(Player *player);
const char *playerObjectiveHint(Player *player);

/* Used by the local join flow and save-game compatibility code. */
void activatePlayer(u8 player_num);

#endif /* PLAYER_H */
