#include <nusys.h>
#include "camera.h"
#include "blocks.h"
#include "player.h"
#include "math.h"
#include "graphics.h"
#include "geometry.h"
#include "menu.h"
#include "details.h"

#define FOV_Y 60
#define FOV_Y_COOP 40
/* Wider than the game's own lens would be, deliberately: the scenic camera
   sits low, and a long lens flattened the terrain into a texture swatch. */
#define FOV_Y_LOADING 48
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
/* The scenic orbit runs on wall-clock seconds, not on rendered frames.  This
   was a per-frame constant tuned when the preview drew around 20 fps, so every
   scheduling win since then spun the camera up by the same factor.  Seconds
   keep the carousel looking identical whatever the frame rate does next. */
#define MENU_ORBIT_DEGREES_PER_SECOND 5.6f
/* One rendered frame can be arbitrarily long -- a mesh build behind the menu,
   or the gap either side of a world load -- and the orbit must not teleport
   across it. */
#define LOADING_MAX_FRAME_SECONDS .25f
#define LOADING_ORBIT_RADIUS 2000.f
/* The world is 32 blocks tall, so 2450 units put the camera above every peak
   the generator can produce and the terrain read as a flat map.  1650 is
   inside the terrain's own height band: hills and trees now stand up in it. */
#define LOADING_CAMERA_HEIGHT 1650.f
#define LOADING_CAMERA_BOB 120.f
#define LOADING_WORLD_CENTER (MAX_X * BLOCK_SIZE / 2.f)
/* World units: deliberately below one pixel at normal interaction distance,
   but enough lateral travel for a little motion parallax while at rest. */
#define IDLE_SWAY_SIDE 0.50f
#define IDLE_SWAY_FORWARD 0.12f
#define IDLE_SWAY_VERTICAL 0.25f
#define IDLE_SWAY_DEGREES_PER_FRAME 1.25f
#define IDLE_SWAY_FADE_IN 36.f
#define IDLE_SWAY_FADE_OUT 5.f

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
/* See camera.h: the visible slots as a compact list for the terrain pass.
   Rebuilt alongside the array every time visibility is computed. */
u16 visible_slot_list[MAX_PLAYERS][VISIBLE_SLOT_LIST_MAX];
u16 visible_slot_count[MAX_PLAYERS];
u8 third_person_avatar_visible[MAX_PLAYERS];

/* Runtime copy of the solo cap, the other half of the Z + C-left LOD and
   visibility preset chord: distance-trimming a few more of the outermost,
   nearly fully fogged columns is the cheapest RSP saving on the table, and
   how far it can go before the horizon suffers is a CRT judgement. */
u16 solo_max_visible_columns = SOLO_MAX_VISIBLE_COLUMNS;

/* How many visible columns actually reach the fog band, per viewer.  At the
   parked fog_start nothing does, and the terrain pass uses the zero to
   skip the two-cycle far pass outright. */
u16 visible_far_count[MAX_PLAYERS];

/* Squared block distance to the nearest frustum cell with no geometry
   behind it -- not resident, or resident and not yet meshed -- which is
   where this viewer sees the void.  VISIBLE_HOLE_NONE when the whole view
   is meshed.  The auto fog reads this to sit just past the frontier. */
float visible_hole_sq[MAX_PLAYERS] = {
  VISIBLE_HOLE_NONE, VISIBLE_HOLE_NONE, VISIBLE_HOLE_NONE, VISIBLE_HOLE_NONE
};

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
static float loading_preview_elapsed;
static OSTime loading_preview_last_time;
static float idle_sway_phase[MAX_PLAYERS];
static float idle_sway_amount[MAX_PLAYERS];

