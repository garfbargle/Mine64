#include <nusys.h>
#include <assert.h>
#include "graphics.h"
#include "geometry.h"
#include "items.h"
#include "mobs.h"
#include "trees.h"
#include "camera.h"
#include "player.h"
#include "menu.h"
#include "quads.h"
#include "cube.h"
#include "textures.h"
#include "day_cycle.h"

#define CROSSHAIR_SIZE 10
#define HOTBAR_SLOT_COUNT INVENTORY_COLUMNS
#define HOTBAR_SLOT_SIZE 22
#define HOTBAR_ICON_SIZE 16
#define HOTBAR_MARGIN 7
#define HELD_PREVIEW_SIZE 32
#define INVENTORY_SLOT_SIZE 18
#define INVENTORY_ICON_SIZE 14
#define CELESTIAL_DISTANCE_SOLO 11000.f
#define CELESTIAL_DISTANCE_COOP 7000.f
#define SUN_SIZE 430.f
#define MOON_SIZE 360.f
#define DROPPED_ITEM_RENDER_DISTANCE (BLOCK_SIZE * 36.f)
#define PLAYER_RENDER_DISTANCE (BLOCK_SIZE * 64.f)
#define SHEEP_RENDER_DISTANCE (BLOCK_SIZE * 30.f)
#define MAX_VISIBLE_SHEEP 3

Gfx *dlp;
u32 dl_no = 0;
Gfx frame_display_lists[NUM_DISPLAY_LISTS][FRAME_DISPLAY_LIST_SIZE];

/* Keep a complete, immutable mesh arena on screen while a replacement is
 * compacted incrementally into the other arena.  This eliminates the old
 * gameplay-time RSP wait and full-world rebuild when append-only updates ran
 * low on space. */
#define NUM_COLUMN_ARENAS 2
#define MESH_REBUILD_BUDGET 1
#define MESH_COMPACTION_RESERVE (DISPLAY_LIST_SIZE / 10)
static Gfx column_display_lists[NUM_COLUMN_ARENAS][DISPLAY_LIST_SIZE];
Gfx *column_dlp;
static Gfx *column_starts[NUM_COLUMN_ARENAS][NUM_TEXTURES]
  [CHUNKS_X * CHUNKS_Z];
static Gfx *column_arena_ends[NUM_COLUMN_ARENAS];
static u8 active_column_arena;
static u8 column_build_arena;
static u8 column_display_list_full;
static u8 dirty_columns[CHUNKS_X * CHUNKS_Z];
static u8 dirty_column_cursor;
static u8 compacting_columns;
static u16 compaction_column_cursor;

static Gfx empty_column_display_list[] = {
  gsSPEndDisplayList()
};

#define BLOCKS_PER_CHUNK (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)

static Mtx c_models[NUM_BLOCKS / BLOCKS_PER_CHUNK];
static Mtx b_models[CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE];

/* Water keeps the normal block footprint but leaves a small lip below an
 * adjacent shore.  Only top faces need a special mesh, so this costs 4 KiB
 * rather than duplicating every terrain quad. */
#define WATER_SURFACE_HEIGHT (BLOCK_SIZE - BLOCK_SIZE / 8)
#define WATER_TOP_QUAD_ADDR(width, height) \
  (water_top_verts + (((width) * CHUNK_SIZE + (height)) * 4))
static Vtx water_top_verts[CHUNK_SIZE * CHUNK_SIZE * 4]
  __attribute__((aligned(8)));

/* Lighting and sky geometry are both frame-buffered just like the camera:
   the RSP can still be drawing the previous frame when the CPU prepares the
   next one. */
static Lights1 world_lights[NUM_DISPLAY_LISTS][MAX_PLAYERS];
static Lights0 preview_lights[NUM_DISPLAY_LISTS];
static Vtx celestial_verts[NUM_DISPLAY_LISTS][MAX_PLAYERS][8];

static Mtx dropped_item_translate[NUM_DISPLAY_LISTS][MAX_DROPPED_ITEMS];
static Mtx dropped_item_rotate[NUM_DISPLAY_LISTS][MAX_DROPPED_ITEMS];

#define FALLING_TREE_RENDER_SLOTS 4
#define FALLING_TREE_BOXES 6

/* A falling tree uses a few chunky volumes, not dozens of independently
   transformed cubes.  Individual cube drops appear after it lands. */
static Mtx falling_tree_translate[NUM_DISPLAY_LISTS][FALLING_TREE_RENDER_SLOTS];
static Mtx falling_tree_rotate[NUM_DISPLAY_LISTS][FALLING_TREE_RENDER_SLOTS];
static Mtx falling_tree_box_translate[NUM_DISPLAY_LISTS][FALLING_TREE_RENDER_SLOTS][FALLING_TREE_BOXES];
static Mtx falling_tree_box_scale[NUM_DISPLAY_LISTS][FALLING_TREE_RENDER_SLOTS][FALLING_TREE_BOXES];

#define ITEM_VERTEX(x, y, z, s, t) {x, y, z, 0, s, t, 255, 255, 255, 255}

static Vtx dropped_item_verts[] = {
  ITEM_VERTEX(-14, 14, 14, 0, 0), ITEM_VERTEX(14, 14, 14, 16 << 5, 0),
  ITEM_VERTEX(14, -14, 14, 16 << 5, 16 << 5), ITEM_VERTEX(-14, -14, 14, 0, 16 << 5),
  ITEM_VERTEX(14, 14, -14, 0, 0), ITEM_VERTEX(-14, 14, -14, 16 << 5, 0),
  ITEM_VERTEX(-14, -14, -14, 16 << 5, 16 << 5), ITEM_VERTEX(14, -14, -14, 0, 16 << 5)
};

static Gfx dropped_item_display_list[] = {
  gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0),
  gsSP2Triangles(4, 5, 6, 0, 4, 6, 7, 0),
  gsSP2Triangles(1, 4, 7, 0, 1, 7, 2, 0),
  gsSP2Triangles(5, 0, 3, 0, 5, 3, 6, 0),
  gsSP2Triangles(5, 4, 1, 0, 5, 1, 0, 0),
  gsSP2Triangles(3, 2, 7, 0, 3, 7, 6, 0),
  gsSPEndDisplayList()
};

/* Steve is deliberately built from a handful of shaded boxes instead of a
   texture atlas.  It is cheap enough to submit once in each co-op viewport,
   yet still reads as a character at the game's normal view distance. */
#define STEVE_HEAD 0
#define STEVE_BODY 1
#define STEVE_LEFT_ARM 2
#define STEVE_RIGHT_ARM 3
#define STEVE_LEFT_LEG 4
#define STEVE_RIGHT_LEG 5
#define STEVE_SWORD 6
#define STEVE_PART_COUNT 7

static Mtx steve_translate[NUM_DISPLAY_LISTS][MAX_PLAYERS][STEVE_PART_COUNT];
static Mtx steve_rotate[NUM_DISPLAY_LISTS][MAX_PLAYERS][STEVE_PART_COUNT];
static Mtx first_person_sword_translate[NUM_DISPLAY_LISTS][MAX_PLAYERS];
static Mtx first_person_sword_rotate[NUM_DISPLAY_LISTS][MAX_PLAYERS];

#define SHEEP_BODY 0
#define SHEEP_HEAD 1
#define SHEEP_FRONT_LEFT_LEG 2
#define SHEEP_FRONT_RIGHT_LEG 3
#define SHEEP_BACK_LEFT_LEG 4
#define SHEEP_BACK_RIGHT_LEG 5
#define SHEEP_PART_COUNT 6

static Mtx sheep_translate[NUM_DISPLAY_LISTS][MAX_SHEEP][SHEEP_PART_COUNT];
static Mtx sheep_rotate[NUM_DISPLAY_LISTS][MAX_SHEEP][SHEEP_PART_COUNT];

#define STEVE_VERTEX(x, y, z, r, g, b) {x, y, z, 0, 0, 0, r, g, b, 255}

/* Head, torso, arms, and legs.  Limbs start at y = 0 so their rotation has a
   convincing shoulder/hip pivot instead of spinning around their middles. */
static Vtx steve_head_verts[] = {
  STEVE_VERTEX(-16, 16, 16, 198, 137, 90), STEVE_VERTEX(16, 16, 16, 198, 137, 90),
  STEVE_VERTEX(16, -16, 16, 198, 137, 90), STEVE_VERTEX(-16, -16, 16, 198, 137, 90),
  STEVE_VERTEX(16, 16, -16, 198, 137, 90), STEVE_VERTEX(-16, 16, -16, 198, 137, 90),
  STEVE_VERTEX(-16, -16, -16, 198, 137, 90), STEVE_VERTEX(16, -16, -16, 198, 137, 90)
};

static Vtx steve_body_verts[] = {
  STEVE_VERTEX(-18, 22, 9, 54, 140, 190), STEVE_VERTEX(18, 22, 9, 54, 140, 190),
  STEVE_VERTEX(18, -22, 9, 54, 140, 190), STEVE_VERTEX(-18, -22, 9, 54, 140, 190),
  STEVE_VERTEX(18, 22, -9, 54, 140, 190), STEVE_VERTEX(-18, 22, -9, 54, 140, 190),
  STEVE_VERTEX(-18, -22, -9, 54, 140, 190), STEVE_VERTEX(18, -22, -9, 54, 140, 190)
};

static Vtx steve_arm_verts[] = {
  STEVE_VERTEX(-7, 0, 7, 198, 137, 90), STEVE_VERTEX(7, 0, 7, 198, 137, 90),
  STEVE_VERTEX(7, -44, 7, 198, 137, 90), STEVE_VERTEX(-7, -44, 7, 198, 137, 90),
  STEVE_VERTEX(7, 0, -7, 198, 137, 90), STEVE_VERTEX(-7, 0, -7, 198, 137, 90),
  STEVE_VERTEX(-7, -44, -7, 198, 137, 90), STEVE_VERTEX(7, -44, -7, 198, 137, 90)
};

static Vtx steve_leg_verts[] = {
  STEVE_VERTEX(-8, 0, 8, 55, 70, 150), STEVE_VERTEX(8, 0, 8, 55, 70, 150),
  STEVE_VERTEX(8, -44, 8, 55, 70, 150), STEVE_VERTEX(-8, -44, 8, 55, 70, 150),
  STEVE_VERTEX(8, 0, -8, 55, 70, 150), STEVE_VERTEX(-8, 0, -8, 55, 70, 150),
  STEVE_VERTEX(-8, -44, -8, 55, 70, 150), STEVE_VERTEX(8, -44, -8, 55, 70, 150)
};

/* The sheep is all chunky shaded geometry, matching Steve's inexpensive
   renderer.  Its fleece therefore needs no new character atlas or UV work. */
static Vtx sheep_body_verts[] = {
  STEVE_VERTEX(-27, 17, 18, 236, 234, 218), STEVE_VERTEX(27, 17, 18, 236, 234, 218),
  STEVE_VERTEX(27, -17, 18, 236, 234, 218), STEVE_VERTEX(-27, -17, 18, 236, 234, 218),
  STEVE_VERTEX(27, 17, -18, 204, 204, 194), STEVE_VERTEX(-27, 17, -18, 204, 204, 194),
  STEVE_VERTEX(-27, -17, -18, 204, 204, 194), STEVE_VERTEX(27, -17, -18, 204, 204, 194)
};

