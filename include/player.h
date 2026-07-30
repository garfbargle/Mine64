#ifndef PLAYER_H
#define PLAYER_H

#include <nusys.h>
#include "math.h"

#define MAX_PLAYERS 2
#define INVENTORY_COLUMNS 9
#define INVENTORY_STORAGE_ROWS 3
#define INVENTORY_HOTBAR_ROWS 1
#define INVENTORY_SIZE (INVENTORY_COLUMNS * (INVENTORY_STORAGE_ROWS + INVENTORY_HOTBAR_ROWS))
#define INVENTORY_HOTBAR_START (INVENTORY_COLUMNS * INVENTORY_STORAGE_ROWS)
#define MAX_ITEM_STACK 64
#define WOOD_BREAK_TIME 36.f

/* Item IDs use the existing block IDs for now.  This makes the inventory UI
   useful immediately, while a later item table can add sticks and crafting
   tables without changing the slot representation. */
typedef struct {
  u8 item;
  u8 count;
} ItemStack;

typedef struct {
  Vector3 position;
  float pitch;
  float yaw;
  /* Transient pose state; it is intentionally not stored in save files. */
  float body_yaw;
  float walk_time;
  float walk_swing;
  float y_velocity;
  int held_block;
  ItemStack inventory[INVENTORY_SIZE];
  u8 selected_hotbar_slot;
  u8 inventory_cursor;
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
} Player;

extern Player players[MAX_PLAYERS];
extern u8 active_player_count;

void initPlayers();
void updatePlayers();
void updateTargetBlock(u8 player_num);
void resetPlayerInventory(Player *player);
u8 addItemToInventory(Player *player, u8 item, u8 count);

/* Used by save-game compatibility code. */
void activatePlayerTwo();

#endif /* PLAYER_H */