void updateCameraIdleSway(u8 player_num, float delta, u8 standing_still) {
  float *amount;

  if (player_num >= MAX_PLAYERS) {
    return;
  }

  idle_sway_phase[player_num] += delta * IDLE_SWAY_DEGREES_PER_FRAME;
  if (idle_sway_phase[player_num] >= 360.f) {
    idle_sway_phase[player_num] -= 360.f;
  }

  amount = &idle_sway_amount[player_num];
  if (standing_still) {
    *amount = min(1.f, *amount + delta / IDLE_SWAY_FADE_IN);
  } else {
    /* Leaving the resting pose should feel immediate when the player starts
       walking, without a visible single-frame pop. */
    *amount = max(0.f, *amount - delta / IDLE_SWAY_FADE_OUT);
  }
}

static Vector3 idleSwayOffset(u8 player_num, Player *player) {
  float phase = idle_sway_phase[player_num] * M_DTOR;
  float amount = idle_sway_amount[player_num];
  Vector3 right = {1, 0, 0};
  Vector3 forward = {0, 0, -1};
  Vector3 offset;

  right = rotateY(right, -player->yaw);
  forward = rotateY(forward, -player->yaw);
  offset = add(mul(right, sinf(phase) * IDLE_SWAY_SIDE * amount),
    mul(forward, sinf(phase * .47f + .8f) * IDLE_SWAY_FORWARD * amount));
  offset.y = sinf(phase * 1.71f + .35f) * IDLE_SWAY_VERTICAL * amount;
  return offset;
}