static Vtx sheep_head_verts[] = {
  STEVE_VERTEX(-15, 14, 14, 190, 188, 176), STEVE_VERTEX(15, 14, 14, 190, 188, 176),
  STEVE_VERTEX(15, -14, 14, 190, 188, 176), STEVE_VERTEX(-15, -14, 14, 190, 188, 176),
  STEVE_VERTEX(15, 14, -14, 150, 148, 140), STEVE_VERTEX(-15, 14, -14, 150, 148, 140),
  STEVE_VERTEX(-15, -14, -14, 150, 148, 140), STEVE_VERTEX(15, -14, -14, 150, 148, 140)
};

static Vtx sheep_leg_verts[] = {
  STEVE_VERTEX(-6, 18, 6, 104, 96, 82), STEVE_VERTEX(6, 18, 6, 104, 96, 82),
  STEVE_VERTEX(6, -18, 6, 104, 96, 82), STEVE_VERTEX(-6, -18, 6, 104, 96, 82),
  STEVE_VERTEX(6, 18, -6, 76, 70, 62), STEVE_VERTEX(-6, 18, -6, 76, 70, 62),
  STEVE_VERTEX(-6, -18, -6, 76, 70, 62), STEVE_VERTEX(6, -18, -6, 76, 70, 62)
};

/* Two blue eye quads on the local -Z face make it obvious where Steve is
   looking, even without a character texture. */
static Vtx steve_eye_verts[] = {
  STEVE_VERTEX(-11, 8, -17, 55, 125, 210), STEVE_VERTEX(-5, 8, -17, 55, 125, 210),
  STEVE_VERTEX(-5, 2, -17, 55, 125, 210), STEVE_VERTEX(-11, 2, -17, 55, 125, 210),
  STEVE_VERTEX(5, 8, -17, 55, 125, 210), STEVE_VERTEX(11, 8, -17, 55, 125, 210),
  STEVE_VERTEX(11, 2, -17, 55, 125, 210), STEVE_VERTEX(5, 2, -17, 55, 125, 210)
};

/* The blade points down from the hand pivot.  Keeping it as two shaded boxes
   gives every equipped sword a crisp silhouette without adding a texture or
   a costly animated skeleton. */
static Vtx steve_sword_blade_verts[] = {
  STEVE_VERTEX(-4, 4, 3, 205, 205, 188), STEVE_VERTEX(4, 4, 3, 205, 205, 188),
  STEVE_VERTEX(4, -42, 3, 205, 205, 188), STEVE_VERTEX(-4, -42, 3, 205, 205, 188),
  STEVE_VERTEX(4, 4, -3, 165, 165, 154), STEVE_VERTEX(-4, 4, -3, 165, 165, 154),
  STEVE_VERTEX(-4, -42, -3, 165, 165, 154), STEVE_VERTEX(4, -42, -3, 165, 165, 154)
};

static Vtx steve_sword_hilt_verts[] = {
  STEVE_VERTEX(-12, 7, 5, 142, 83, 38), STEVE_VERTEX(12, 7, 5, 142, 83, 38),
  STEVE_VERTEX(12, 1, 5, 142, 83, 38), STEVE_VERTEX(-12, 1, 5, 142, 83, 38),
  STEVE_VERTEX(12, 7, -5, 104, 58, 27), STEVE_VERTEX(-12, 7, -5, 104, 58, 27),
  STEVE_VERTEX(-12, 1, -5, 104, 58, 27), STEVE_VERTEX(12, 1, -5, 104, 58, 27)
};

/* First person needs the forearm as well as the blade; otherwise a floating
   sword loses the "held" feeling that makes an attack easy to read.  Its hand
   ends at the same origin as the sword hilt. */
static Vtx first_person_arm_verts[] = {
  STEVE_VERTEX(-7, 48, 7, 198, 137, 90), STEVE_VERTEX(7, 48, 7, 198, 137, 90),
  STEVE_VERTEX(7, 0, 7, 198, 137, 90), STEVE_VERTEX(-7, 0, 7, 198, 137, 90),
  STEVE_VERTEX(7, 48, -7, 198, 137, 90), STEVE_VERTEX(-7, 48, -7, 198, 137, 90),
  STEVE_VERTEX(-7, 0, -7, 198, 137, 90), STEVE_VERTEX(7, 0, -7, 198, 137, 90)
};

static Gfx steve_box_display_list[] = {
  gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0),
  gsSP2Triangles(4, 5, 6, 0, 4, 6, 7, 0),
  gsSP2Triangles(1, 4, 7, 0, 1, 7, 2, 0),
  gsSP2Triangles(5, 0, 3, 0, 5, 3, 6, 0),
  gsSP2Triangles(5, 4, 1, 0, 5, 1, 0, 0),
  gsSP2Triangles(3, 2, 7, 0, 3, 7, 6, 0),
  gsSPEndDisplayList()
};

static Gfx steve_eyes_display_list[] = {
  gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0),
  gsSP2Triangles(4, 5, 6, 0, 4, 6, 7, 0),
  gsSPEndDisplayList()
};

static Texture *loaded_texture;

void loadTexture(Texture *texture);

static Vp full_viewport = {
    SCREEN_WD*2, SCREEN_HT*2, G_MAXZ/2, 0,
    SCREEN_WD*2, SCREEN_HT*2, G_MAXZ/2, 0,
};

/* N64 viewport coordinates are 10.2 fixed point.  These two viewports fill
   the same framebuffer as solo play, so split-screen does not double fill-rate. */
static Vp two_player_viewports[2] = {
  {SCREEN_WD*2, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD*2, SCREEN_HT, G_MAXZ/2, 0},
  {SCREEN_WD*2, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD*2, SCREEN_HT*3, G_MAXZ/2, 0}
};

/* Four players use 160x120 quadrants.  vscale/vtrans use the N64's 10.2
   viewport representation, hence the doubled dimensions and centres. */
static Vp four_player_viewports[MAX_PLAYERS] = {
  {SCREEN_WD, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD, SCREEN_HT, G_MAXZ/2, 0},
  {SCREEN_WD, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD*3, SCREEN_HT, G_MAXZ/2, 0},
  {SCREEN_WD, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD, SCREEN_HT*3, G_MAXZ/2, 0},
  {SCREEN_WD, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD*3, SCREEN_HT*3, G_MAXZ/2, 0}
};

static u8 usesFourPlayerLayout(void) {
  return active_player_count >= 3;
}

static u32 playerViewportWidth(void) {
  return usesFourPlayerLayout() ? SCREEN_WD / 2 : SCREEN_WD;
}

static u32 playerViewportHeight(void) {
  return active_player_count > 1 ? SCREEN_HT / 2 : SCREEN_HT;
}

static u32 playerViewportX(u8 player_num) {
  return usesFourPlayerLayout() ? (player_num & 1) * (SCREEN_WD / 2) : 0;
}

static u32 playerViewportY(u8 player_num) {
  return active_player_count > 1 ? (usesFourPlayerLayout() ? player_num / 2 :
    player_num) * (SCREEN_HT / 2) : 0;
}

static void selectPlayerViewport(u8 player_num) {
  u32 x = playerViewportX(player_num);
  u32 y = playerViewportY(player_num);

  if (usesFourPlayerLayout()) {
    gSPViewport(dlp++, &four_player_viewports[player_num]);
  } else {
    gSPViewport(dlp++, &two_player_viewports[player_num]);
  }
  gDPSetScissor(dlp++, G_SC_NON_INTERLACE, x, y,
    x + playerViewportWidth(), y + playerViewportHeight());
}

static void setLightColor(Light *light, SkyColor color) {
  light->l.col[0] = color.r;
  light->l.col[1] = color.g;
  light->l.col[2] = color.b;
  light->l.colc[0] = color.r;
  light->l.colc[1] = color.g;
  light->l.colc[2] = color.b;
}

static void setAmbientColor(Ambient *ambient, SkyColor color) {
  ambient->l.col[0] = color.r;
  ambient->l.col[1] = color.g;
  ambient->l.col[2] = color.b;
  ambient->l.colc[0] = color.r;
  ambient->l.colc[1] = color.g;
  ambient->l.colc[2] = color.b;
}

static void setWorldLight(u8 player_num) {
  Lights1 *lights = &world_lights[dl_no][player_num];
  Vector3 direction = dayCycleLightDirection();

  setAmbientColor(&lights->a, dayCycleAmbientLight());
  setLightColor(&lights->l[0], dayCycleDirectLight());
  lights->l[0].l.dir[0] = direction.x * 127;
  lights->l[0].l.dir[1] = direction.y * 127;
  lights->l[0].l.dir[2] = direction.z * 127;
}

static void setPreviewLight() {
  Lights0 *lights = &preview_lights[dl_no];
  SkyColor ambient = {215, 215, 215};
  SkyColor no_direct_light = {0, 0, 0};

  setAmbientColor(&lights->a, ambient);
  setLightColor(&lights->l[0], no_direct_light);
}

static void setCelestialVertex(Vtx *vertex, Vector3 point, u16 s, u16 t) {
  vertex->v.ob[0] = point.x;
  vertex->v.ob[1] = point.y;
  vertex->v.ob[2] = point.z;
  vertex->v.flag = 0;
  vertex->v.tc[0] = s;
  vertex->v.tc[1] = t;
  vertex->v.cn[0] = 255;
  vertex->v.cn[1] = 255;
  vertex->v.cn[2] = 255;
  vertex->v.cn[3] = 255;
}

static void makeCelestialQuad(Vtx *vertices, Vector3 center, Vector3 right,
    Vector3 up, float size) {
  Vector3 horizontal = mul(right, size);
  Vector3 vertical = mul(up, size);
  Vector3 top_left = add(add(center, vertical), mul(horizontal, -1.f));
  Vector3 top_right = add(add(center, vertical), horizontal);
  Vector3 bottom_right = add(add(center, mul(vertical, -1.f)), horizontal);
  Vector3 bottom_left = add(add(center, mul(vertical, -1.f)), mul(horizontal, -1.f));

  setCelestialVertex(&vertices[0], top_left, 0, 0);
  setCelestialVertex(&vertices[1], top_right, 16 << 5, 0);
  setCelestialVertex(&vertices[2], bottom_right, 16 << 5, 16 << 5);
  setCelestialVertex(&vertices[3], bottom_left, 0, 16 << 5);
}

static Texture *moonTexture() {
  static Texture *moon_textures[] = {
    &moon_0_texture, &moon_1_texture, &moon_2_texture, &moon_3_texture,
    &moon_4_texture, &moon_5_texture, &moon_6_texture, &moon_7_texture
  };
  return moon_textures[dayCycleMoonPhase()];
}

