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
#define FOUR_PLAYER_FOV_RATIO ((float) (SCREEN_WD / 2) / (float) (SCREEN_HT / 2))
#define NUM_CULL_LINES 4
#define ALL_ACCEPT ((1 << NUM_CULL_LINES) - 1)
#define SOLO_MAX_VISIBLE_COLUMNS 128
#define COOP_MAX_VISIBLE_COLUMNS 24
#define FOUR_PLAYER_MAX_VISIBLE_COLUMNS 8
#define THIRD_PERSON_DISTANCE 176.f
#define THIRD_PERSON_HEIGHT 24.f
#define THIRD_PERSON_SAMPLES 12
#define THIRD_PERSON_AVATAR_MIN_DISTANCE 40.f
#define LOADING_PREVIEW_FRAMES 150
#define LOADING_ORBIT_RADIUS 5600.f
#define LOADING_CAMERA_HEIGHT 3850.f
#define LOADING_CAMERA_BOB 280.f
#define LOADING_WORLD_CENTER (MAX_X * BLOCK_SIZE / 2.f)

typedef struct {
  float a;
  float b;
  float c;
} Line2D;

enum CameraViewMode {
  CAMERA_VIEW_SOLO,
  CAMERA_VIEW_TWO_PLAYER,
  CAMERA_VIEW_FOUR_PLAYER
};

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
static Player loading_camera;
static u16 loading_preview_frame;
static u8 loading_preview_active;

static u8 cameraPointSolid(Vector3 position) {
  int x = floor(position.x / BLOCK_SIZE);
  int y = floor(position.y / BLOCK_SIZE);
  int z = floor(position.z / BLOCK_SIZE);

  if (x < 0 || z < 0 || x >= MAX_X || z >= MAX_Z || y < 0) {
    return TRUE;
  }
  return y < MAX_Y &&
    BLOCK_IS_SOLID(blocks[x * MAX_Y * MAX_Z + y * MAX_Z + z]);
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
      FOV_Y, FOV_RATIO, 10, 14000, 1.0);
    guPerspective(&projection_matrix[frame][CAMERA_VIEW_TWO_PLAYER], &perspective_norm[frame][CAMERA_VIEW_TWO_PLAYER],
      FOV_Y_COOP, COOP_FOV_RATIO, 10, 8000, 1.0);
    guPerspective(&projection_matrix[frame][CAMERA_VIEW_FOUR_PLAYER], &perspective_norm[frame][CAMERA_VIEW_FOUR_PLAYER],
      FOV_Y_COOP, FOUR_PLAYER_FOV_RATIO, 10, 8000, 1.0);
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

