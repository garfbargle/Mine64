#include <nusys.h>
#include "camera.h"
#include "blocks.h"
#include "player.h"
#include "math.h"
#include "graphics.h"
#include "geometry.h"

#define FOV_Y 60
#define FOV_Y_COOP 40
#define FOV_RATIO ((float) SCREEN_WD / (float) SCREEN_HT)
#define COOP_FOV_RATIO ((float) SCREEN_WD / (float) (SCREEN_HT / 2))
#define NUM_CULL_LINES 4
#define ALL_ACCEPT ((1 << NUM_CULL_LINES) - 1)
#define COOP_MAX_VISIBLE_COLUMNS 24
#define THIRD_PERSON_DISTANCE 176.f
#define THIRD_PERSON_HEIGHT 24.f
#define THIRD_PERSON_SAMPLES 12
#define THIRD_PERSON_AVATAR_MIN_DISTANCE 40.f

typedef struct {
  float a;
  float b;
  float c;
} Line2D;

u8 visible_columns[MAX_PLAYERS][CHUNKS_X * CHUNKS_Z];
u8 third_person_avatar_visible[MAX_PLAYERS];

/*
 * Every matrix referenced by an RSP display list must remain unchanged until
 * that task has completed.  The CPU is allowed to prepare the next frame
 * while the previous one is still in flight, so these matrices follow the
 * same double-buffer index as the world display lists.
 */
static Mtx projection_matrix[NUM_DISPLAY_LISTS][MAX_PLAYERS];
static u16 perspective_norm[NUM_DISPLAY_LISTS][MAX_PLAYERS];
static Mtx cam_rotate2[NUM_DISPLAY_LISTS][MAX_PLAYERS];
static Mtx cam_rotate[NUM_DISPLAY_LISTS][MAX_PLAYERS];
static Mtx cam_translate[NUM_DISPLAY_LISTS][MAX_PLAYERS];
static Line2D cull_lines[NUM_CULL_LINES];
static u8 point_sides[CHUNKS_X + 1][CHUNKS_Z + 1];

static u8 cameraPointSolid(Vector3 position) {
  int x = floor(position.x / BLOCK_SIZE);
  int y = floor(position.y / BLOCK_SIZE);
  int z = floor(position.z / BLOCK_SIZE);

  if (x < 0 || z < 0 || x >= MAX_X || z >= MAX_Z || y < 0) {
    return TRUE;
  }
  return y < MAX_Y &&
    blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z] != AIR;
}

static u8 cameraSpaceClear(Vector3 position) {
  Vector3 probe = position;

  if (cameraPointSolid(position)) {
    return FALSE;
  }
  probe.x -= 8;
  if (cameraPointSolid(probe)) return FALSE;
  probe.x += 16;
  if (cameraPointSolid(probe)) return FALSE;
  probe = position;
  probe.y -= 8;
  if (cameraPointSolid(probe)) return FALSE;
  probe.y += 16;
  if (cameraPointSolid(probe)) return FALSE;
  probe = position;
  probe.z -= 8;
  if (cameraPointSolid(probe)) return FALSE;
  probe.z += 16;
  return !cameraPointSolid(probe);
}

Vector3 playerCameraPosition(u8 player_num) {
  Player *player = &players[player_num];
  Vector3 camera = player->position;
  Vector3 desired;
  Vector3 offset;
  Vector3 forward = {0, 0, -1};
  u8 sample;

  if (player->camera_mode != CAMERA_THIRD_PERSON) {
    return camera;
  }

  forward = rotateX(forward, player->pitch);
  forward = rotateY(forward, -player->yaw);
  desired = add(player->position, mul(forward, -THIRD_PERSON_DISTANCE));
  desired.y += THIRD_PERSON_HEIGHT;
  offset = add(desired, mul(player->position, -1.f));

  /* Pull the camera toward the player before it can enter a wall.  Twelve
     samples are plenty across a sub-three-block arm and cost far less than a
     second collision system. */
  for (sample = 1; sample <= THIRD_PERSON_SAMPLES; sample++) {
    Vector3 candidate = add(player->position,
      mul(offset, sample / (float) THIRD_PERSON_SAMPLES));
    if (!cameraSpaceClear(candidate)) {
      break;
    }
    camera = candidate;
  }
  return camera;
}

void initCamera() {
  u8 frame;

  for (frame = 0; frame < NUM_DISPLAY_LISTS; frame++) {
    guPerspective(&projection_matrix[frame][0], &perspective_norm[frame][0],
      FOV_Y, FOV_RATIO, 10, 8000, 1.0);
    guPerspective(&projection_matrix[frame][1], &perspective_norm[frame][1],
      FOV_Y_COOP, COOP_FOV_RATIO, 10, 8000, 1.0);
  }
}

static void makeCullLine(Line2D *line, Vector3 normal, Player *player,
    Vector3 camera_position) {
  Vector3 cam_b = {camera_position.x / BLOCK_SIZE,
    camera_position.y / BLOCK_SIZE, camera_position.z / BLOCK_SIZE};
  line->a = normal.x;
  line->b = normal.z;
  line->c = -dot(normal, cam_b);
  if (player->pitch < 180) line->c += MAX_Y * normal.y;
}

static void makeHorizontalCullLine(Line2D *line, float side, Player *player,
    Vector3 camera_position, float fov_x) {
  Vector3 normal = {side, 0, 0};
  normal = rotateY(normal, side * fov_x / 2);
  normal = rotateX(normal, player->pitch);
  normal = rotateY(normal, -player->yaw);
  makeCullLine(line, normal, player, camera_position);
}