static void drawCelestialBodies(u8 player_num) {
  Player *player = &players[player_num];
  Vector3 sun = dayCycleSunDirection();
  Vector3 moon = mul(sun, -1.f);
  Vector3 camera = playerCameraPosition(player_num);
  Vector3 right = rotateY((Vector3) {1, 0, 0}, -player->yaw);
  Vector3 up = rotateY(rotateX((Vector3) {0, 1, 0}, player->pitch), -player->yaw);
  Vtx *vertices = celestial_verts[dl_no][player_num];
  float distance = active_player_count > 1 ? CELESTIAL_DISTANCE_COOP :
    CELESTIAL_DISTANCE_SOLO;

  gSPClearGeometryMode(dlp++, G_CULL_BACK | G_LIGHTING);
  gDPSetCombineMode(dlp++, G_CC_MODULATERGB, G_CC_MODULATERGB);
  gDPSetRenderMode(dlp++, G_RM_AA_ZB_TEX_EDGE, G_RM_AA_ZB_TEX_EDGE2);
  gDPSetAlphaCompare(dlp++, G_AC_THRESHOLD);

  if (sun.y >= 0) {
    makeCelestialQuad(vertices, add(camera, mul(sun, distance)),
      right, up, SUN_SIZE);
    loadTexture(&sun_texture);
    gSPVertex(dlp++, vertices, 4, 0);
    gSP1Quadrangle(dlp++, 0, 1, 2, 3, 0);
  }
  if (moon.y >= 0) {
    makeCelestialQuad(vertices + 4, add(camera, mul(moon, distance)),
      right, up, MOON_SIZE);
    loadTexture(moonTexture());
    gSPVertex(dlp++, vertices + 4, 4, 0);
    gSP1Quadrangle(dlp++, 0, 1, 2, 3, 0);
  }

  gDPSetAlphaCompare(dlp++, G_AC_NONE);
  loaded_texture = NULL;
}

static Gfx setup_display_list[] = {
  gsSPSegment(0, 0x0),
  gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),
  gsDPSetScissor(G_SC_NON_INTERLACE, 0,0, SCREEN_WD,SCREEN_HT),
  gsSPEndDisplayList()
};

static Gfx draw_setup_display_list[] = {
  gsDPSetCycleType(G_CYC_1CYCLE),
  gsDPSetRenderMode(G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2),
  gsSPClearGeometryMode(0xFFFFFFFF),
  gsSPSetGeometryMode(G_ZBUFFER | G_CULL_BACK | G_SHADE | G_SHADING_SMOOTH),
  
  gsSPTexture(0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON),
  gsDPSetTexturePersp(G_TP_PERSP),
  gsDPSetCombineMode(G_CC_MODULATERGB, G_CC_MODULATERGB),
  gsDPSetTextureLUT(G_TT_RGBA16),
  gsSPEndDisplayList()
};

static Gfx wireframe_setup_display_list[] = {
  gsDPSetCycleType(G_CYC_1CYCLE),
  gsDPSetRenderMode(G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2),
  gsSPClearGeometryMode(0xFFFFFFFF),
  gsSPSetGeometryMode(G_ZBUFFER | G_CULL_BACK | G_SHADE | G_SHADING_SMOOTH),
  gsSPEndDisplayList()
};

static Gfx wireframe_display_list[] = {
  gsSPLine3D(0, 1, 0),
  gsSPLine3D(1, 2, 0),
  gsSPLine3D(2, 3, 0),
  gsSPLine3D(3, 0, 0),
  
  gsSPLine3D(4, 5, 0),
  gsSPLine3D(5, 6, 0),
  gsSPLine3D(6, 7, 0),
  gsSPLine3D(7, 4, 0),
  
  gsSPLine3D(0, 5, 0),
  gsSPLine3D(1, 4, 0),
  gsSPLine3D(2, 7, 0),
  gsSPLine3D(3, 6, 0),

  gsSPEndDisplayList()
};

/* Three stages of increasingly dense fractures are drawn over a log while it
   is being punched.  They use the same block-local coordinate space as the
   targeting outline, so no world mesh rebuild is needed every frame. */
#define CRACK_VERTEX(x, y, z) {x, y, z, 0, 0, 0, 66, 38, 20, 255}

static Vtx breaking_crack_verts[] = {
  /* Front face */
  CRACK_VERTEX(4, 65, 66), CRACK_VERTEX(22, 43, 66),
  CRACK_VERTEX(13, 19, 66), CRACK_VERTEX(34, -1, 66),
  CRACK_VERTEX(61, 65, 66), CRACK_VERTEX(45, 42, 66),
  CRACK_VERTEX(54, 21, 66),
  /* Right face */
  CRACK_VERTEX(66, 60, 6), CRACK_VERTEX(66, 40, 24),
  CRACK_VERTEX(66, 18, 13), CRACK_VERTEX(66, -1, 34),
  /* Top face */
  CRACK_VERTEX(7, 66, 9), CRACK_VERTEX(28, 66, 29),
  CRACK_VERTEX(17, 66, 51), CRACK_VERTEX(39, 66, 66),
  /* Left and back faces keep the effect visible from every direction. */
  CRACK_VERTEX(-2, 59, 60), CRACK_VERTEX(-2, 40, 42),
  CRACK_VERTEX(-2, 18, 53), CRACK_VERTEX(-2, -1, 31),
  CRACK_VERTEX(60, 59, -2), CRACK_VERTEX(41, 39, -2),
  CRACK_VERTEX(53, 18, -2), CRACK_VERTEX(32, -1, -2)
};

static Gfx breaking_crack_stage_one[] = {
  gsSPLine3D(0, 1, 0),
  gsSPLine3D(1, 2, 0),
  gsSPLine3D(2, 3, 0),
  gsSPEndDisplayList()
};

static Gfx breaking_crack_stage_two[] = {
  gsSPLine3D(0, 1, 0), gsSPLine3D(1, 2, 0), gsSPLine3D(2, 3, 0),
  gsSPLine3D(4, 5, 0), gsSPLine3D(5, 6, 0), gsSPLine3D(6, 3, 0),
  gsSPLine3D(7, 8, 0), gsSPLine3D(8, 9, 0),
  gsSPEndDisplayList()
};

static Gfx breaking_crack_stage_three[] = {
  gsSPLine3D(0, 1, 0), gsSPLine3D(1, 2, 0), gsSPLine3D(2, 3, 0),
  gsSPLine3D(4, 5, 0), gsSPLine3D(5, 6, 0), gsSPLine3D(6, 3, 0),
  gsSPLine3D(7, 8, 0), gsSPLine3D(8, 9, 0), gsSPLine3D(9, 10, 0),
  gsSPLine3D(11, 12, 0), gsSPLine3D(12, 13, 0), gsSPLine3D(13, 14, 0),
  gsSPLine3D(15, 16, 0), gsSPLine3D(16, 17, 0), gsSPLine3D(17, 18, 0),
  gsSPLine3D(19, 20, 0), gsSPLine3D(20, 21, 0), gsSPLine3D(21, 22, 0),
  gsSPEndDisplayList()
};

static Gfx *breaking_crack_stages[] = {
  breaking_crack_stage_one,
  breaking_crack_stage_two,
  breaking_crack_stage_three
};

void clearBuffers(u16 bg_color) {
  gDPSetDepthImage(dlp++, OS_K0_TO_PHYSICAL(nuGfxZBuffer));
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetColorImage(dlp++, G_IM_FMT_RGBA, G_IM_SIZ_16b,SCREEN_WD,
		   OS_K0_TO_PHYSICAL(nuGfxZBuffer));
  gDPSetFillColor(dlp++,(GPACK_ZDZ(G_MAXFBZ,0) << 16 |
			       GPACK_ZDZ(G_MAXFBZ,0)));
  gDPFillRectangle(dlp++, 0, 0, SCREEN_WD-1, SCREEN_HT-1);
  gDPPipeSync(dlp++);
  
  gDPSetColorImage(dlp++, G_IM_FMT_RGBA, G_IM_SIZ_16b, SCREEN_WD,
		   osVirtualToPhysical(nuGfxCfb_ptr));
  gDPSetFillColor(dlp++, (bg_color << 16 | bg_color));
  gDPFillRectangle(dlp++, 0, 0, SCREEN_WD-1, SCREEN_HT-1);
  gDPPipeSync(dlp++);
}

void loadTexture(Texture *texture) {
  if (texture != loaded_texture) {
    loaded_texture = texture;
    gDPLoadTLUT_pal16(dlp++, 0, texture->pallet);
    gDPLoadTextureBlock_4b(dlp++, texture->color_indices, G_IM_FMT_CI,
                      16, 16, 0, G_TX_WRAP, loaded_texture == &grass_side_texture? G_TX_CLAMP : G_TX_WRAP, 
                      4, 4, G_TX_NOLOD, G_TX_NOLOD);
  }
}

static Gfx *columnArenaStart(void) {
  return column_display_lists[column_build_arena];
}

static u32 columnArenaFree(void) {
  return columnArenaStart() + DISPLAY_LIST_SIZE - column_dlp;
}

static void beginColumnArenaBuild(u8 arena) {
  column_build_arena = arena;
  column_dlp = column_display_lists[arena];
  column_display_list_full = FALSE;
}

void makeQuadDL(u16 chunk, u8 bx, u8 by, u8 bz, u8 width, u8 height,
    u8 face, u8 is_water) {
  u32 b = bx * CHUNK_SIZE * CHUNK_SIZE + by * CHUNK_SIZE + bz;

  /* Reserve one final command for the column's EndDisplayList.  A maximally
     fragmented player-built chunk can exceed the normal greedy-mesh budget;
     dropping only excess faces is far safer than overwriting adjacent RAM. */
  if (columnArenaFree() < 6) {
    column_display_list_full = TRUE;
    return;
  }
  gSPMatrix(column_dlp++,OS_K0_TO_PHYSICAL(c_models + chunk),
    G_MTX_MODELVIEW|G_MTX_LOAD|G_MTX_NOPUSH);
  gSPMatrix(column_dlp++,OS_K0_TO_PHYSICAL(b_models + b),
    G_MTX_MODELVIEW|G_MTX_MUL|G_MTX_NOPUSH);
  gSPVertex(column_dlp++, is_water && face == TOP ?
    WATER_TOP_QUAD_ADDR(width, height) : QUAD_ADDR(face, width, height), 4, 0);
  gSP1Quadrangle(column_dlp++, 3, 2, 1, 0, 0);
}

void makeQuadDLRST(u16 chunk, u8 br, u8 bs, u8 bt, u8 axes, u8 width,
    u8 height, u8 face, u8 is_water) {
  if (face == NONE) {
    return;
  }

  if (axes == ZXY) {
    makeQuadDL(chunk, bs, bt, br, width, height, face, is_water);
  } else if (axes == XZY) {
    makeQuadDL(chunk, br, bt, bs, width, height, face, is_water);
  } else if (axes == YXZ) {
    makeQuadDL(chunk, bs, br, bt, width, height, face, is_water);
  }
}

