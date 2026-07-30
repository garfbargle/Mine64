#include <nusys.h>
#include "player.h"
#include "math.h"
#include "menu.h"
#include "blocks.h"
#include "graphics.h"
#include "geometry.h"
#include "storage.h"

#define START_X 32
#define START_Z 48

#define STICK_DAMPER 22
#define MOVE_SPEED (1 / 8.f)
#define JUMP_SPEED (BLOCK_SIZE / 4.5)
#define TERMINAL_SPEED (BLOCK_SIZE / 2)
#define GRAVITY (BLOCK_SIZE / 40)

#define BOX_RADIUS 0.35
#define BOX_HEIGHT 1.8
#define EYE_HEIGHT 1.5

Player players[MAX_PLAYERS];
u8 active_player_count = 1;

static Vector3 bounding_box[] = {
  {-BOX_RADIUS, -EYE_HEIGHT, -BOX_RADIUS},
  {-BOX_RADIUS, -EYE_HEIGHT,  BOX_RADIUS},
  {-BOX_RADIUS, BOX_HEIGHT - EYE_HEIGHT, -BOX_RADIUS},
  {-BOX_RADIUS, BOX_HEIGHT - EYE_HEIGHT,  BOX_RADIUS},
  { BOX_RADIUS, -EYE_HEIGHT, -BOX_RADIUS},
  { BOX_RADIUS, -EYE_HEIGHT,  BOX_RADIUS},
  { BOX_RADIUS, BOX_HEIGHT - EYE_HEIGHT, -BOX_RADIUS},
  { BOX_RADIUS, BOX_HEIGHT - EYE_HEIGHT,  BOX_RADIUS}
};

static NUContData cont_data[MAX_PLAYERS];
static OSTime last_time = 0;
static u16 down_held = FALSE;
static u16 up_held = FALSE;
static u16 act_held = FALSE;
static u8 block_dec_held[MAX_PLAYERS];
static u8 block_inc_held[MAX_PLAYERS];

static void spawnPlayer(Player *player, int x, int z) {
  int y;

  player->pitch = 0;
  player->yaw = 0;
  player->body_yaw = 0;
  player->walk_time = 0;
  player->walk_swing = 0;
  player->y_velocity = 0;
  player->held_block = COBBLESTONE;
  player->active = TRUE;
  player->target_present = FALSE;
  player->position.x = (x + 0.5) * BLOCK_SIZE;
  player->position.z = (z + 0.5) * BLOCK_SIZE;

  for (y = MAX_Y - 1; y >= 0; y--) {
    if (blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z]) {
      player->position.y = (y + 1 + EYE_HEIGHT) * BLOCK_SIZE;
      return;
    }
  }

  player->position.y = (MAX_Y + EYE_HEIGHT) * BLOCK_SIZE;
}

void initPlayers() {
  active_player_count = 1;
  spawnPlayer(&players[0], START_X, START_Z);
  players[1].active = FALSE;
  players[1].target_present = FALSE;
}

void activatePlayerTwo() {
  if (!players[1].active) {
    /* A nearby, separate spawn avoids an immediate overlap without needing
       a second world copy or an expensive entity system. */
    spawnPlayer(&players[1], START_X + 3, START_Z);
    active_player_count = 2;
    player_two_joined_message = 120;
  }
}

void updateTargetBlock(u8 player_num) {
  Player *player = &players[player_num];
  float t;
  Vector3i step;
  Vector3 direction = {0, 0, -1};

  direction = rotateX(direction, player->pitch);
  direction = rotateY(direction, -player->yaw);

  player->target_x = player->position.x / BLOCK_SIZE;
  player->target_y = player->position.y / BLOCK_SIZE;
  player->target_z = player->position.z / BLOCK_SIZE;

  player->target_present = TRUE;
  while (player->target_x >= MAX_X || player->target_y >= MAX_Y || player->target_z >= MAX_Z ||
    !blocks[player->target_x * MAX_Y * MAX_Z + player->target_y * MAX_Z + player->target_z]) {
    t = 9999;

    rayStepAxis(player->position.x, direction.x, player->target_x, &t, &step, 0);
    rayStepAxis(player->position.y, direction.y, player->target_y, &t, &step, 1);
    rayStepAxis(player->position.z, direction.z, player->target_z, &t, &step, 2);

    if (t > BLOCK_SIZE * 6 || (player->target_x == 0 && step.x < 0) ||
        (player->target_y == 0 && step.y < 0) || (player->target_z == 0 && step.z < 0)) {
      player->target_present = FALSE;
      break;
    }

    player->target_x += step.x;
    player->target_y += step.y;
    player->target_z += step.z;
  }

  if (player->target_present) {
    player->build_offset_x = -step.x;
    player->build_offset_y = -step.y;
    player->build_offset_z = -step.z;
  }
}