static u8 cameraPointSolid(Vector3 position) {
  int x = floor(position.x / BLOCK_SIZE);
  int y = floor(position.y / BLOCK_SIZE);
  int z = floor(position.z / BLOCK_SIZE);

  if (y < 0) {
    return TRUE;
  }
  return worldCellSolid(x, y, z);
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
    return add(camera, idleSwayOffset(player_num, player));
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
  u16 far_count = 0;
  float nearest_hole_sq = VISIBLE_HOLE_NONE;
  float dx, dz;
  u8 multiplayer = view_mode == CAMERA_VIEW_TWO_PLAYER ||
    view_mode == CAMERA_VIEW_FOUR_PLAYER;
  u16 max_visible_columns = view_mode == CAMERA_VIEW_LOADING ?
    LOADING_MAX_VISIBLE_COLUMNS : (view_mode == CAMERA_VIEW_SOLO ?
    solo_max_visible_columns : (view_mode == CAMERA_VIEW_FOUR_PLAYER ?
    FOUR_PLAYER_MAX_VISIBLE_COLUMNS : COOP_MAX_VISIBLE_COLUMNS));
  float fov_y = view_mode == CAMERA_VIEW_LOADING ? FOV_Y_LOADING :
    (multiplayer ? FOV_Y_COOP : FOV_Y);
  float fov_x = fov_y * (view_mode == CAMERA_VIEW_FOUR_PLAYER ?
    FOUR_PLAYER_FOV_RATIO : (multiplayer ? COOP_FOV_RATIO : FOV_RATIO));
  /*
   * The block distance where fog begins, inverted from the screen-depth
   * mapping documented over fog_start in graphics.c:
   *
   *     v(d) = 1000 * far * (64d - near) / (64d * (far - near))
   *
   * with the near plane at 10 and the far plane per projection.  A column
   * whose farthest point is still closer than this renders identically with
   * fog off, and saying so here lets the terrain pass draw it in
   * single-cycle mode at twice the RDP pixel rate.  Half a column diagonal
   * (~6 blocks) converts the centre distance the loop below measures into
   * that farthest point.  Derived from the live fog_start so the Z + D-pad
   * fog tuning moves the near/far boundary with it.
   */
  float fog_far_plane = multiplayer ? 8000.f : 14000.f;
  float fog_begin_denom = fog_far_plane -
    (float) fog_start * (fog_far_plane - 10.f) * .001f;
  float fog_near_blocks = fog_begin_denom > 0.f ?
    (fog_far_plane * 10.f) / (64.f * fog_begin_denom) - 6.f : 0.f;
  float fog_near_sq = fog_near_blocks > 0.f ?
    fog_near_blocks * fog_near_blocks : 0.f;

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
     extent below never visits must not keep a stale mark and send a
     point-visibility reader to a column that is no longer resident. */
  for (slot = 0; slot < WINDOW_SLOTS; slot++) {
    visible_columns[player_num][slot] = FALSE;
  }
  visible_slot_count[player_num] = 0;

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
      /* A frustum cell the mesh cannot fill yet is where this view meets
         the void; note the nearest so the auto fog can cover it.  Tested
         before the residency override, which is itself one of the two ways
         a cell can be a hole. */
      if (visible && (!windowColumnResident(world_cx, world_cz) ||
          graphicsColumnMissingMesh(world_cx, world_cz))) {
        float hole_sq = dx * dx + dz * dz;

        if (hole_sq < nearest_hole_sq) {
          nearest_hole_sq = hole_sq;
        }
      }
      /* An unbound slot has no mesh behind it. */
      if (!windowColumnResident(world_cx, world_cz)) {
        visible = FALSE;
      }
      if (visible && dx * dx + dz * dz > fog_near_sq) {
        visible_columns[player_num][WINDOW_SLOT(world_cx, world_cz)] =
          COLUMN_VISIBLE_FAR;
        far_count++;
      } else {
        visible_columns[player_num][WINDOW_SLOT(world_cx, world_cz)] =
          visible ? COLUMN_VISIBLE_NEAR : FALSE;
      }
      if (visible) {
        /* Cannot overflow: the scan visits CULL_SPAN * CULL_SPAN cells --
           the list's exact size -- and the span is narrower than the window,
           so no two cells share a slot. */
        visible_slot_list[player_num][visible_slot_count[player_num]++] =
          (u16) WINDOW_SLOT(world_cx, world_cz);
        visible_count++;
      }
    }
  }

  /* Shed the farthest columns down to the cap.  The wider ring can pass a
     few hundred columns through the frustum, and re-scanning for the strict
     farthest per removal made the shed quadratic in the overage -- up to
     ~320 removals against ~440 candidates.  A distance histogram finds the
     cut in one pass instead: every band strictly beyond the boundary is
     shed whole, and the boundary band sheds just enough of its members, in
     list order, to reach the cap.  A band spans 256 of squared-block
     distance -- under two blocks of range where shedding happens -- so
     "farthest first" now holds to within a band where it held exactly, for
     three linear passes instead of the quadratic rescan. */
  if (visible_count > max_visible_columns) {
    static u8 trim_bucket[VISIBLE_SLOT_LIST_MAX];
    u16 bucket_count[256];
    u16 overage = visible_count - max_visible_columns;
    u16 above = 0;
    u16 from_boundary;
    u16 i;
    int boundary;

    for (i = 0; i < 256; i++) {
      bucket_count[i] = 0;
    }
    for (i = 0; i < visible_slot_count[player_num]; i++) {
      u32 band;

      slot = visible_slot_list[player_num][i];
      dx = windowSlotChunkX(slot) * CHUNK_SIZE + CHUNK_SIZE / 2 -
        camera_position.x / BLOCK_SIZE;
      dz = windowSlotChunkZ(slot) * CHUNK_SIZE + CHUNK_SIZE / 2 -
        camera_position.z / BLOCK_SIZE;
      band = (u32) (dx * dx + dz * dz) >> 8;
      if (band > 255) {
        band = 255;
      }
      trim_bucket[i] = (u8) band;
      bucket_count[band]++;
    }
    for (boundary = 255; boundary > 0; boundary--) {
      if (above + bucket_count[boundary] >= overage) {
        break;
      }
      above += bucket_count[boundary];
    }
    from_boundary = overage - above;
    for (i = 0; i < visible_slot_count[player_num]; i++) {
      if (trim_bucket[i] < boundary) {
        continue;
      }
      if (trim_bucket[i] == boundary) {
        if (from_boundary == 0) {
          continue;
        }
        from_boundary--;
      }
      slot = visible_slot_list[player_num][i];
      if (visible_columns[player_num][slot] == COLUMN_VISIBLE_FAR) {
        far_count--;
      }
      visible_columns[player_num][slot] = FALSE;
      visible_count--;
    }

    /* The shed marked slots invisible behind the list's back; drop them. */
    {
      u16 kept = 0;

      for (i = 0; i < visible_slot_count[player_num]; i++) {
        u16 s = visible_slot_list[player_num][i];

        if (visible_columns[player_num][s]) {
          visible_slot_list[player_num][kept++] = s;
        }
      }
      visible_slot_count[player_num] = kept;
    }
  }
  visible_far_count[player_num] = far_count;
  visible_hole_sq[player_num] = nearest_hole_sq;
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