void makeChunkAxisDL(DualQuadList *axis_quads, u16 chunk, u8 axes, u8 face1, u8 face2, u8 block) {
  u8 br, i;
  DualQuadList *both_quads;
  for (br = 0; br < CHUNK_SIZE; br++) {
    both_quads = &(axis_quads)[br];
    for (i = 0; i < both_quads->n_front + both_quads->n_back; i++) {
      if (both_quads->quads[i].block == block) {
        makeQuadDLRST(chunk, br, both_quads->quads[i].bs, both_quads->quads[i].bt, axes,
          both_quads->quads[i].width, both_quads->quads[i].height,
          i < both_quads->n_front ? face1 : face2, block == WATER);
      }
    }
  }
}

void makeColumnDL(u8 cx, u8 cz, u8 texture) {
  u8 cy, i;
  u16 chunk;
  ChunkQuads *c_quads;
  FaceSpec *faces = textures[texture]->faces;

  if (column_display_list_full ||
      columnArenaFree() < 1) {
    column_display_list_full = TRUE;
    column_starts[column_build_arena][texture][cx * CHUNKS_Z + cz] =
      empty_column_display_list;
    return;
  }
  column_starts[column_build_arena][texture][cx * CHUNKS_Z + cz] = column_dlp;

  for (cy = 0; cy < CHUNKS_Y; cy++) {
    chunk = cx * CHUNKS_Y * CHUNKS_Z + cy * CHUNKS_Z + cz;
    c_quads = &column_quads[cy];

    for (i = 0; i < textures[texture]->n_faces; i++) {
      if (faces[i].sides) {
        makeChunkAxisDL(c_quads->z_quads, chunk, ZXY, FRONT, BACK, faces[i].block);
        makeChunkAxisDL(c_quads->x_quads, chunk, XZY, RIGHT, LEFT, faces[i].block);
      }

      if (faces[i].top || faces[i].bottom) {
        makeChunkAxisDL(c_quads->y_quads, chunk, YXZ, faces[i].top? TOP : NONE, faces[i].bottom? BOTTOM : NONE, faces[i].block);
      }
    }
  }

  gSPEndDisplayList(column_dlp++);
}

static void makeColumnDisplayLists(u8 cx, u8 cz) {
  u8 texture;

  makeColumnGeometry(cx, cz);
  for (texture = 0; texture < NUM_TEXTURES; texture++) {
    makeColumnDL(cx, cz, texture);
  }
}

void makeWorldDisplayLists() {
  u8 cx, cz;
  u16 column;

  /* This is a complete mesh rebuild, not an incremental block edit.  World
     previews can rebuild several slots back-to-back, so start from the
     beginning of the fixed command arena after the RSP is done with it. */
  nuGfxTaskAllEndWait();
  active_column_arena = 0;
  compacting_columns = FALSE;
  dirty_column_cursor = 0;
  for (column = 0; column < CHUNKS_X * CHUNKS_Z; column++) {
    dirty_columns[column] = FALSE;
  }
  beginColumnArenaBuild(active_column_arena);
  for (cx = 0; cx < CHUNKS_X; cx++) {
    for (cz = 0; cz < CHUNKS_Z; cz++) {
      makeColumnDisplayLists(cx, cz);
    }
  }
  column_arena_ends[active_column_arena] = column_dlp;
}

static void markColumnDirty(u8 cx, u8 cz) {
  if (cx < CHUNKS_X && cz < CHUNKS_Z) {
    dirty_columns[cx * CHUNKS_Z + cz] = TRUE;
  }
}

void makeDisplayListsAt(u8 x, u8 z) {
  u8 cx = x / CHUNK_SIZE;
  u8 cz = z / CHUNK_SIZE;

  /* Block edits are cheap to mark now.  A later graphics callback bakes a
     bounded amount of geometry, so mining and tree felling cannot monopolize
     a gameplay frame. */
  markColumnDirty(cx, cz);
  if (x % CHUNK_SIZE == 0 && cx > 0) {
    markColumnDirty(cx - 1, cz);
  }
  if (z % CHUNK_SIZE == 0 && cz > 0) {
    markColumnDirty(cx, cz - 1);
  }
}

static u8 takeDirtyColumn(u8 *cx, u8 *cz) {
  u16 offset;
  u16 count = CHUNKS_X * CHUNKS_Z;

  for (offset = 0; offset < count; offset++) {
    u16 index = (dirty_column_cursor + offset) % count;
    if (dirty_columns[index]) {
      dirty_columns[index] = FALSE;
      dirty_column_cursor = (index + 1) % count;
      *cx = index / CHUNKS_Z;
      *cz = index % CHUNKS_Z;
      return TRUE;
    }
  }
  return FALSE;
}

static void startColumnCompaction(void) {
  u16 index;

  /* Existing dirty marks are already represented in the world array.  The
   * inactive arena is built from that latest state; only edits arriving while
   * compaction is in progress need a later incremental replacement. */
  for (index = 0; index < CHUNKS_X * CHUNKS_Z; index++) {
    dirty_columns[index] = FALSE;
  }
  dirty_column_cursor = 0;
  compaction_column_cursor = 0;
  compacting_columns = TRUE;
  beginColumnArenaBuild(active_column_arena ^ 1);
}

static void processColumnDisplayListUpdates(int can_reclaim_mesh_arena) {
  u8 budget;

  if (compacting_columns) {
    for (budget = 0; budget < MESH_REBUILD_BUDGET &&
        compaction_column_cursor < CHUNKS_X * CHUNKS_Z; budget++) {
      u8 cx = compaction_column_cursor / CHUNKS_Z;
      u8 cz = compaction_column_cursor % CHUNKS_Z;
      makeColumnDisplayLists(cx, cz);
      compaction_column_cursor++;
    }
    if (compaction_column_cursor == CHUNKS_X * CHUNKS_Z) {
      column_arena_ends[column_build_arena] = column_dlp;
      active_column_arena = column_build_arena;
      compacting_columns = FALSE;
    }
    return;
  }

  column_build_arena = active_column_arena;
  column_dlp = column_arena_ends[active_column_arena];
  column_display_list_full = FALSE;
  if (columnArenaFree() < MESH_COMPACTION_RESERVE) {
    /* The old arena can become the inactive build target only after every
     * submitted RSP task has finished reading it. */
    if (can_reclaim_mesh_arena) {
      startColumnCompaction();
    }
    return;
  }

  for (budget = 0; budget < MESH_REBUILD_BUDGET; budget++) {
    u8 cx, cz;
    if (!takeDirtyColumn(&cx, &cz)) {
      break;
    }
    makeColumnDisplayLists(cx, cz);
    column_arena_ends[active_column_arena] = column_dlp;
    if (column_display_list_full || columnArenaFree() < MESH_COMPACTION_RESERVE) {
      break;
    }
  }
}

void drawTextured(u8 texture, u8 player_num) {
  u8 cx, cz;
  for (cx = 0; cx < CHUNKS_X; cx++) {
    for (cz = 0; cz < CHUNKS_Z; cz++) {
      if (visible_columns[player_num][cx * CHUNKS_Z + cz]) {
        gSPDisplayList(dlp++, column_starts[active_column_arena][texture]
          [cx * CHUNKS_Z + cz]);
      }
    }
  }
}

static void setWaterTopVertex(Vtx *vertex, s16 x, s16 z, s16 s, s16 t) {
  vertex->v.ob[0] = x;
  vertex->v.ob[1] = WATER_SURFACE_HEIGHT;
  vertex->v.ob[2] = z;
  vertex->v.flag = 0;
  vertex->v.tc[0] = s;
  vertex->v.tc[1] = t;
  /* Water is rendered unlit so its atlas colours remain stable through the
   * day/night render states.  White modulation keeps the blue texture intact. */
  vertex->v.cn[0] = 255;
  vertex->v.cn[1] = 255;
  vertex->v.cn[2] = 255;
  vertex->v.cn[3] = 255;
}

static void setStevePartTransform(u8 player_num, u8 part, Vector3 local_offset,
    float pitch, float yaw) {
  Player *player = &players[player_num];
  Vector3 offset = rotateY(local_offset, -player->body_yaw);

  guTranslate(&steve_translate[dl_no][player_num][part], player->position.x + offset.x,
    player->position.y + offset.y, player->position.z + offset.z);
  guRotateRPY(&steve_rotate[dl_no][player_num][part], pitch, yaw, 0);
}

static u8 playerHoldingSword(Player *player) {
  ItemStack *held = &player->inventory[INVENTORY_HOTBAR_START +
    player->selected_hotbar_slot];
  return held->count > 0 && held->item == WOOD_SWORD;
}

static float swordSwingAngle(Player *player) {
  float phase;

  if (player->attack_time <= 0) {
    return 0;
  }
  phase = 1.f - player->attack_time / PLAYER_ATTACK_DURATION;
  /* Start raised, sweep through the target at mid-animation, then settle. */
  return -58.f + sinf(phase * 180.f * M_DTOR) * 135.f;
}

static void makeStevePose(u8 player_num) {
  Player *player = &players[player_num];
  float head_pitch = player->pitch > 180 ? player->pitch - 360 : player->pitch;
  float swing = sinf(player->walk_time) * 28 * player->walk_swing;
  float right_arm_pitch = -swing + swordSwingAngle(player);
  float hurt_bob = player->hurt_time > 0 ?
    sinf((PLAYER_ATTACK_DURATION - player->hurt_time) * 180.f * M_DTOR) * 7.f : 0;

  setStevePartTransform(player_num, STEVE_BODY, (Vector3) {hurt_bob, -30, 0},
    0, -player->body_yaw);
  /* Unlike the torso, this orientation uses the current camera yaw/pitch. */
  setStevePartTransform(player_num, STEVE_HEAD, (Vector3) {hurt_bob, 8, 0},
    head_pitch, -player->yaw);
  setStevePartTransform(player_num, STEVE_LEFT_ARM, (Vector3) {-25 + hurt_bob, -8, 0},
    swing, -player->body_yaw);
  setStevePartTransform(player_num, STEVE_RIGHT_ARM, (Vector3) {25 + hurt_bob, -8, 0},
    right_arm_pitch, -player->body_yaw);
  /* The sword's origin is the right hand, and it shares the arm's pitch so
     walking and attacks naturally carry it through the same arc. */
  setStevePartTransform(player_num, STEVE_SWORD, (Vector3) {25 + hurt_bob, -52, 0},
    right_arm_pitch, -player->body_yaw);
  setStevePartTransform(player_num, STEVE_LEFT_LEG, (Vector3) {-10 + hurt_bob, -52, 0},
    -swing, -player->body_yaw);
  setStevePartTransform(player_num, STEVE_RIGHT_LEG, (Vector3) {10 + hurt_bob, -52, 0},
    swing, -player->body_yaw);
}

static void drawStevePart(u8 player_num, u8 part, Vtx *verts, Gfx *part_dl) {
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&steve_translate[dl_no][player_num][part]),
    G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&steve_rotate[dl_no][player_num][part]),
    G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
  gSPVertex(dlp++, verts, 8, 0);
  gSPDisplayList(dlp++, part_dl);
}