static void boxBlockRange(Vector3 pos, Vector3i *min_block, Vector3i *max_block) {
  int i;
  Vector3 v;
  Vector3 box_min = {9999, 9999, 9999};
  Vector3 box_max = {-9999, -9999, -9999};

  for (i = 0; i < 8; i++) {
    v = add(pos, bounding_box[i]);
    box_min.x = min(box_min.x, v.x);
    box_min.y = min(box_min.y, v.y);
    box_min.z = min(box_min.z, v.z);
    box_max.x = max(box_max.x, v.x);
    box_max.y = max(box_max.y, v.y);
    box_max.z = max(box_max.z, v.z);
  }

  min_block->x = floor(box_min.x);
  min_block->y = floor(box_min.y);
  min_block->z = floor(box_min.z);
  max_block->x = floor(box_max.x);
  max_block->y = floor(box_max.y);
  max_block->z = floor(box_max.z);
}

static u8 boxObstructed(Vector3 pos, int override_axis, int override_block) {
  int x, y, z;
  Vector3i min_block, max_block;

  boxBlockRange(pos, &min_block, &max_block);
  *ati(&min_block, override_axis) = override_block;
  *ati(&max_block, override_axis) = override_block;

  for (x = min_block.x; x <= max_block.x; x++) {
    for (y = min_block.y; y <= max_block.y; y++) {
      for (z = min_block.z; z <= max_block.z; z++) {
        if (x < 0 || y < 0 || z < 0 || x >= MAX_X || z >= MAX_Z ||
            (y < MAX_Y && blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z])) {
          return TRUE;
        }
      }
    }
  }
  return FALSE;
}

static float detectCollision(Player *player, Vector3 velocity, float max_t, int *collision_axis) {
  float t;
  Vector3i step;
  int step_axis;
  Vector3 pos;
  Vector3 origin = add(player->position,
    mul(bounding_box[(velocity.x > 0) * 4 + (velocity.y > 0) * 2 + (velocity.z > 0)], BLOCK_SIZE));
  Vector3i block = divToInt(origin, BLOCK_SIZE);

  while (TRUE) {
    t = 1;
    rayStepAxis(origin.x, velocity.x, block.x, &t, &step, 0);
    rayStepAxis(origin.y, velocity.y, block.y, &t, &step, 1);
    rayStepAxis(origin.z, velocity.z, block.z, &t, &step, 2);

    if (t >= max_t) {
      return max_t;
    }
    step_axis = step.x != 0 ? 0 : (step.y != 0 ? 1 : 2);
    block = addi(block, step);
    pos = div(add(player->position, mul(velocity, t)), BLOCK_SIZE);

    if (boxObstructed(pos, step_axis, *ati(&block, step_axis))) {
      *collision_axis = step_axis;
      return t;
    }
  }
}

static void placeBlock(u8 player_num, u8 x, u8 y, u8 z) {
  int bx, by, bz, i;
  Vector3i min_block, max_block;

  if (x >= MAX_X || y >= MAX_Y || z >= MAX_Z) {
    return;
  }

  /* Co-op cannot bury either player inside a freshly placed block. */
  for (i = 0; i < active_player_count; i++) {
    boxBlockRange(div(players[i].position, BLOCK_SIZE), &min_block, &max_block);
    for (bx = min_block.x; bx <= max_block.x; bx++) {
      for (by = min_block.y; by <= max_block.y; by++) {
        for (bz = min_block.z; bz <= max_block.z; bz++) {
          if (bx == x && by == y && bz == z) {
            return;
          }
        }
      }
    }
  }

  blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z] = players[player_num].held_block;
  regenerateBlock(x, y, z);
  makeDisplayListsAt(x, z);
}

static void breakBlock(u8 x, u8 y, u8 z) {
  blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z] = AIR;
  regenerateBlock(x, y, z);
  makeDisplayListsAt(x, z);
}

static u8 onGround(Player *player) {
  int x, y, z;
  Vector3i min_block, max_block;
  Vector3 low_pos = div(player->position, BLOCK_SIZE);
  low_pos.y -= 0.01;
  boxBlockRange(low_pos, &min_block, &max_block);

  y = min_block.y;
  for (x = min_block.x; x <= max_block.x; x++) {
    for (z = min_block.z; z <= max_block.z; z++) {
      if (x < 0 || y < 0 || z < 0 || x >= MAX_X || z >= MAX_Z ||
          (y < MAX_Y && blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z])) {
        return TRUE;
      }
    }
  }
  return FALSE;
}

