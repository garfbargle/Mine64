#include <nusys.h>
#include "camera.h"
#include "blocks.h"
#include "player.h"
#include "math.h"
#include "graphics.h"
#include "geometry.h"
#include "menu.h"

#define FOV_Y 60
#define FOV_Y_COOP 40
#define FOV_Y_LOADING 34
#define FOV_RATIO ((float) SCREEN_WD / (float) SCREEN_HT)
#define COOP_FOV_RATIO ((float) SCREEN_WD / (float) (SCREEN_HT / 2))
#define FOUR_PLAYER_FOV_RATIO ((float) (SCREEN_WD / 2) / (float) (SCREEN_HT / 2))
#define NUM_CULL_LINES 4
#define ALL_ACCEPT ((1 << NUM_CULL_LINES) - 1)
/* Tuned on hardware against the W row: 160 was measurably choppy with the
   wide ring, 120 keeps movement smooth while still doubling the old view. */
#define SOLO_MAX_VISIBLE_COLUMNS 120
#define COOP_MAX_VISIBLE_COLUMNS 24
#define FOUR_PLAYER_MAX_VISIBLE_COLUMNS 8
#define LOADING_MAX_VISIBLE_COLUMNS 96
#define THIRD_PERSON_DISTANCE 128.f
#define THIRD_PERSON_HEIGHT 40.f
#define THIRD_PERSON_SHOULDER_OFFSET 18.f
#define THIRD_PERSON_SAMPLES 12
#define THIRD_PERSON_AVATAR_MIN_DISTANCE 40.f
#define LOADING_PREVIEW_FRAMES 180
#define LOADING_ORBIT_DEGREES_PER_FRAME .62f
#define MENU_ORBIT_DEGREES_PER_FRAME .28f
#define LOADING_ORBIT_RADIUS 2240.f
#define LOADING_CAMERA_HEIGHT 2450.f
#define LOADING_CAMERA_BOB 120.f
#define LOADING_WORLD_CENTER (MAX_X * BLOCK_SIZE / 2.f)

typedef struct {
  float a;
  float b;
  float c;
} Line2D;

enum CameraViewMode {
  CAMERA_VIEW_SOLO,
  CAMERA_VIEW_TWO_PLAYER,
  CAMERA_VIEW_FOUR_PLAYER,
  CAMERA_VIEW_LOADING
};

/* Indexed by window slot, matching the per-column tables in graphics.c. */
u8 visible_columns[MAX_PLAYERS][WINDOW_SLOTS];
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
/*
 * Culling follows the player rather than a fixed world.  The span is the
 * residency ring's 15 columns, plus one for the far corners of the last
 * column.  Indexed relative to the span's base, not by absolute chunk.
 */
/* Matches the decorated/mesh ring: everything that can have geometry is
   inside this span. */
#define CULL_RADIUS STREAM_TREE_RADIUS
#define CULL_SPAN (2 * CULL_RADIUS + 1)
static u8 point_sides[CULL_SPAN + 1][CULL_SPAN + 1];
static Player loading_camera;
static u32 loading_preview_frame;