static void drawSteve(u8 player_num) {
  makeStevePose(player_num);

  drawStevePart(player_num, STEVE_BODY, steve_body_verts, steve_box_display_list);
  drawStevePart(player_num, STEVE_LEFT_ARM, steve_arm_verts, steve_box_display_list);
  drawStevePart(player_num, STEVE_RIGHT_ARM, steve_arm_verts, steve_box_display_list);
  if (playerHoldingSword(&players[player_num])) {
    drawStevePart(player_num, STEVE_SWORD, steve_sword_blade_verts,
      steve_box_display_list);
    drawStevePart(player_num, STEVE_SWORD, steve_sword_hilt_verts,
      steve_box_display_list);
  }
  drawStevePart(player_num, STEVE_LEFT_LEG, steve_leg_verts, steve_box_display_list);
  drawStevePart(player_num, STEVE_RIGHT_LEG, steve_leg_verts, steve_box_display_list);
  drawStevePart(player_num, STEVE_HEAD, steve_head_verts, steve_box_display_list);

  /* The eyes share the head transform, so their direction matches its pitch
     and yaw exactly. */
  drawStevePart(player_num, STEVE_HEAD, steve_eye_verts, steve_eyes_display_list);
}

static void drawFirstPersonSword(u8 player_num) {
  Player *player = &players[player_num];
  Vector3 forward = {0, 0, -1};
  Vector3 right = {1, 0, 0};
  Vector3 position;
  float swing;

  if (player->camera_mode != CAMERA_FIRST_PERSON || !playerHoldingSword(player)) {
    return;
  }
  forward = rotateX(forward, player->pitch);
  forward = rotateY(forward, -player->yaw);
  right = rotateY(right, -player->yaw);
  position = add(player->position, mul(forward, 72.f));
  position = add(position, mul(right, 34.f));
  position.y -= 36.f;
  swing = swordSwingAngle(player);
  guTranslate(&first_person_sword_translate[dl_no][player_num], position.x,
    position.y, position.z);
  guRotateRPY(&first_person_sword_rotate[dl_no][player_num],
    20.f - player->pitch + swing, -player->yaw, -18.f);
  gSPClearGeometryMode(dlp++, G_CULL_BACK);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&first_person_sword_translate[dl_no][player_num]),
    G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&first_person_sword_rotate[dl_no][player_num]),
    G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
  gSPVertex(dlp++, first_person_arm_verts, 8, 0);
  gSPDisplayList(dlp++, steve_box_display_list);
  gSPVertex(dlp++, steve_sword_blade_verts, 8, 0);
  gSPDisplayList(dlp++, steve_box_display_list);
  gSPVertex(dlp++, steve_sword_hilt_verts, 8, 0);
  gSPDisplayList(dlp++, steve_box_display_list);
  gSPSetGeometryMode(dlp++, G_CULL_BACK);
}

static u8 pointVisibleToPlayer(u8 viewer_num, Vector3 point,
    float max_distance) {
  Vector3 offset = add(point, mul(players[viewer_num].position, -1.f));
  int cx = point.x / (BLOCK_SIZE * CHUNK_SIZE);
  int cz = point.z / (BLOCK_SIZE * CHUNK_SIZE);

  if (dot(offset, offset) > max_distance * max_distance) {
    return FALSE;
  }
  return cx >= 0 && cx < CHUNKS_X && cz >= 0 && cz < CHUNKS_Z &&
    visible_columns[viewer_num][cx * CHUNKS_Z + cz];
}

static void setSheepPartTransform(u8 sheep_num, u8 part,
    Vector3 local_offset) {
  Sheep *mob = &sheep[sheep_num];
  Vector3 offset = rotateY(local_offset, -mob->yaw);

  guTranslate(&sheep_translate[dl_no][sheep_num][part],
    mob->position.x + offset.x, mob->position.y + offset.y,
    mob->position.z + offset.z);
  guRotateRPY(&sheep_rotate[dl_no][sheep_num][part], 0, -mob->yaw, 0);
}

static void drawSheepPart(u8 sheep_num, u8 part, Vtx *verts) {
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&sheep_translate[dl_no][sheep_num][part]),
    G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&sheep_rotate[dl_no][sheep_num][part]),
    G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
  gSPVertex(dlp++, verts, 8, 0);
  gSPDisplayList(dlp++, steve_box_display_list);
}

static void drawSheep(u8 sheep_num) {
  Sheep *mob = &sheep[sheep_num];
  float step = sinf(mob->walk_time) * 4.f;
  float hurt = mob->hurt_time > 0 ?
    sinf((PLAYER_ATTACK_DURATION - mob->hurt_time) * 180.f * M_DTOR) * 4.f : 0;

  setSheepPartTransform(sheep_num, SHEEP_BODY, (Vector3) {hurt, 43, 0});
  setSheepPartTransform(sheep_num, SHEEP_HEAD, (Vector3) {hurt, 43, -29});
  setSheepPartTransform(sheep_num, SHEEP_FRONT_LEFT_LEG,
    (Vector3) {-18 + hurt, 18 + step, -13});
  setSheepPartTransform(sheep_num, SHEEP_FRONT_RIGHT_LEG,
    (Vector3) {18 + hurt, 18 - step, -13});
  setSheepPartTransform(sheep_num, SHEEP_BACK_LEFT_LEG,
    (Vector3) {-18 + hurt, 18 - step, 13});
  setSheepPartTransform(sheep_num, SHEEP_BACK_RIGHT_LEG,
    (Vector3) {18 + hurt, 18 + step, 13});

  drawSheepPart(sheep_num, SHEEP_BODY, sheep_body_verts);
  drawSheepPart(sheep_num, SHEEP_HEAD, sheep_head_verts);
  drawSheepPart(sheep_num, SHEEP_FRONT_LEFT_LEG, sheep_leg_verts);
  drawSheepPart(sheep_num, SHEEP_FRONT_RIGHT_LEG, sheep_leg_verts);
  drawSheepPart(sheep_num, SHEEP_BACK_LEFT_LEG, sheep_leg_verts);
  drawSheepPart(sheep_num, SHEEP_BACK_RIGHT_LEG, sheep_leg_verts);
}

static void drawSheepForPlayer(u8 viewer_num) {
  u8 sheep_num;
  u8 visible = 0;

  gSPTexture(dlp++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
  gDPSetCombineMode(dlp++, G_CC_SHADE, G_CC_SHADE);
  gSPClearGeometryMode(dlp++, G_CULL_BACK);
  for (sheep_num = 0; sheep_num < MAX_SHEEP && visible < MAX_VISIBLE_SHEEP;
      sheep_num++) {
    if (sheep[sheep_num].active && pointVisibleToPlayer(viewer_num,
        sheep[sheep_num].position, SHEEP_RENDER_DISTANCE)) {
      drawSheep(sheep_num);
      visible++;
    }
  }
  gSPSetGeometryMode(dlp++, G_CULL_BACK);
}

static void drawOtherPlayers(u8 viewer_num) {
  u8 player_num;

  gSPTexture(dlp++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
  gDPSetCombineMode(dlp++, G_CC_SHADE, G_CC_SHADE);
  /* The box model has intentionally minimal geometry; disabling culling
     keeps its face and eye quads reliable from every camera angle. */
  gSPClearGeometryMode(dlp++, G_CULL_BACK);
  for (player_num = 0; player_num < active_player_count; player_num++) {
    if (players[player_num].active &&
        (player_num != viewer_num ||
         (players[viewer_num].camera_mode == CAMERA_THIRD_PERSON &&
          third_person_avatar_visible[viewer_num])) &&
        pointVisibleToPlayer(viewer_num, players[player_num].position,
          PLAYER_RENDER_DISTANCE)) {
      drawSteve(player_num);
    }
  }
  gSPSetGeometryMode(dlp++, G_CULL_BACK);
}

static void drawDroppedItems(u8 viewer_num) {
  u8 i;

  /* Small cubes are particularly sensitive to winding/culling mistakes on
     hardware.  Rendering both sides makes pickups visible from every angle. */
  gSPClearGeometryMode(dlp++, G_CULL_BACK);
  for (i = 0; i < MAX_DROPPED_ITEMS; i++) {
    DroppedItem *drop = &dropped_items[i];
    if (!drop->active) {
      continue;
    }
    if (!pointVisibleToPlayer(viewer_num, drop->position,
        DROPPED_ITEM_RENDER_DISTANCE)) {
      continue;
    }

    guTranslate(&dropped_item_translate[dl_no][i], drop->position.x,
      drop->position.y + sinf(drop->rotation * M_DTOR) * 3.f,
      drop->position.z);
    guRotateRPY(&dropped_item_rotate[dl_no][i], 0, drop->rotation, 0);
    loadTexture(preview_textures[drop->item]);
    gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&dropped_item_translate[dl_no][i]),
      G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&dropped_item_rotate[dl_no][i]),
      G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
    gSPVertex(dlp++, dropped_item_verts, 8, 0);
    gSPDisplayList(dlp++, dropped_item_display_list);
  }
  gSPSetGeometryMode(dlp++, G_CULL_BACK);
}

static void drawFallingTreeBox(u8 slot, u8 box, u8 item, float x, float y,
    float z, float sx, float sy, float sz) {
  const float cube_scale = BLOCK_SIZE / 28.f;

  loadTexture(preview_textures[item]);
  guTranslate(&falling_tree_box_translate[dl_no][slot][box], x, y, z);
  guScale(&falling_tree_box_scale[dl_no][slot][box], sx * cube_scale,
    sy * cube_scale, sz * cube_scale);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&falling_tree_translate[dl_no][slot]),
    G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&falling_tree_rotate[dl_no][slot]),
    G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&falling_tree_box_translate[dl_no][slot][box]),
    G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&falling_tree_box_scale[dl_no][slot][box]),
    G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
  gSPVertex(dlp++, dropped_item_verts, 8, 0);
  gSPDisplayList(dlp++, dropped_item_display_list);
}

static void drawFallingTree(TreeRecord *tree, u8 slot) {
  float progress = tree->fall_progress;
  float eased = progress * progress * (3.f - 2.f * progress);
  float angle = eased * 88.f + sinf(progress * 720.f * M_DTOR) *
    (1.f - progress) * 3.f;
  float pitch = 0;
  float roll = 0;
  float height = tree->canopy_y - tree->base_y + 1;
  float canopy_y = (tree->canopy_y - tree->base_y) * BLOCK_SIZE;

  if (tree->fall_direction == 0) {
    roll = -angle;
  } else if (tree->fall_direction == 1) {
    pitch = angle;
  } else if (tree->fall_direction == 2) {
    roll = angle;
  } else {
    pitch = -angle;
  }

  guTranslate(&falling_tree_translate[dl_no][slot],
    (tree->x + 0.5f) * BLOCK_SIZE, (tree->base_y + 1) * BLOCK_SIZE,
    (tree->z + 0.5f) * BLOCK_SIZE);
  guRotateRPY(&falling_tree_rotate[dl_no][slot], pitch, 0, roll);

  drawFallingTreeBox(slot, 0, WOOD, 0, height * BLOCK_SIZE / 2.f, 0,
    1, height, 1);
  drawFallingTreeBox(slot, 1, LEAVES, 0, canopy_y + BLOCK_SIZE, 0,
    3, 3, 3);
  drawFallingTreeBox(slot, 2, LEAVES, 0, canopy_y + BLOCK_SIZE * 1.5f,
    -BLOCK_SIZE * 1.8f, 3, 1, 1);
  drawFallingTreeBox(slot, 3, LEAVES, 0, canopy_y + BLOCK_SIZE * 1.5f,
    BLOCK_SIZE * 1.8f, 3, 1, 1);
  drawFallingTreeBox(slot, 4, LEAVES, -BLOCK_SIZE * 1.8f,
    canopy_y + BLOCK_SIZE * 1.5f, 0, 1, 1, 3);
  drawFallingTreeBox(slot, 5, LEAVES, BLOCK_SIZE * 1.8f,
    canopy_y + BLOCK_SIZE * 1.5f, 0, 1, 1, 3);
}