static void updatePlayer(u8 player_num, float delta) {
  Player *player = &players[player_num];
  NUContData *cont = &cont_data[player_num];
  Vector3 velocity = {0, 0, 0};
  float t, t_total = 0;
  int collision_axis = 0;
  s8 stick_x = cont->stick_x;
  s8 stick_y = cont->stick_y;

  if (stick_x > -4 && stick_x < 4) stick_x = 0;
  if (stick_y > -4 && stick_y < 4) stick_y = 0;

  if (cont->button & Z_TRIG) {
    player->yaw -= stick_x * delta / STICK_DAMPER;
    player->pitch += stick_y * delta / STICK_DAMPER;
    if (player->yaw < 0) player->yaw += 360;
    else if (player->yaw >= 360) player->yaw -= 360;
    if (player->pitch < 0) player->pitch += 360;
    else if (player->pitch >= 360) player->pitch -= 360;
    if (player->pitch > 90 && player->pitch < 180) player->pitch = 90;
    if (player->pitch < 270 && player->pitch > 180) player->pitch = 270;
  } else {
    velocity.x += stick_x * cosf(player->yaw * M_DTOR) - stick_y * sinf(player->yaw * M_DTOR);
    velocity.z -= stick_x * sinf(player->yaw * M_DTOR) + stick_y * cosf(player->yaw * M_DTOR);
    velocity.x *= MOVE_SPEED;
    velocity.z *= MOVE_SPEED;
  }

  /* The torso follows travel, while the avatar's head continues to follow
     the camera direction when the player stops. */
  if (velocity.x != 0 || velocity.z != 0) {
    player->body_yaw = player->yaw;
    player->walk_time += delta * 12;
    player->walk_swing = min(1, player->walk_swing + delta / 8);
  } else {
    /* Ease a stopped avatar back to a neutral pose instead of leaving a
       leg or arm hanging at the last animation frame. */
    player->walk_swing = max(0, player->walk_swing - delta / 6);
  }

  if (cont->button & L_CBUTTONS) {
    if (!block_dec_held[player_num]) {
      block_dec_held[player_num] = TRUE;
      if (--player->held_block < FIRST_PLACEABLE_BLOCK) player->held_block = BLOCK_TYPE_COUNT;
    }
  } else block_dec_held[player_num] = FALSE;

  if (cont->button & R_CBUTTONS) {
    if (!block_inc_held[player_num]) {
      block_inc_held[player_num] = TRUE;
      if (++player->held_block > BLOCK_TYPE_COUNT) player->held_block = FIRST_PLACEABLE_BLOCK;
    }
  } else block_inc_held[player_num] = FALSE;

  if (onGround(player)) {
    player->y_velocity = (cont->button & R_TRIG) ? JUMP_SPEED : 0;
  } else if (player->y_velocity > -TERMINAL_SPEED) {
    player->y_velocity -= GRAVITY;
  }
  velocity.y += player->y_velocity;

  velocity = mul(velocity, delta);
  while (t_total < 1) {
    t = detectCollision(player, velocity, 1 - t_total, &collision_axis);
    player->position = add(player->position, mul(velocity, t - 0.01));
    t_total += t;
    if (t_total < 1) {
      *at(&velocity, collision_axis) = 0;
      if (collision_axis == 1) player->y_velocity = 0;
    }
  }

  updateTargetBlock(player_num);
  if ((cont->trigger & A_BUTTON) && player->target_present) {
    placeBlock(player_num, player->build_offset_x + player->target_x,
      player->build_offset_y + player->target_y, player->build_offset_z + player->target_z);
  }
  if ((cont->trigger & B_BUTTON) && player->target_present) {
    breakBlock(player->target_x, player->target_y, player->target_z);
  }
}

void updatePlayers() {
  OSTime time;
  float delta;
  u16 down_pressed, up_pressed, act_pressed;
  u8 i;

  nuContDataGetEx(&cont_data[0], 0);
  nuContDataGetEx(&cont_data[1], 1);
  time = osGetTime();
  delta = OS_CYCLES_TO_USEC(time - last_time) * 60 / 1000000.f;
  last_time = time;

  if (current_screen != GAME) {
    down_pressed = (cont_data[0].button & D_CBUTTONS) || cont_data[0].stick_y < -50;
    up_pressed = (cont_data[0].button & U_CBUTTONS) || cont_data[0].stick_y > 50;
    act_pressed = cont_data[0].button & (START_BUTTON | A_BUTTON | B_BUTTON);
    if (down_pressed && !down_held) menuDown();
    if (up_pressed && !up_held) menuUp();
    if (act_pressed && !act_held) menuAct();
    down_held = down_pressed;
    up_held = up_pressed;
    act_held = act_pressed;
    return;
  }

  if (!players[1].active && (cont_data[1].trigger & START_BUTTON)) {
    activatePlayerTwo();
  }
  for (i = 0; i < active_player_count; i++) {
    updatePlayer(i, delta);
  }

  if (saving_available && (cont_data[0].trigger & (U_JPAD | D_JPAD | L_JPAD | R_JPAD))) {
    saveGame();
    save_message_cooldown = 60;
  }
}