static u8 cameraPointSolid(Vector3 position) {
  int x = floor(position.x / BLOCK_SIZE);
  int y = floor(position.y / BLOCK_SIZE);
  int z = floor(position.z / BLOCK_SIZE);

  if (y < 0) {
    return TRUE;
  }
  return y < MAX_Y &&
    BLOCK_IS_SOLID(blockGet(x, y, z));
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
  Vector3 origin;
  Vector3 desired;
  Vector3 offset;
  Vector3 forward = {0, 0, -1};
  Vector3 right = {1, 0, 0};
  u8 sample;

  camera.y += player->camera_y_offset;
  if (player->camera_mode != CAMERA_THIRD_PERSON) {
    return camera;
  }

  origin = camera;
  forward = rotateX(forward, player->pitch);
  forward = rotateY(forward, -player->yaw);
  right = rotateY(right, -player->yaw);
  desired = add(origin, mul(forward, -THIRD_PERSON_DISTANCE));
  desired = add(desired, mul(right, THIRD_PERSON_SHOULDER_OFFSET));
  desired.y += THIRD_PERSON_HEIGHT;
  offset = add(desired, mul(origin, -1.f));

  /* Pull the camera toward the player before it can enter a wall.  Twelve
     samples are plenty across a sub-three-block arm and cost far less than a
     second collision system. */
  for (sample = 1; sample <= THIRD_PERSON_SAMPLES; sample++) {
    Vector3 candidate = add(origin,
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
    guPerspective(&projection_matrix[frame][CAMERA_VIEW_LOADING],
      &perspective_norm[frame][CAMERA_VIEW_LOADING], FOV_Y_LOADING,
      FOV_RATIO, 10, 14000, 1.0);
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
  int cx, cz;
  u8 i;
  int base_cx, base_cz;
  u8 s1, s2, s3, s4;
  u16 slot;
  /* A full window is 256 columns, which does not fit the u8 this used to be. */
  u16 visible_count = 0;
  float dx, dz, farthest_distance;
  u8 multiplayer = view_mode == CAMERA_VIEW_TWO_PLAYER ||
    view_mode == CAMERA_VIEW_FOUR_PLAYER;
  u8 max_visible_columns = view_mode == CAMERA_VIEW_LOADING ?
    LOADING_MAX_VISIBLE_COLUMNS : (view_mode == CAMERA_VIEW_SOLO ?
    SOLO_MAX_VISIBLE_COLUMNS : (view_mode == CAMERA_VIEW_FOUR_PLAYER ?
    FOUR_PLAYER_MAX_VISIBLE_COLUMNS : COOP_MAX_VISIBLE_COLUMNS));
  float fov_y = view_mode == CAMERA_VIEW_LOADING ? FOV_Y_LOADING :
    (multiplayer ? FOV_Y_COOP : FOV_Y);
  float fov_x = fov_y * (view_mode == CAMERA_VIEW_FOUR_PLAYER ?
    FOUR_PLAYER_FOV_RATIO : (multiplayer ? COOP_FOV_RATIO : FOV_RATIO));

  makeHorizontalCullLine(&cull_lines[0], -1, player, camera_position, fov_x);
  makeHorizontalCullLine(&cull_lines[1], 1, player, camera_position, fov_x);
  makeVerticalCullLine(&cull_lines[2], -1, player, camera_position, fov_y);
  makeVerticalCullLine(&cull_lines[3], 1, player, camera_position, fov_y);

  /* The scenic orbit views one fixed world from outside it, so it keeps
     covering that world; gameplay follows the player. */
  if (view_mode == CAMERA_VIEW_LOADING) {
    base_cx = CHUNKS_X / 2 - CULL_RADIUS;
    base_cz = CHUNKS_Z / 2 - CULL_RADIUS;
  } else {
    base_cx = floor(player->position.x / (BLOCK_SIZE * CHUNK_SIZE)) -
      CULL_RADIUS;
    base_cz = floor(player->position.z / (BLOCK_SIZE * CHUNK_SIZE)) -
      CULL_RADIUS;
  }

  for (cx = 0; cx <= CULL_SPAN; cx++) {
    for (cz = 0; cz <= CULL_SPAN; cz++) {
      int corner_x = (base_cx + cx) * CHUNK_SIZE;
      int corner_z = (base_cz + cz) * CHUNK_SIZE;
      point_sides[cx][cz] = 0;
      for (i = 0; i < NUM_CULL_LINES; i++) {
        if (corner_x * cull_lines[i].a + corner_z * cull_lines[i].b +
            cull_lines[i].c <= 0) {
          point_sides[cx][cz] += 1 << i;
        }
      }
    }
  }

  /* Visibility is recorded per window slot, so clear every slot first: one the
     extent below never visits must not keep a stale mark and send the draw
     loop to a column that is no longer resident. */
  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    visible_columns[player_num][slot] = FALSE;
  }

  for (cx = 0; cx < CULL_SPAN; cx++) {
    for (cz = 0; cz < CULL_SPAN; cz++) {
      int world_cx = base_cx + cx;
      int world_cz = base_cz + cz;
      u8 visible;

      s1 = point_sides[cx][cz];
      s2 = point_sides[cx + 1][cz];
      s3 = point_sides[cx][cz + 1];
      s4 = point_sides[cx + 1][cz + 1];
      visible = (s1 | s2 | s3 | s4) == ALL_ACCEPT;

      /* A radius limit prevents multiplayer cameras from turning co-op into
         a worst-case multi-RSP transform. */
      dx = world_cx * CHUNK_SIZE + CHUNK_SIZE / 2 -
        camera_position.x / BLOCK_SIZE;
      dz = world_cz * CHUNK_SIZE + CHUNK_SIZE / 2 -
        camera_position.z / BLOCK_SIZE;
      if (multiplayer && dx * dx + dz * dz > 1600) {
        visible = FALSE;
      }
      /* An unbound slot has no mesh behind it. */
      if (!windowColumnResident(world_cx, world_cz)) {
        visible = FALSE;
      }
      visible_columns[player_num][WINDOW_SLOT(world_cx, world_cz)] = visible;
      if (visible) visible_count++;
    }
  }

  /* Shed the farthest columns down to the cap.  The wider ring can pass a
     few hundred columns through the frustum, so the trim works from a
     compact list gathered once rather than rescanning every window slot per
     removal -- that rescan was quadratic in the overage. */
  if (visible_count > max_visible_columns) {
    static u16 trim_slots[CULL_SPAN * CULL_SPAN];
    static float trim_distance[CULL_SPAN * CULL_SPAN];
    u16 trim_count = 0;
    u16 i;

    for (slot = 0; slot < WINDOW_SLOTS && trim_count < CULL_SPAN * CULL_SPAN;
        slot++) {
      if (visible_columns[player_num][slot]) {
        dx = windowSlotChunkX(slot) * CHUNK_SIZE + CHUNK_SIZE / 2 -
          camera_position.x / BLOCK_SIZE;
        dz = windowSlotChunkZ(slot) * CHUNK_SIZE + CHUNK_SIZE / 2 -
          camera_position.z / BLOCK_SIZE;
        trim_slots[trim_count] = slot;
        trim_distance[trim_count] = dx * dx + dz * dz;
        trim_count++;
      }
    }
    while (visible_count > max_visible_columns) {
      u16 farthest = 0;

      farthest_distance = -1;
      for (i = 0; i < trim_count; i++) {
        if (trim_distance[i] > farthest_distance) {
          farthest_distance = trim_distance[i];
          farthest = i;
        }
      }
      visible_columns[player_num][trim_slots[farthest]] = FALSE;
      trim_distance[farthest] = -2;
      visible_count--;
    }
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
  /* Origin-relative, like every world-space Mtx: the camera and the terrain
     must subtract the same origin or the world jumps when it rebases. */
  guTranslate(&cam_translate[dl_no][player_num],
    -(camera_position.x - render_origin_units_x),
    -camera_position.y,
    -(camera_position.z - render_origin_units_z));
  guRotateRPY(&cam_rotate[dl_no][player_num], 0.0, -player->yaw, 0.0);
  guRotateRPY(&cam_rotate2[dl_no][player_num], -player->pitch, 0.0, 0.0);
}

void updateCameraMatrices(u8 player_num) {
  updateCameraMatricesFor(player_num, &players[player_num],
    playerCameraPosition(player_num), TRUE);
}

void beginLoadingPreview() {
  loading_preview_frame = 0;
}

void updateLoadingCamera() {
  float angle;
  float low_orbit;
  Vector3 camera_position;

  if (current_screen == LOADING_PREVIEW) {
    /* A steady camera gives the eye enough motion to blend individual N64
       frames. Easing from a crawl made the terrain read as a tick-tock
       slideshow on displays where the render cadence is not perfectly even. */
    angle = loading_preview_frame * LOADING_ORBIT_DEGREES_PER_FRAME;
  } else {
    /* The world picker may remain open for minutes, so keep its orbit a
       calm, continuous carousel rather than reusing the loading reveal. */
    angle = loading_preview_frame * MENU_ORBIT_DEGREES_PER_FRAME;
  }
  low_orbit = sinf(angle * .5f * M_DTOR);

  /* Keep the orbit over the terrain and use a narrower lens below. Together
     they fit the complete view inside the 96-column cinematic budget, so the
     horizon never relies on distance-based column removal. */
  loading_camera.position.x = LOADING_WORLD_CENTER +
    sinf(angle * M_DTOR) * LOADING_ORBIT_RADIUS;
  loading_camera.position.z = LOADING_WORLD_CENTER +
    cosf(angle * M_DTOR) * LOADING_ORBIT_RADIUS;
  loading_camera.position.y = LOADING_CAMERA_HEIGHT +
    low_orbit * LOADING_CAMERA_BOB;
  loading_camera.yaw = angle;
  /* Aim into the near/middle terrain rather than across the whole map. The
     48-degree downward view keeps the far edge above the frame while the
     closer orbit preserves roughly the previous terrain scale. */
  /* Player pitch is normalized to 0..360, and the column culler relies on
     that convention when choosing the conservative world-height edge.  -48
     renders the same view, but makes culling treat this downward camera as an
     upward one and clips columns before they leave the screen. */
  loading_camera.pitch = 312.f;
  loading_camera.camera_mode = CAMERA_FIRST_PERSON;
  camera_position = loading_camera.position;

  /* Keep the visible set in lockstep with the camera. The loading-specific
     column cap is already the substantial performance saving here; caching
     this step made the slow orbit visibly jump at each refresh. */
  updateVisibleColumnsFor(0, &loading_camera, camera_position,
    CAMERA_VIEW_LOADING);
  updateCameraMatricesFor(0, &loading_camera, camera_position, FALSE);
  loading_preview_frame++;
}

u8 loadingPreviewFinished() {
  return loading_preview_frame >= LOADING_PREVIEW_FRAMES;
}

u8 loadingPreviewProgress() {
  if (loading_preview_frame >= LOADING_PREVIEW_FRAMES) {
    return 100;
  }
  return loading_preview_frame * 100 / LOADING_PREVIEW_FRAMES;
}

static u8 cameraProjectionMode() {
  u8 projection = CAMERA_VIEW_SOLO;

  /* The menu can launch its already-loaded preview directly into gameplay.
     Screen state is therefore the projection authority: a sticky preview
     flag would leave the 34-degree cinematic lens active in the game. */
  if (current_screen == LOADING_PREVIEW || current_screen == MENU ||
      current_screen == WORLD_NAMING) {
    projection = CAMERA_VIEW_LOADING;
  } else {
    projection = active_player_count >= 3 ? CAMERA_VIEW_FOUR_PLAYER :
      (active_player_count == 2 ? CAMERA_VIEW_TWO_PLAYER : CAMERA_VIEW_SOLO);
  }
  return projection;
}

void loadCameraProjection() {
  u8 projection = cameraProjectionMode();

  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&projection_matrix[dl_no][projection]),
    G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
  gSPPerspNormalize(dlp++, perspective_norm[dl_no][projection]);
}

void loadCameraMatrices(u8 player_num) {
  loadCameraProjection();
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&cam_rotate2[dl_no][player_num]),
    G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&cam_rotate[dl_no][player_num]),
    G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&cam_translate[dl_no][player_num]),
    G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);
}