static void drawFallingTrees(u8 viewer_num) {
  u8 tree_index;
  u8 slot = 0;

  for (tree_index = 0; tree_index < MAX_TREES; tree_index++) {
    Vector3 position = {(trees[tree_index].x + .5f) * BLOCK_SIZE,
      (trees[tree_index].base_y + 1) * BLOCK_SIZE,
      (trees[tree_index].z + .5f) * BLOCK_SIZE};
    if (trees[tree_index].state == TREE_STATE_FALLING &&
        pointVisibleToPlayer(viewer_num, position, DROPPED_ITEM_RENDER_DISTANCE)) {
      break;
    }
  }
  if (tree_index == MAX_TREES) {
    return;
  }

  gSPClearGeometryMode(dlp++, G_CULL_BACK);
  for (; tree_index < MAX_TREES &&
      slot < FALLING_TREE_RENDER_SLOTS; tree_index++) {
    Vector3 position = {(trees[tree_index].x + .5f) * BLOCK_SIZE,
      (trees[tree_index].base_y + 1) * BLOCK_SIZE,
      (trees[tree_index].z + .5f) * BLOCK_SIZE};
    if (trees[tree_index].state == TREE_STATE_FALLING &&
        pointVisibleToPlayer(viewer_num, position, DROPPED_ITEM_RENDER_DISTANCE)) {
      drawFallingTree(&trees[tree_index], slot++);
    }
  }
  gSPSetGeometryMode(dlp++, G_CULL_BACK);
}

void drawWorld() {
  u8 i, player_num;
  u8 cinematic = current_screen == LOADING_PREVIEW || current_screen == MENU ||
    current_screen == WORLD_NAMING;
  u8 viewer_count = cinematic ? 1 : active_player_count;

  if (cinematic) {
    clearBuffers(GPACK_RGBA5551(42, 78, 130, 1));
  } else {
    /* A single time-varying clear is much cheaper than the former banded
     * gradient while retaining a daylight/nightfall sky behind the terrain. */
    SkyColor sky = dayCycleSkyColor(255);
    clearBuffers(GPACK_RGBA5551(sky.r, sky.g, sky.b, 1));
  }

  for (player_num = 0; player_num < viewer_count; player_num++) {
    if (viewer_count > 1) {
      selectPlayerViewport(player_num);
    } else {
      gSPViewport(dlp++, &full_viewport);
      gDPSetScissor(dlp++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WD, SCREEN_HT);
    }
    gSPDisplayList(dlp++, draw_setup_display_list);
    loadCameraMatrices(player_num);
    if (!cinematic) {
      drawCelestialBodies(player_num);
      /* Celestial sprites use AA texture-edge mode.  Terrain must explicitly
       * restore its opaque no-read render mode afterwards. */
      gDPSetRenderMode(dlp++, G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2);
    }
    gSPSetGeometryMode(dlp++, G_CULL_BACK | G_LIGHTING);
    gDPSetCombineMode(dlp++, G_CC_MODULATERGB, G_CC_MODULATERGB);
    if (cinematic) {
      setPreviewLight();
      gSPSetLights0(dlp++, preview_lights[dl_no]);
    } else {
      setWorldLight(player_num);
      gSPSetLights1(dlp++, world_lights[dl_no][player_num]);
    }
    for (i = 0; i < NUM_TEXTURES; i++) {
      if (textures[i] == &water_spec) {
        /* Water's dedicated vertices carry white RGB, not normals. */
        gSPClearGeometryMode(dlp++, G_LIGHTING);
      }
      loadTexture(textures[i]->texture);
      drawTextured(i, player_num);
      if (textures[i] == &water_spec) {
        gSPSetGeometryMode(dlp++, G_LIGHTING);
      }
    }
    if (!cinematic) {
      gSPClearGeometryMode(dlp++, G_LIGHTING);
      drawFallingTrees(player_num);
      drawDroppedItems(player_num);
      drawSheepForPlayer(player_num);
    }
    if (!cinematic && (active_player_count > 1 ||
        players[player_num].camera_mode == CAMERA_THIRD_PERSON)) {
      drawOtherPlayers(player_num);
    }
    if (!cinematic) {
      drawFirstPersonSword(player_num);
    }
  }

}

void drawWireframes() {
  u8 player_num;

  for (player_num = 0; player_num < active_player_count; player_num++) {
    if (!players[player_num].target_present) continue;
    if (active_player_count > 1) {
      selectPlayerViewport(player_num);
    } else {
      gSPViewport(dlp++, &full_viewport);
      gDPSetScissor(dlp++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WD, SCREEN_HT);
    }
    gSPDisplayList(dlp++, wireframe_setup_display_list);
    loadCameraMatrices(player_num);
    gSPMatrix(dlp++,OS_K0_TO_PHYSICAL(c_models + (players[player_num].target_x / CHUNK_SIZE) * CHUNKS_Y * CHUNKS_Z + (players[player_num].target_y / CHUNK_SIZE) * CHUNKS_Z + (players[player_num].target_z / CHUNK_SIZE)),
      G_MTX_MODELVIEW|G_MTX_LOAD|G_MTX_NOPUSH);
    gSPMatrix(dlp++,OS_K0_TO_PHYSICAL(b_models + (players[player_num].target_x % CHUNK_SIZE) * CHUNK_SIZE * CHUNK_SIZE + (players[player_num].target_y % CHUNK_SIZE) * CHUNK_SIZE + (players[player_num].target_z % CHUNK_SIZE)),
      G_MTX_MODELVIEW|G_MTX_MUL|G_MTX_NOPUSH);
    gSPVertex(dlp++, cube_verts, 8, 0);
    gSPDisplayList(dlp++, wireframe_display_list);
    if (players[player_num].breaking) {
      u8 stage = players[player_num].break_progress * 3 /
        players[player_num].break_time;
      if (stage > 2) {
        stage = 2;
      }
      gSPVertex(dlp++, breaking_crack_verts, 23, 0);
      gSPDisplayList(dlp++, breaking_crack_stages[stage]);
    }
  }

}

static void setHudFillColor(u8 r, u8 g, u8 b);

static void drawLoadingOverlay() {
  u32 progress = loadingPreviewProgress();
  u32 filled = progress * 224 / 100;
  u32 pulse = (progress / 8) % 3;

  /* A compact framed status deck preserves nearly all of the flyover while
     making the handoff feel intentional on a CRT. */
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(9, 16, 30);
  gDPFillRectangle(dlp++, 0, SCREEN_HT - 36, SCREEN_WD - 1, SCREEN_HT - 1);
  setHudFillColor(71, 160, 196);
  gDPFillRectangle(dlp++, 47, SCREEN_HT - 20, 272, SCREEN_HT - 14);
  setHudFillColor(13, 35, 53);
  gDPFillRectangle(dlp++, 49, SCREEN_HT - 18, 270, SCREEN_HT - 16);
  if (filled > 0) {
    setHudFillColor(100 + pulse * 24, 215, 192);
    gDPFillRectangle(dlp++, 49, SCREEN_HT - 18, 48 + filled,
      SCREEN_HT - 16);
  }
  gDPPipeSync(dlp++);
}

static void drawCrosshair(u32 x, u32 y, Player *player) {
  u8 red = 255;
  u8 green = 255;
  u8 blue = 255;

  if (player->breaking) {
    green = 180;
    blue = 48;
  } else if (player->target_present) {
    red = 150;
    blue = 150;
  }
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  gDPSetFillColor(dlp++, (GPACK_RGBA5551(0, 0, 0, 1) << 16 |
    GPACK_RGBA5551(0, 0, 0, 1)));
  gDPFillRectangle(dlp++, x - CROSSHAIR_SIZE / 2 - 1, y - 2,
    x + CROSSHAIR_SIZE / 2, y + 1);
  gDPFillRectangle(dlp++, x - 2, y - CROSSHAIR_SIZE / 2 - 1,
    x + 1, y + CROSSHAIR_SIZE / 2);
  gDPSetFillColor(dlp++, (GPACK_RGBA5551(red, green, blue, 1) << 16 |
    GPACK_RGBA5551(red, green, blue, 1)));
  gDPFillRectangle(dlp++, x - CROSSHAIR_SIZE / 2, y - 1, x + CROSSHAIR_SIZE / 2 - 1, y);
  gDPFillRectangle(dlp++, x - 1, y - CROSSHAIR_SIZE / 2, x, y + CROSSHAIR_SIZE / 2 - 1);
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_1CYCLE);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
  gDPSetPrimColor(dlp++, 0, 0, 255, 255, 255, 255);
  gDPSetTexturePersp(dlp++, G_TP_NONE);
  gDPSetTextureLUT(dlp++, G_TT_RGBA16);
}

static void drawBreakProgress(u32 x, u32 y, Player *player) {
  u32 width;

  if (!player->breaking || player->break_time <= 0) {
    return;
  }
  width = player->break_progress * 30 / player->break_time;
  if (width > 30) {
    width = 30;
  }
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(15, 15, 15);
  gDPFillRectangle(dlp++, x - 17, y + 10, x + 16, y + 14);
  if (width > 0) {
    setHudFillColor(255, 190, 48);
    gDPFillRectangle(dlp++, x - 15, y + 12, x - 16 + width, y + 12);
  }
}

static void setHudFillColor(u8 r, u8 g, u8 b) {
  u32 color = GPACK_RGBA5551(r, g, b, 1);
  gDPSetFillColor(dlp++, (color << 16) | color);
}

static void drawHealth(u8 player_num) {
  u8 compact = usesFourPlayerLayout();
  u32 size = compact ? 4 : 6;
  u32 x = playerViewportX(player_num) + (compact ? 5 : 7);
  u32 y = playerViewportY(player_num) + playerViewportHeight() -
    (compact ? 14 + 4 + 8 : HOTBAR_SLOT_SIZE + HOTBAR_MARGIN + 10);
  u8 heart;

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  for (heart = 0; heart < PLAYER_MAX_HEALTH / 2; heart++) {
    u8 full = players[player_num].health >= (heart + 1) * 2;
    setHudFillColor(full ? 220 : 72, full ? 48 : 22, full ? 48 : 22);
    /* A few filled pixels read as a heart on a CRT more clearly than a
       single health bar, while still fitting beside a four-player hotbar. */
    gDPFillRectangle(dlp++, x + 1, y, x + size - 2, y + 1);
    gDPFillRectangle(dlp++, x, y + 1, x + size - 1, y + size - 2);
    gDPFillRectangle(dlp++, x + 1, y + size - 1, x + size - 2, y + size);
    x += size + 2;
  }
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_1CYCLE);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
  gDPSetPrimColor(dlp++, 0, 0, 255, 255, 255, 255);
  gDPSetTexturePersp(dlp++, G_TP_NONE);
  gDPSetTextureLUT(dlp++, G_TT_RGBA16);
}