void resetPreviewOrbit() {
  loading_preview_elapsed = 0.f;
  /* The world build that precedes the reveal renders nothing, so its whole
     duration would otherwise arrive as the first frame's delta. */
  loading_preview_last_time = osGetTime();
}

/* Seconds since the previous rendered preview frame. */
static float loadingPreviewDelta() {
  OSTime now = osGetTime();
  float delta;

  if (loading_preview_last_time == 0) {
    loading_preview_last_time = now;
    return 0.f;
  }
  delta = (u32) OS_CYCLES_TO_USEC(now - loading_preview_last_time) / 1000000.f;
  loading_preview_last_time = now;
  if (delta > LOADING_MAX_FRAME_SECONDS) {
    delta = LOADING_MAX_FRAME_SECONDS;
  }
  return delta;
}

void updateLoadingCamera() {
  float angle;
  float low_orbit;
  Vector3 camera_position;

  /* The cards may stay open for minutes, so the orbit is a calm continuous
     carousel.  A steady rate rather than an eased one: easing from a crawl
     made the terrain read as a tick-tock slideshow on displays where the
     render cadence is not perfectly even. */
  loading_preview_elapsed += loadingPreviewDelta();
  angle = loading_preview_elapsed * MENU_ORBIT_DEGREES_PER_SECOND;
  low_orbit = sinf(angle * .5f * M_DTOR);

  /* Keep the orbit over the terrain and close in.  The wide lens sees more
     of the world per column, so the whole view still fits inside the
     96-column cinematic budget and the horizon never relies on
     distance-based column removal. */
  loading_camera.position.x = LOADING_WORLD_CENTER +
    sinf(angle * M_DTOR) * LOADING_ORBIT_RADIUS;
  loading_camera.position.z = LOADING_WORLD_CENTER +
    cosf(angle * M_DTOR) * LOADING_ORBIT_RADIUS;
  loading_camera.position.y = LOADING_CAMERA_HEIGHT +
    low_orbit * LOADING_CAMERA_BOB;
  loading_camera.yaw = angle;
  /* Aim into the near/middle terrain rather than across the whole map.  The
     steeper the tilt, the nearer the ground the top of the frame lands on, so
     the streamed world's far edge stays out of shot. */
  /* Player pitch is normalized to 0..360, and the column culler relies on
     that convention when choosing the conservative world-height edge.  -40
     renders the same view, but makes culling treat this downward camera as an
     upward one and clips columns before they leave the screen. */
  loading_camera.pitch = 320.f;
  loading_camera.camera_mode = CAMERA_FIRST_PERSON;
  camera_position = loading_camera.position;

  /* Keep the visible set in lockstep with the camera. The loading-specific
     column cap is already the substantial performance saving here; caching
     this step made the slow orbit visibly jump at each refresh. */
  updateVisibleColumnsFor(0, &loading_camera, camera_position,
    CAMERA_VIEW_LOADING);
  updateCameraMatricesFor(0, &loading_camera, camera_position, FALSE);
}

static u8 cameraProjectionMode() {
  u8 projection = CAMERA_VIEW_SOLO;

  /* The menu can launch its already-loaded preview directly into gameplay.
     Screen state is therefore the projection authority: a sticky preview
     flag would leave the 34-degree cinematic lens active in the game. */
  if (screenShowsPreview(current_screen)) {
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
