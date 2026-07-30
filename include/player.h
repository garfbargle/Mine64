#ifndef PLAYER_H
#define PLAYER_H

#include <nusys.h>
#include "math.h"

#define MAX_PLAYERS 2

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
  u8 active;

  u8 target_x;
  u8 target_y;
  u8 target_z;
  s8 build_offset_x;
  s8 build_offset_y;
  s8 build_offset_z;
  u8 target_present;
} Player;

extern Player players[MAX_PLAYERS];
extern u8 active_player_count;

void initPlayers();
void updatePlayers();
void updateTargetBlock(u8 player_num);

/* Used by save-game compatibility code. */
void activatePlayerTwo();

#endif /* PLAYER_H */