/* A compact, deliberately chunky version of the Minecraft hotbar.  It uses
   the existing 16x16 block previews, keeping the HUD cheap enough for both
   split-screen players without introducing another texture atlas. */
static void drawItemIcon(u8 item, u32 x, u32 y, u32 size) {
  if (ITEM_IS_VALID(item) &&
      preview_textures[item] != NULL) {
    loadTexture(preview_textures[item]);
    gSPTextureRectangle(dlp++, x << 2, y << 2,
      ((x + size) << 2) - 2, ((y + size) << 2) - 2,
      G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);
    return;
  }

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  if (item == STICK) {
    setHudFillColor(142, 83, 38);
    gDPFillRectangle(dlp++, x + size / 2 - 1, y + 2, x + size / 2 + 1, y + size - 3);
  } else if (item == WOOD_SWORD) {
    setHudFillColor(188, 188, 178);
    gDPFillRectangle(dlp++, x + size / 2 - 1, y + 1, x + size / 2 + 1, y + size - 6);
    setHudFillColor(142, 83, 38);
    gDPFillRectangle(dlp++, x + size / 2 - 3, y + size - 6, x + size / 2 + 3, y + size - 4);
    gDPFillRectangle(dlp++, x + size / 2 - 1, y + size - 3, x + size / 2 + 1, y + size - 1);
  } else if (item == WOOD_PICKAXE) {
    setHudFillColor(188, 188, 178);
    gDPFillRectangle(dlp++, x + 1, y + 2, x + size - 2, y + 4);
    setHudFillColor(142, 83, 38);
    gDPFillRectangle(dlp++, x + size / 2 - 1, y + 4, x + size / 2 + 1, y + size - 1);
  }
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_1CYCLE);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
  gDPSetPrimColor(dlp++, 0, 0, 255, 255, 255, 255);
  gDPSetTexturePersp(dlp++, G_TP_NONE);
  gDPSetTextureLUT(dlp++, G_TT_RGBA16);
}

static void drawHotbar(u8 player_num) {
  u8 compact = usesFourPlayerLayout();
  u32 viewport_width = playerViewportWidth();
  u32 viewport_height = playerViewportHeight();
  u32 x_offset = playerViewportX(player_num);
  u32 y_offset = playerViewportY(player_num);
  u32 slot_size = compact ? 14 : HOTBAR_SLOT_SIZE;
  u32 icon_size = compact ? 10 : HOTBAR_ICON_SIZE;
  u32 preview_size = compact ? 16 : HELD_PREVIEW_SIZE;
  u32 margin = compact ? 4 : HOTBAR_MARGIN;
  u32 bar_width = HOTBAR_SLOT_COUNT * slot_size;
  u32 bar_x = x_offset + (viewport_width - bar_width) / 2;
  u32 bar_y = y_offset + viewport_height - slot_size - margin;
  u32 held_x = x_offset + viewport_width - preview_size - margin;
  u32 held_y = bar_y - preview_size - 3;
  u8 slot;

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);

  /* One raised rim groups all nine slots into a single familiar item bar. */
  setHudFillColor(20, 20, 20);
  gDPFillRectangle(dlp++, bar_x - 2, bar_y - 2, bar_x + bar_width + 1,
    bar_y + slot_size + 1);

  for (slot = 0; slot < HOTBAR_SLOT_COUNT; slot++) {
    u32 x = bar_x + slot * slot_size;
    u8 selected = players[player_num].selected_hotbar_slot == slot;

    setHudFillColor(selected ? 250 : 78, selected ? 250 : 78, selected ? 250 : 78);
    gDPFillRectangle(dlp++, x, bar_y, x + slot_size - 1,
      bar_y + slot_size - 1);
    setHudFillColor(selected ? 118 : 42, selected ? 118 : 42, selected ? 118 : 42);
    gDPFillRectangle(dlp++, x + 2, bar_y + 2, x + slot_size - 3,
      bar_y + slot_size - 3);
  }

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_1CYCLE);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
  gDPSetPrimColor(dlp++, 0, 0, 255, 255, 255, 255);
  gDPSetTexturePersp(dlp++, G_TP_NONE);
  gDPSetTextureLUT(dlp++, G_TT_RGBA16);

  for (slot = 0; slot < HOTBAR_SLOT_COUNT; slot++) {
    u32 x = bar_x + slot * slot_size + (slot_size - icon_size) / 2;
    u32 y = bar_y + (slot_size - icon_size) / 2;
    ItemStack *stack = &players[player_num].inventory[INVENTORY_HOTBAR_START + slot];
    if (stack->count > 0) {
      drawItemIcon(stack->item, x, y, icon_size);
    }
  }

  /* The enlarged current block is the first-person held-item cue. */
  if (players[player_num].inventory[INVENTORY_HOTBAR_START +
      players[player_num].selected_hotbar_slot].count > 0) {
    drawItemIcon(players[player_num].held_block, held_x, held_y, preview_size);
  }
}

static void drawInventorySlot(u32 x, u32 y, ItemStack *stack, u8 selected) {
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(76, 76, 76);
  gDPFillRectangle(dlp++, x, y, x + INVENTORY_SLOT_SIZE - 1, y + INVENTORY_SLOT_SIZE - 1);
  setHudFillColor(38, 38, 38);
  gDPFillRectangle(dlp++, x + 2, y + 2, x + INVENTORY_SLOT_SIZE - 3, y + INVENTORY_SLOT_SIZE - 3);

  if (stack->item != AIR && stack->count > 0) {
    gDPPipeSync(dlp++);
    gDPSetCycleType(dlp++, G_CYC_1CYCLE);
    gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
    gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
    gDPSetPrimColor(dlp++, 0, 0, 255, 255, 255, 255);
    gDPSetTexturePersp(dlp++, G_TP_NONE);
    gDPSetTextureLUT(dlp++, G_TT_RGBA16);
    drawItemIcon(stack->item, x + 2, y + 2, INVENTORY_ICON_SIZE);
  }

  /* Equipped is a small green tab, distinct from the movable cursor. */
  if (selected) {
    gDPPipeSync(dlp++);
    gDPSetCycleType(dlp++, G_CYC_FILL);
    gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
    setHudFillColor(90, 220, 110);
    gDPFillRectangle(dlp++, x + 5, y + INVENTORY_SLOT_SIZE - 3,
      x + INVENTORY_SLOT_SIZE - 6, y + INVENTORY_SLOT_SIZE - 1);
  }
}

static void drawInventoryFocus(u32 x, u32 y) {
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(255, 232, 72);
  gDPFillRectangle(dlp++, x, y, x + INVENTORY_SLOT_SIZE - 1, y + 1);
  gDPFillRectangle(dlp++, x, y + INVENTORY_SLOT_SIZE - 2,
    x + INVENTORY_SLOT_SIZE - 1, y + INVENTORY_SLOT_SIZE - 1);
  gDPFillRectangle(dlp++, x, y + 2, x + 1, y + INVENTORY_SLOT_SIZE - 3);
  gDPFillRectangle(dlp++, x + INVENTORY_SLOT_SIZE - 2, y + 2,
    x + INVENTORY_SLOT_SIZE - 1, y + INVENTORY_SLOT_SIZE - 3);
}

/* While crafting, the last inventory slot remains the material source.  Its
   cool outline distinguishes it from the active yellow cursor and the green
   equipped hotbar slot without obscuring either icon. */
static void drawInventorySource(u32 x, u32 y) {
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(76, 172, 236);
  gDPFillRectangle(dlp++, x + 2, y + 2, x + INVENTORY_SLOT_SIZE - 3, y + 2);
  gDPFillRectangle(dlp++, x + 2, y + INVENTORY_SLOT_SIZE - 3,
    x + INVENTORY_SLOT_SIZE - 3, y + INVENTORY_SLOT_SIZE - 3);
  gDPFillRectangle(dlp++, x + 2, y + 3, x + 2, y + INVENTORY_SLOT_SIZE - 4);
  gDPFillRectangle(dlp++, x + INVENTORY_SLOT_SIZE - 3, y + 3,
    x + INVENTORY_SLOT_SIZE - 3, y + INVENTORY_SLOT_SIZE - 4);
}

static void drawInventory() {
  Player *player = &players[inventory_player];
  u8 craft_columns = player->crafting_table_open ? CRAFTING_TABLE_COLUMNS : PLAYER_CRAFTING_COLUMNS;
  u8 craft_rows = player->crafting_table_open ? CRAFTING_TABLE_ROWS : PLAYER_CRAFTING_ROWS;
  u32 craft_x = player->crafting_table_open ? 40 : 48;
  u32 output_x = player->crafting_table_open ? 104 : 86;
  u32 output_y = player->crafting_table_open ? 71 : 62;
  u32 slot_x = player->crafting_table_open ? 145 : 115;
  u32 slot_y = 53;
  u8 row, column;
  ItemStack output = {AIR, 0};

  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setHudFillColor(18, 18, 18);
  gDPFillRectangle(dlp++, 30, 18, SCREEN_WD - 31, SCREEN_HT - 19);
  setHudFillColor(112, 112, 112);
  gDPFillRectangle(dlp++, 33, 21, SCREEN_WD - 34, SCREEN_HT - 22);
  setHudFillColor(54, 54, 54);
  gDPFillRectangle(dlp++, 36, 24, SCREEN_WD - 37, SCREEN_HT - 25);

  for (row = 0; row < craft_rows; row++) {
    for (column = 0; column < craft_columns; column++) {
      u8 index = row * CRAFTING_TABLE_COLUMNS + column;
      drawInventorySlot(craft_x + column * INVENTORY_SLOT_SIZE, 53 + row * INVENTORY_SLOT_SIZE,
        &player->crafting[index], FALSE);
    }
  }
  getCraftResult(player, &output);
  drawInventorySlot(output_x, output_y, &output, FALSE);
  drawInventorySlot(output_x, 104, &player->carried_item, FALSE);

  for (row = 0; row < INVENTORY_STORAGE_ROWS + INVENTORY_HOTBAR_ROWS; row++) {
    for (column = 0; column < INVENTORY_COLUMNS; column++) {
      u8 index = row * INVENTORY_COLUMNS + column;
      drawInventorySlot(slot_x + column * INVENTORY_SLOT_SIZE, slot_y + row * INVENTORY_SLOT_SIZE,
        &player->inventory[index], row == INVENTORY_STORAGE_ROWS &&
        player->selected_hotbar_slot == column);
    }
  }

  if (player->inventory_area == INVENTORY_AREA_CRAFTING) {
    drawInventorySource(slot_x + (player->inventory_cursor % INVENTORY_COLUMNS) *
      INVENTORY_SLOT_SIZE, slot_y + (player->inventory_cursor / INVENTORY_COLUMNS) *
      INVENTORY_SLOT_SIZE);
  }

  /* Draw the cursor last so every edge remains intact on the real RDP. */
  if (player->inventory_area == INVENTORY_AREA_CRAFTING) {
    drawInventoryFocus(craft_x +
      (player->crafting_cursor % CRAFTING_TABLE_COLUMNS) * INVENTORY_SLOT_SIZE,
      53 + (player->crafting_cursor / CRAFTING_TABLE_COLUMNS) *
      INVENTORY_SLOT_SIZE);
  } else if (player->inventory_area == INVENTORY_AREA_OUTPUT) {
    drawInventoryFocus(output_x, output_y);
  } else {
    drawInventoryFocus(slot_x +
      (player->inventory_cursor % INVENTORY_COLUMNS) * INVENTORY_SLOT_SIZE,
      slot_y + (player->inventory_cursor / INVENTORY_COLUMNS) *
      INVENTORY_SLOT_SIZE);
  }
}