static void makeVerticalCullLine(Line2D *line, float side, Player *player,
    Vector3 camera_position, float fov_y) {
  Vector3 normal = {0, side, 0};
  if (player->pitch < 90 - fov_y / 2 || player->pitch > 270 + fov_y / 2) {
    normal = rotateX(normal, side * 90);
  } else {
    normal = rotateX(normal, player->pitch + side * fov_y / 2);
  }
  normal = rotateY(normal, -player->yaw);
  makeCullLine(line, normal, player, camera_position);
}

void updateVisibleColumns(u8 player_num) {
  u8 cx, cz, i;
  u8 s1, s2, s3, s4;
  u8 visible_count = 0;
  u8 farthest_x = 0, farthest_z = 0;
  float dx, dz, distance, farthest_distance;
  float fov_y = active_player_count == 2 ? FOV_Y_COOP : FOV_Y;
  float fov_x = fov_y * (active_player_count == 2 ? COOP_FOV_RATIO : FOV_RATIO);
  Player *player = &players[player_num];
  Vector3 camera_position = playerCameraPosition(player_num);

  makeHorizontalCullLine(&cull_lines[0], -1, player, camera_position, fov_x);
  makeHorizontalCullLine(&cull_lines[1], 1, player, camera_position, fov_x);
  makeVerticalCullLine(&cull_lines[2], -1, player, camera_position, fov_y);
  makeVerticalCullLine(&cull_lines[3], 1, player, camera_position, fov_y);

  for (cx = 0; cx <= CHUNKS_X; cx++) {
    for (cz = 0; cz <= CHUNKS_Z; cz++) {
      point_sides[cx][cz] = 0;
      for (i = 0; i < NUM_CULL_LINES; i++) {
        if (cx * CHUNK_SIZE * cull_lines[i].a + cz * CHUNK_SIZE * cull_lines[i].b + cull_lines[i].c <= 0) {
          point_sides[cx][cz] += 1 << i;
        }
      }
    }
  }

  for (cx = 0; cx < CHUNKS_X; cx++) {
    for (cz = 0; cz < CHUNKS_Z; cz++) {
      s1 = point_sides[cx][cz];
      s2 = point_sides[cx + 1][cz];
      s3 = point_sides[cx][cz + 1];
      s4 = point_sides[cx + 1][cz + 1];
      visible_columns[player_num][cx * CHUNKS_Z + cz] = (s1 | s2 | s3 | s4) == ALL_ACCEPT;

      /* A radius limit prevents the second camera from turning co-op into a
         worst-case double RSP transform.  The world is only eight chunks wide. */
      dx = cx * CHUNK_SIZE + CHUNK_SIZE / 2 -
        camera_position.x / BLOCK_SIZE;
      dz = cz * CHUNK_SIZE + CHUNK_SIZE / 2 -
        camera_position.z / BLOCK_SIZE;
      if (active_player_count == 2 && dx * dx + dz * dz > 1600) {
        visible_columns[player_num][cx * CHUNKS_Z + cz] = FALSE;
      }
      if (visible_columns[player_num][cx * CHUNKS_Z + cz]) visible_count++;
    }
  }

  /* The world display list is deliberately bounded as well as the view
     distance: 24 columns x 11 textures x 2 players is comfortably within
     the 1,024-command per-frame list on a stock N64. */
  while (active_player_count == 2 && visible_count > COOP_MAX_VISIBLE_COLUMNS) {
    farthest_distance = -1;
    for (cx = 0; cx < CHUNKS_X; cx++) {
      for (cz = 0; cz < CHUNKS_Z; cz++) {
        if (visible_columns[player_num][cx * CHUNKS_Z + cz]) {
          dx = cx * CHUNK_SIZE + CHUNK_SIZE / 2 -
            camera_position.x / BLOCK_SIZE;
          dz = cz * CHUNK_SIZE + CHUNK_SIZE / 2 -
            camera_position.z / BLOCK_SIZE;
          distance = dx * dx + dz * dz;
          if (distance > farthest_distance) {
            farthest_distance = distance;
            farthest_x = cx;
            farthest_z = cz;
          }
        }
      }
    }
    visible_columns[player_num][farthest_x * CHUNKS_Z + farthest_z] = FALSE;
    visible_count--;
  }
}

void updateCameraMatrices(u8 player_num) {
  Player *player = &players[player_num];
  Vector3 camera_position = playerCameraPosition(player_num);
  Vector3 camera_offset = add(camera_position, mul(player->position, -1.f));

  third_person_avatar_visible[player_num] =
    player->camera_mode == CAMERA_THIRD_PERSON &&
    dot(camera_offset, camera_offset) >=
      THIRD_PERSON_AVATAR_MIN_DISTANCE * THIRD_PERSON_AVATAR_MIN_DISTANCE;
  guTranslate(&cam_translate[dl_no][player_num], -camera_position.x,
    -camera_position.y, -camera_position.z);
  guRotateRPY(&cam_rotate[dl_no][player_num], 0.0, -player->yaw, 0.0);
  guRotateRPY(&cam_rotate2[dl_no][player_num], -player->pitch, 0.0, 0.0);
}

void loadCameraMatrices(u8 player_num) {
  u8 projection = active_player_count == 2;
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&projection_matrix[dl_no][projection]),
    G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
  gSPPerspNormalize(dlp++, perspective_norm[dl_no][projection]);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&cam_rotate2[dl_no][player_num]),
    G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&cam_rotate[dl_no][player_num]),
    G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&cam_translate[dl_no][player_num]),
    G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);
}