static void updateVisibleColumnsFor(u8 player_num, Player *player,
    Vector3 camera_position, u8 view_mode) {
  u8 cx, cz, i;
  u8 s1, s2, s3, s4;
  u8 visible_count = 0;
  u8 farthest_x = 0, farthest_z = 0;
  float dx, dz, distance, farthest_distance;
  u8 multiplayer = view_mode != CAMERA_VIEW_SOLO;
  u8 max_visible_columns = view_mode == CAMERA_VIEW_SOLO ?
    SOLO_MAX_VISIBLE_COLUMNS : (view_mode == CAMERA_VIEW_FOUR_PLAYER ?
    FOUR_PLAYER_MAX_VISIBLE_COLUMNS : COOP_MAX_VISIBLE_COLUMNS);
  float fov_y = multiplayer ? FOV_Y_COOP : FOV_Y;
  float fov_x = fov_y * (view_mode == CAMERA_VIEW_FOUR_PLAYER ?
    FOUR_PLAYER_FOV_RATIO : (multiplayer ? COOP_FOV_RATIO : FOV_RATIO));

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

      /* A radius limit prevents multiplayer cameras from turning co-op into
         a worst-case multi-RSP transform. The world is fourteen chunks wide. */
      dx = cx * CHUNK_SIZE + CHUNK_SIZE / 2 -
        camera_position.x / BLOCK_SIZE;
      dz = cz * CHUNK_SIZE + CHUNK_SIZE / 2 -
        camera_position.z / BLOCK_SIZE;
      if (multiplayer && dx * dx + dz * dz > 1600) {
        visible_columns[player_num][cx * CHUNKS_Z + cz] = FALSE;
      }
      if (visible_columns[player_num][cx * CHUNKS_Z + cz]) visible_count++;
    }
  }

  /* Display submission is bounded even for the loading orbit: 128 columns
     x 11 texture lists stays below the 3,072-command solo list, while the
     multiplayer caps keep all viewports within the same budget. */
  while (visible_count > max_visible_columns) {
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

void updateVisibleColumns(u8 player_num) {
  u8 view_mode = active_player_count >= 3 ? CAMERA_VIEW_FOUR_PLAYER :
    (active_player_count == 2 ? CAMERA_VIEW_TWO_PLAYER : CAMERA_VIEW_SOLO);
  updateVisibleColumnsFor(player_num, &players[player_num],
    playerCameraPosition(player_num), view_mode);
}

static void updateCameraMatricesFor(u8 player_num, Player *player,
    Vector3 camera_position, u8 show_avatar) {
  Vector3 camera_offset = add(camera_position, mul(player->position, -1.f));

  third_person_avatar_visible[player_num] =
    show_avatar && player->camera_mode == CAMERA_THIRD_PERSON &&
    dot(camera_offset, camera_offset) >=
      THIRD_PERSON_AVATAR_MIN_DISTANCE * THIRD_PERSON_AVATAR_MIN_DISTANCE;
  guTranslate(&cam_translate[dl_no][player_num], -camera_position.x,
    -camera_position.y, -camera_position.z);
  guRotateRPY(&cam_rotate[dl_no][player_num], 0.0, -player->yaw, 0.0);
  guRotateRPY(&cam_rotate2[dl_no][player_num], -player->pitch, 0.0, 0.0);
}

void updateCameraMatrices(u8 player_num) {
  updateCameraMatricesFor(player_num, &players[player_num],
    playerCameraPosition(player_num), TRUE);
}

void beginLoadingPreview() {
  loading_preview_frame = 0;
  loading_preview_active = TRUE;
}

void updateLoadingCamera() {
  float angle = loading_preview_frame * .9f;
  float low_orbit = sinf(angle * .5f * M_DTOR);
  Vector3 camera_position;

  /* The orbit looks inward across the centre of the 112x112-block map. Its
     radius and height keep the complete landscape in a 60-degree view
     without using a separate, low-detail world model. */
  loading_camera.position.x = LOADING_WORLD_CENTER +
    sinf(angle * M_DTOR) * LOADING_ORBIT_RADIUS;
  loading_camera.position.z = LOADING_WORLD_CENTER +
    cosf(angle * M_DTOR) * LOADING_ORBIT_RADIUS;
  loading_camera.position.y = LOADING_CAMERA_HEIGHT +
    low_orbit * LOADING_CAMERA_BOB;
  loading_camera.yaw = angle;
  /* A steeper look direction lifts the landscape in the loading composition
     and keeps it clear of the status deck at the bottom of the screen. */
  loading_camera.pitch = -30.f;
  loading_camera.camera_mode = CAMERA_FIRST_PERSON;
  camera_position = loading_camera.position;

  updateVisibleColumnsFor(0, &loading_camera, camera_position, CAMERA_VIEW_SOLO);
  updateCameraMatricesFor(0, &loading_camera, camera_position, FALSE);
  loading_preview_frame++;
}

u8 loadingPreviewFinished() {
  if (loading_preview_frame >= LOADING_PREVIEW_FRAMES) {
    loading_preview_active = FALSE;
    return TRUE;
  }
  return FALSE;
}

u8 loadingPreviewProgress() {
  if (loading_preview_frame >= LOADING_PREVIEW_FRAMES) {
    return 100;
  }
  return loading_preview_frame * 100 / LOADING_PREVIEW_FRAMES;
}

void loadCameraMatrices(u8 player_num) {
  u8 projection = CAMERA_VIEW_SOLO;
  if (!loading_preview_active) {
    projection = active_player_count >= 3 ? CAMERA_VIEW_FOUR_PLAYER :
      (active_player_count == 2 ? CAMERA_VIEW_TWO_PLAYER : CAMERA_VIEW_SOLO);
  }
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