static void drawStackCount(ItemStack *stack, u32 x, u32 y) {
  if (stack->count >= 10) {
    drawChar('0' + stack->count / 10, x + 2, y + 8);
    drawChar('0' + stack->count % 10, x + 8, y + 8);
  } else if (stack->count > 0) {
    drawChar('0' + stack->count, x + 8, y + 8);
  }
}

static void drawInventoryStackCounts() {
  Player *player = &players[inventory_player];
  u8 craft_columns = player->crafting_table_open ? CRAFTING_TABLE_COLUMNS : PLAYER_CRAFTING_COLUMNS;
  u8 craft_rows = player->crafting_table_open ? CRAFTING_TABLE_ROWS : PLAYER_CRAFTING_ROWS;
  u32 craft_x = player->crafting_table_open ? 40 : 48;
  u32 output_x = player->crafting_table_open ? 104 : 86;
  u32 output_y = player->crafting_table_open ? 71 : 62;
  u32 slot_x = player->crafting_table_open ? 145 : 115;
  u32 slot_y = 53;
  u8 row, column;
  ItemStack output = {AIR, 0};

  for (row = 0; row < craft_rows; row++) {
    for (column = 0; column < craft_columns; column++) {
      u8 index = row * CRAFTING_TABLE_COLUMNS + column;
      drawStackCount(&player->crafting[index], craft_x + column * INVENTORY_SLOT_SIZE,
        53 + row * INVENTORY_SLOT_SIZE);
    }
  }
  getCraftResult(player, &output);
  drawStackCount(&output, output_x, output_y);
  drawStackCount(&player->carried_item, output_x, 104);

  for (row = 0; row < INVENTORY_STORAGE_ROWS + INVENTORY_HOTBAR_ROWS; row++) {
    for (column = 0; column < INVENTORY_COLUMNS; column++) {
      ItemStack *stack = &player->inventory[row * INVENTORY_COLUMNS + column];
      u32 x = slot_x + column * INVENTORY_SLOT_SIZE;
      u32 y = slot_y + row * INVENTORY_SLOT_SIZE;

      drawStackCount(stack, x, y);
    }
  }
}

static void drawGameText() {
  u8 player_num;

  beginText();
  for (player_num = 0; player_num < active_player_count; player_num++) {
    u32 x_offset = playerViewportX(player_num);
    u32 y_offset = playerViewportY(player_num);
    u32 viewport_height = playerViewportHeight();
    u32 bar_width = HOTBAR_SLOT_COUNT * HOTBAR_SLOT_SIZE;
    u32 bar_x;
    u32 bar_y;
    ItemStack *held_stack = &players[player_num].inventory[
      INVENTORY_HOTBAR_START + players[player_num].selected_hotbar_slot];
    u8 slot;

    if (usesFourPlayerLayout()) {
      bar_width = HOTBAR_SLOT_COUNT * 14;
      bar_x = x_offset + (playerViewportWidth() - bar_width) / 2;
      bar_y = y_offset + viewport_height - 14 - 4;
      drawChar('P', x_offset + 5, y_offset + 5);
      drawChar('1' + player_num, x_offset + 12, y_offset + 5);
    } else {
      bar_x = (SCREEN_WD - bar_width) / 2;
      bar_y = y_offset + viewport_height - HOTBAR_SLOT_SIZE - HOTBAR_MARGIN;
      drawChar('P', 6, y_offset + 6);
      drawChar('1' + player_num, 13, y_offset + 6);
      if (players[player_num].camera_mode == CAMERA_THIRD_PERSON) {
        drawString("3P", 25, y_offset + 6);
        drawString(itemName(held_stack->count > 0 ? held_stack->item : AIR),
          43, y_offset + 6);
      } else {
        drawString(itemName(held_stack->count > 0 ? held_stack->item : AIR),
          25, y_offset + 6);
      }
      if (pickup_message[player_num] > 0) {
        drawChar('+', 6, y_offset + 16);
        drawString(itemName(pickup_item[player_num]), 13, y_offset + 16);
      }
    }
    for (slot = 0; slot < HOTBAR_SLOT_COUNT; slot++) {
      ItemStack *stack = &players[player_num].inventory[
        INVENTORY_HOTBAR_START + slot];
      drawStackCount(stack, bar_x + slot * (usesFourPlayerLayout() ? 14 : HOTBAR_SLOT_SIZE) + 2,
        bar_y + 1);
    }
  }
}

void drawHUD() {
  u8 player_num;

  if (current_screen != GAME && current_screen != INVENTORY &&
      current_screen != LOADING_PREVIEW &&
      !((current_screen == MENU || current_screen == WORLD_NAMING) &&
        !menuPreviewRequested())) {
    clearBuffers(GPACK_RGBA5551(0, 0, 0, 1));
  } else if (current_screen == LOADING_PREVIEW) {
    drawLoadingOverlay();
  } else if (current_screen == MENU || current_screen == WORLD_NAMING) {
    /* The carousel has its own text overlay; never let the inventory panel
       fall through on top of its world preview. */
  } else if (current_screen == GAME) {
    loaded_texture = NULL;
    for (player_num = 0; player_num < active_player_count; player_num++) {
      u32 crosshair_x = playerViewportX(player_num) + playerViewportWidth() / 2;
      u32 crosshair_y = playerViewportY(player_num) + playerViewportHeight() / 2;
      drawCrosshair(crosshair_x, crosshair_y, &players[player_num]);
      drawBreakProgress(crosshair_x, crosshair_y, &players[player_num]);
      drawHotbar(player_num);
      drawHealth(player_num);
    }
    if (active_player_count > 1) {
      gDPSetCycleType(dlp++, G_CYC_FILL);
      gDPSetFillColor(dlp++, (GPACK_RGBA5551(0, 0, 0, 1) << 16 | GPACK_RGBA5551(0, 0, 0, 1)));
      gDPFillRectangle(dlp++, 0, SCREEN_HT / 2 - 1, SCREEN_WD - 1, SCREEN_HT / 2);
      if (usesFourPlayerLayout()) {
        gDPFillRectangle(dlp++, SCREEN_WD / 2 - 1, 0, SCREEN_WD / 2, SCREEN_HT - 1);
      }
      gDPPipeSync(dlp++);
    }
    gDPPipeSync(dlp++);
  } else {
    /* Inventory input pauses movement, so redrawing the 3D world beneath a
     * mostly opaque panel only burns RSP/RDP time.  A stable dark backdrop is
     * also safe with NuSystem's rotating triple framebuffer. */
    clearBuffers(GPACK_RGBA5551(10, 16, 28, 1));
    loaded_texture = NULL;
    drawInventory();
    gDPPipeSync(dlp++);
  }
  drawMenu();
  if (current_screen == GAME) {
    drawGameText();
  } else if (current_screen == INVENTORY) {
    drawInventoryStackCounts();
  }

}

void draw(int can_reclaim_mesh_arena) {
  loaded_texture = NULL;
  dlp = &frame_display_lists[dl_no][0];
  gSPDisplayList(dlp++, setup_display_list);

  if (current_screen == GAME) {
    u8 player_num;
    for (player_num = 0; player_num < active_player_count; player_num++) {
      updateVisibleColumns(player_num);
      updateCameraMatrices(player_num);
    }
    processColumnDisplayListUpdates(can_reclaim_mesh_arena);
    drawWorld();
    drawWireframes();
  } else if (current_screen == LOADING_PREVIEW) {
    updateLoadingCamera();
    processColumnDisplayListUpdates(can_reclaim_mesh_arena);
    drawWorld();
  } else if ((current_screen == MENU || current_screen == WORLD_NAMING) &&
      !menuPreviewRequested()) {
    updateLoadingCamera();
    processColumnDisplayListUpdates(can_reclaim_mesh_arena);
    drawWorld();
  }
  drawHUD();

  gDPFullSync(dlp++);
  gSPEndDisplayList(dlp++);
  assert(dlp - frame_display_lists[dl_no] <= FRAME_DISPLAY_LIST_SIZE);
  nuGfxTaskStart(frame_display_lists[dl_no],
    (s32)(dlp - frame_display_lists[dl_no]) * sizeof (Gfx),
    NU_GFX_UCODE_F3DEX, NU_SC_SWAPBUFFER);

  /* Switch display list buffers */
  dl_no ^= 1;
}

void initGraphics() {
  int x, y, z;

  nuGfxInit();
  nuGfxDisplayOn();

  for (x = 0; x < CHUNKS_X; x++) {
    for (y = 0; y < CHUNKS_Y; y++) {
      for (z = 0; z < CHUNKS_Z; z++) {
        guTranslate(&(c_models[x * CHUNKS_Y * CHUNKS_Z + y * CHUNKS_Z + z]), x * BLOCK_SIZE * CHUNK_SIZE, y * BLOCK_SIZE * CHUNK_SIZE, z * BLOCK_SIZE * CHUNK_SIZE);
      }
    }
  }

  for (x = 0; x < CHUNK_SIZE; x++) {
    for (y = 0; y < CHUNK_SIZE; y++) {
      for (z = 0; z < CHUNK_SIZE; z++) {
        guTranslate(&(b_models[x * CHUNK_SIZE * CHUNK_SIZE + y * CHUNK_SIZE + z]), x * BLOCK_SIZE, y * BLOCK_SIZE, z * BLOCK_SIZE);
      }
    }
  }

  for (x = 0; x < CHUNK_SIZE; x++) {
    for (z = 0; z < CHUNK_SIZE; z++) {
      Vtx *vertices = WATER_TOP_QUAD_ADDR(x, z);
      s16 width = (x + 1) * BLOCK_SIZE;
      s16 depth = (z + 1) * BLOCK_SIZE;
      s16 texture_width = (x + 1) * 1024;
      s16 texture_depth = (z + 1) * 1024;

      setWaterTopVertex(&vertices[0], 0, 0, 0, 0);
      setWaterTopVertex(&vertices[1], width, 0, texture_width, 0);
      setWaterTopVertex(&vertices[2], width, depth, texture_width,
        texture_depth);
      setWaterTopVertex(&vertices[3], 0, depth, 0, texture_depth);
    }
  }

  active_column_arena = 0;
  compacting_columns = FALSE;
  beginColumnArenaBuild(active_column_arena);
  column_arena_ends[active_column_arena] = column_dlp;
}
