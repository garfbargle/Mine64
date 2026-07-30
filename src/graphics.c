#include <nusys.h>
#include <assert.h>
#include "graphics.h"
#include "geometry.h"
#include "items.h"
#include "camera.h"
#include "player.h"
#include "menu.h"
#include "quads.h"
#include "cube.h"
#include "textures.h"

#define CROSSHAIR_SIZE 10
#define HOTBAR_SLOT_COUNT INVENTORY_COLUMNS
#define HOTBAR_SLOT_SIZE 22
#define HOTBAR_ICON_SIZE 16
#define HOTBAR_MARGIN 7
#define HELD_PREVIEW_SIZE 32
#define INVENTORY_SLOT_SIZE 18
#define INVENTORY_ICON_SIZE 14

Gfx *dlp;
u32 dl_no = 0;
Gfx display_lists[NUM_DISPLAY_LISTS][WORLD_DISPLAY_LIST_SIZE];
Gfx line_display_lists[NUM_DISPLAY_LISTS][LINE_DISPLAY_LIST_SIZE];
Gfx hud_display_lists[NUM_DISPLAY_LISTS][HUD_DISPLAY_LIST_SIZE];

Gfx column_display_list[DISPLAY_LIST_SIZE];
Gfx *column_dlp;
Gfx *column_starts[NUM_TEXTURES][CHUNKS_X * CHUNKS_Z];
static u8 column_display_list_full;

static Gfx empty_column_display_list[] = {
  gsSPEndDisplayList()
};

#define BLOCKS_PER_CHUNK (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)

static Mtx c_models[NUM_BLOCKS / BLOCKS_PER_CHUNK];
static Mtx b_models[CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE];

static Mtx dropped_item_translate[NUM_DISPLAY_LISTS][MAX_DROPPED_ITEMS];
static Mtx dropped_item_rotate[NUM_DISPLAY_LISTS][MAX_DROPPED_ITEMS];

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
#define STEVE_PART_COUNT 6

static Mtx steve_translate[NUM_DISPLAY_LISTS][MAX_PLAYERS][STEVE_PART_COUNT];
static Mtx steve_rotate[NUM_DISPLAY_LISTS][MAX_PLAYERS][STEVE_PART_COUNT];

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

/* Two blue eye quads on the local -Z face make it obvious where Steve is
   looking, even without a character texture. */
static Vtx steve_eye_verts[] = {
  STEVE_VERTEX(-11, 8, -17, 55, 125, 210), STEVE_VERTEX(-5, 8, -17, 55, 125, 210),
  STEVE_VERTEX(-5, 2, -17, 55, 125, 210), STEVE_VERTEX(-11, 2, -17, 55, 125, 210),
  STEVE_VERTEX(5, 8, -17, 55, 125, 210), STEVE_VERTEX(11, 8, -17, 55, 125, 210),
  STEVE_VERTEX(11, 2, -17, 55, 125, 210), STEVE_VERTEX(5, 2, -17, 55, 125, 210)
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

static Vp full_viewport = {
    SCREEN_WD*2, SCREEN_HT*2, G_MAXZ/2, 0,
    SCREEN_WD*2, SCREEN_HT*2, G_MAXZ/2, 0,
};

/* N64 viewport coordinates are 10.2 fixed point.  These two viewports fill
   the same framebuffer as solo play, so split-screen does not double fill-rate. */
static Vp coop_viewports[MAX_PLAYERS] = {
  {SCREEN_WD*2, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD*2, SCREEN_HT, G_MAXZ/2, 0},
  {SCREEN_WD*2, SCREEN_HT, G_MAXZ/2, 0, SCREEN_WD*2, SCREEN_HT*3, G_MAXZ/2, 0}
};

static Gfx setup_display_list[] = {
  gsSPSegment(0, 0x0),
  gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),
  gsDPSetScissor(G_SC_NON_INTERLACE, 0,0, SCREEN_WD,SCREEN_HT),
  gsSPEndDisplayList()
};

static Gfx draw_setup_display_list[] = {
  gsDPSetCycleType(G_CYC_1CYCLE),
  gsDPSetRenderMode(G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2),
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
  gsDPSetRenderMode(G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2),
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

void makeQuadDL(u8 chunk, u8 bx, u8 by, u8 bz, u8 width, u8 height, u8 face) {
  u32 b = bx * CHUNK_SIZE * CHUNK_SIZE + by * CHUNK_SIZE + bz;

  /* Reserve one final command for the column's EndDisplayList.  A maximally
     fragmented player-built chunk can exceed the normal greedy-mesh budget;
     dropping only excess faces is far safer than overwriting adjacent RAM. */
  if (column_display_list + DISPLAY_LIST_SIZE - column_dlp < 6) {
    column_display_list_full = TRUE;
    return;
  }
  gSPMatrix(column_dlp++,OS_K0_TO_PHYSICAL(c_models + chunk),
    G_MTX_MODELVIEW|G_MTX_LOAD|G_MTX_NOPUSH);
  gSPMatrix(column_dlp++,OS_K0_TO_PHYSICAL(b_models + b),
    G_MTX_MODELVIEW|G_MTX_MUL|G_MTX_NOPUSH);
  gSPVertex(column_dlp++, QUAD_ADDR(face, width, height), 4, 0);
  gSP1Quadrangle(column_dlp++, 3, 2, 1, 0, 0);
}

void makeQuadDLRST(u8 chunk, u8 br, u8 bs, u8 bt, u8 axes, u8 width, u8 height, u8 face) {
  if (face == NONE) {
    return;
  }

  if (axes == ZXY) {
    makeQuadDL(chunk, bs, bt, br, width, height, face);
  } else if (axes == XZY) {
    makeQuadDL(chunk, br, bt, bs, width, height, face);
  } else if (axes == YXZ) {
    makeQuadDL(chunk, bs, br, bt, width, height, face);
  }
}

void makeChunkAxisDL(DualQuadList *axis_quads, u8 chunk, u8 axes, u8 face1, u8 face2, u8 block) {
  u8 br, i;
  DualQuadList *both_quads;
  for (br = 0; br < CHUNK_SIZE; br++) {
    both_quads = &(axis_quads)[br];
    for (i = 0; i < both_quads->n_front + both_quads->n_back; i++) {
      if (both_quads->quads[i].block == block) {
        makeQuadDLRST(chunk, br, both_quads->quads[i].bs, both_quads->quads[i].bt, axes,
          both_quads->quads[i].width, both_quads->quads[i].height, i < both_quads->n_front? face1 : face2);
      }
    }
  }
}

void makeColumnDL(u8 cx, u8 cz, u8 texture) {
  u8 cy, i, chunk;
  ChunkQuads *c_quads;
  FaceSpec *faces = textures[texture]->faces;

  if (column_display_list_full ||
      column_display_list + DISPLAY_LIST_SIZE - column_dlp < 1) {
    column_display_list_full = TRUE;
    column_starts[texture][cx * CHUNKS_Z + cz] =
      empty_column_display_list;
    return;
  }
  column_starts[texture][cx * CHUNKS_Z + cz] = column_dlp;

  for (cy = 0; cy < CHUNKS_Y; cy++) {
    chunk = cx * CHUNKS_Y * CHUNKS_Z + cy * CHUNKS_Z + cz;
    c_quads = &chunk_quads[chunk];

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

void makeWorldDisplayLists() {
  u8 cx, cz, i;
  if (column_dlp == column_display_list) {
    column_display_list_full = FALSE;
  }
  for (i = 0; i < NUM_TEXTURES; i++) {
    for (cx = 0; cx < CHUNKS_X; cx++) {
      for (cz = 0; cz < CHUNKS_Z; cz++) {
        makeColumnDL(cx, cz, i);
      }
    }
  }
}

void makeDisplayListsAt(u8 x, u8 z) {
  u8 i;
  u8 cx = x / CHUNK_SIZE;
  u8 cz = z / CHUNK_SIZE;

  if ((column_display_list + DISPLAY_LIST_SIZE - column_dlp) < (DISPLAY_LIST_SIZE) / 10) {
    /*
     * Normal edits append replacement column lists, so an in-flight RSP task
     * can safely keep reading the old commands.  Compaction is the one time
     * this arena is overwritten; wait for all submitted tasks before doing it.
     */
    nuGfxTaskAllEndWait();
    column_dlp = column_display_list;
    column_display_list_full = FALSE;
    makeWorldDisplayLists();
  } else {
    for (i = 0; i < NUM_TEXTURES; i++) {
      makeColumnDL(cx, cz, i);
      if (x % CHUNK_SIZE == 0 && cx > 0) {
        makeColumnDL(cx - 1, cz, i);
      }
      if (z % CHUNK_SIZE == 0 && cz > 0) {
        makeColumnDL(cx, cz - 1, i);
      }
    }
  }
}

void drawTextured(u8 texture, u8 player_num) {
  u8 cx, cz;
  for (cx = 0; cx < CHUNKS_X; cx++) {
    for (cz = 0; cz < CHUNKS_Z; cz++) {
      if (visible_columns[player_num][cx * CHUNKS_Z + cz]) {
        gSPDisplayList(dlp++, column_starts[texture][cx * CHUNKS_Z + cz]);
      }
    }
  }
}

static void setStevePartTransform(u8 player_num, u8 part, Vector3 local_offset,
    float pitch, float yaw) {
  Player *player = &players[player_num];
  Vector3 offset = rotateY(local_offset, -player->body_yaw);

  guTranslate(&steve_translate[dl_no][player_num][part], player->position.x + offset.x,
    player->position.y + offset.y, player->position.z + offset.z);
  guRotateRPY(&steve_rotate[dl_no][player_num][part], pitch, yaw, 0);
}

static void makeStevePose(u8 player_num) {
  Player *player = &players[player_num];
  float head_pitch = player->pitch > 180 ? player->pitch - 360 : player->pitch;
  float swing = sinf(player->walk_time) * 28 * player->walk_swing;

  setStevePartTransform(player_num, STEVE_BODY, (Vector3) {0, -30, 0},
    0, -player->body_yaw);
  /* Unlike the torso, this orientation uses the current camera yaw/pitch. */
  setStevePartTransform(player_num, STEVE_HEAD, (Vector3) {0, 8, 0},
    head_pitch, -player->yaw);
  setStevePartTransform(player_num, STEVE_LEFT_ARM, (Vector3) {-25, -8, 0},
    swing, -player->body_yaw);
  setStevePartTransform(player_num, STEVE_RIGHT_ARM, (Vector3) {25, -8, 0},
    -swing, -player->body_yaw);
  setStevePartTransform(player_num, STEVE_LEFT_LEG, (Vector3) {-10, -52, 0},
    -swing, -player->body_yaw);
  setStevePartTransform(player_num, STEVE_RIGHT_LEG, (Vector3) {10, -52, 0},
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
  drawStevePart(player_num, STEVE_LEFT_LEG, steve_leg_verts, steve_box_display_list);
  drawStevePart(player_num, STEVE_RIGHT_LEG, steve_leg_verts, steve_box_display_list);
  drawStevePart(player_num, STEVE_HEAD, steve_head_verts, steve_box_display_list);

  /* The eyes share the head transform, so their direction matches its pitch
     and yaw exactly. */
  drawStevePart(player_num, STEVE_HEAD, steve_eye_verts, steve_eyes_display_list);
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
          third_person_avatar_visible[viewer_num]))) {
      drawSteve(player_num);
    }
  }
  gSPSetGeometryMode(dlp++, G_CULL_BACK);
}

static void drawDroppedItems() {
  u8 i;

  /* Small cubes are particularly sensitive to winding/culling mistakes on
     hardware.  Rendering both sides makes pickups visible from every angle. */
  gSPClearGeometryMode(dlp++, G_CULL_BACK);
  for (i = 0; i < MAX_DROPPED_ITEMS; i++) {
    DroppedItem *drop = &dropped_items[i];
    if (!drop->active) {
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

void drawWorld() {
  u8 i, player_num;

  dlp = &display_lists[dl_no][0];

  gSPDisplayList(dlp++, setup_display_list);

  clearBuffers(GPACK_RGBA5551(158, 207, 255, 1));

  for (player_num = 0; player_num < active_player_count; player_num++) {
    gSPDisplayList(dlp++, draw_setup_display_list);
    if (active_player_count == 2) {
      gSPViewport(dlp++, &coop_viewports[player_num]);
      gDPSetScissor(dlp++, G_SC_NON_INTERLACE, 0, player_num * (SCREEN_HT / 2), SCREEN_WD, (player_num + 1) * (SCREEN_HT / 2));
    } else {
      gSPViewport(dlp++, &full_viewport);
      gDPSetScissor(dlp++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WD, SCREEN_HT);
    }
    loadCameraMatrices(player_num);
    for (i = 0; i < NUM_TEXTURES; i++) {
      loadTexture(textures[i]->texture);
      drawTextured(i, player_num);
    }
    drawDroppedItems();
    if (active_player_count == 2 ||
        players[player_num].camera_mode == CAMERA_THIRD_PERSON) {
      drawOtherPlayers(player_num);
    }
  }

  gDPFullSync(dlp++);
  gSPEndDisplayList(dlp++);

  nuGfxTaskStart(&display_lists[dl_no][0],
		(s32)(dlp - display_lists[dl_no]) * sizeof (Gfx),
		NU_GFX_UCODE_F3DEX,
    NU_SC_NOSWAPBUFFER
  );
}

void drawWireframes() {
  u8 player_num;
  Gfx *line_display_list = line_display_lists[dl_no];
  dlp = line_display_list;

  gSPDisplayList(dlp++, setup_display_list);

  for (player_num = 0; player_num < active_player_count; player_num++) {
    if (!players[player_num].target_present) continue;
    if (active_player_count == 2) {
      gSPViewport(dlp++, &coop_viewports[player_num]);
      gDPSetScissor(dlp++, G_SC_NON_INTERLACE, 0, player_num * (SCREEN_HT / 2), SCREEN_WD, (player_num + 1) * (SCREEN_HT / 2));
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

  gDPFullSync(dlp++);
  gSPEndDisplayList(dlp++);

  nuGfxTaskStart(line_display_list,
		(s32)(dlp - line_display_list) * sizeof (Gfx),
		NU_GFX_UCODE_L3DEX,
    NU_SC_NOSWAPBUFFER
  );
}

static void setHudFillColor(u8 r, u8 g, u8 b);

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

/* A compact, deliberately chunky version of the Minecraft hotbar.  It uses
   the existing 16x16 block previews, keeping the HUD cheap enough for both
   split-screen players without introducing another texture atlas. */
static void drawItemIcon(u8 item, u32 x, u32 y, u32 size) {
  if (item <= BLOCK_TYPE_COUNT && preview_textures[item] != NULL) {
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

static void drawHotbar(u8 player_num, u32 y_offset) {
  u32 viewport_height = active_player_count == 2 ? SCREEN_HT / 2 : SCREEN_HT;
  u32 bar_width = HOTBAR_SLOT_COUNT * HOTBAR_SLOT_SIZE;
  u32 bar_x = (SCREEN_WD - bar_width) / 2;
  u32 bar_y = y_offset + viewport_height - HOTBAR_SLOT_SIZE - HOTBAR_MARGIN;
  u32 held_x = SCREEN_WD - HELD_PREVIEW_SIZE - HOTBAR_MARGIN;
  u32 held_y = bar_y - HELD_PREVIEW_SIZE - 4;
  u8 slot;

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);

  /* One raised rim groups all nine slots into a single familiar item bar. */
  setHudFillColor(20, 20, 20);
  gDPFillRectangle(dlp++, bar_x - 2, bar_y - 2, bar_x + bar_width + 1,
    bar_y + HOTBAR_SLOT_SIZE + 1);

  for (slot = 0; slot < HOTBAR_SLOT_COUNT; slot++) {
    u32 x = bar_x + slot * HOTBAR_SLOT_SIZE;
    u8 selected = players[player_num].selected_hotbar_slot == slot;

    setHudFillColor(selected ? 250 : 78, selected ? 250 : 78, selected ? 250 : 78);
    gDPFillRectangle(dlp++, x, bar_y, x + HOTBAR_SLOT_SIZE - 1,
      bar_y + HOTBAR_SLOT_SIZE - 1);
    setHudFillColor(selected ? 118 : 42, selected ? 118 : 42, selected ? 118 : 42);
    gDPFillRectangle(dlp++, x + 2, bar_y + 2, x + HOTBAR_SLOT_SIZE - 3,
      bar_y + HOTBAR_SLOT_SIZE - 3);
  }

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_1CYCLE);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  gDPSetCombineMode(dlp++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
  gDPSetPrimColor(dlp++, 0, 0, 255, 255, 255, 255);
  gDPSetTexturePersp(dlp++, G_TP_NONE);
  gDPSetTextureLUT(dlp++, G_TT_RGBA16);

  for (slot = 0; slot < HOTBAR_SLOT_COUNT; slot++) {
    u32 x = bar_x + slot * HOTBAR_SLOT_SIZE + (HOTBAR_SLOT_SIZE - HOTBAR_ICON_SIZE) / 2;
    u32 y = bar_y + (HOTBAR_SLOT_SIZE - HOTBAR_ICON_SIZE) / 2;
    ItemStack *stack = &players[player_num].inventory[INVENTORY_HOTBAR_START + slot];
    if (stack->count > 0) {
      drawItemIcon(stack->item, x, y, HOTBAR_ICON_SIZE);
    }
  }

  /* The enlarged current block is the first-person held-item cue. */
  if (players[player_num].inventory[INVENTORY_HOTBAR_START +
      players[player_num].selected_hotbar_slot].count > 0) {
    drawItemIcon(players[player_num].held_block, held_x, held_y, HELD_PREVIEW_SIZE);
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
    u32 y_offset = active_player_count == 2 ?
      player_num * (SCREEN_HT / 2) : 0;
    u32 viewport_height = active_player_count == 2 ?
      SCREEN_HT / 2 : SCREEN_HT;
    u32 bar_width = HOTBAR_SLOT_COUNT * HOTBAR_SLOT_SIZE;
    u32 bar_x = (SCREEN_WD - bar_width) / 2;
    u32 bar_y = y_offset + viewport_height - HOTBAR_SLOT_SIZE -
      HOTBAR_MARGIN;
    ItemStack *held_stack = &players[player_num].inventory[
      INVENTORY_HOTBAR_START + players[player_num].selected_hotbar_slot];
    u8 slot;

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
    for (slot = 0; slot < HOTBAR_SLOT_COUNT; slot++) {
      ItemStack *stack = &players[player_num].inventory[
        INVENTORY_HOTBAR_START + slot];
      drawStackCount(stack, bar_x + slot * HOTBAR_SLOT_SIZE + 2,
        bar_y + 1);
    }
  }
}

void drawHUD() {
  u8 player_num;
  Gfx *hud_display_list = hud_display_lists[dl_no];
  dlp = hud_display_list;

  gSPDisplayList(dlp++, setup_display_list);

  if (current_screen != GAME && current_screen != INVENTORY) {
    clearBuffers(GPACK_RGBA5551(0, 0, 0, 1));
  } else if (current_screen == GAME) {
    loaded_texture = NULL;
    for (player_num = 0; player_num < active_player_count; player_num++) {
      u32 y_offset = active_player_count == 2 ? player_num * (SCREEN_HT / 2) : 0;
      u32 crosshair_y = y_offset +
        (active_player_count == 2 ? SCREEN_HT / 4 : SCREEN_HT / 2);
      drawCrosshair(SCREEN_WD / 2, crosshair_y, &players[player_num]);
      drawBreakProgress(SCREEN_WD / 2, crosshair_y, &players[player_num]);
      drawHotbar(player_num, y_offset);
    }
    if (active_player_count == 2) {
      gDPSetCycleType(dlp++, G_CYC_FILL);
      gDPSetFillColor(dlp++, (GPACK_RGBA5551(0, 0, 0, 1) << 16 | GPACK_RGBA5551(0, 0, 0, 1)));
      gDPFillRectangle(dlp++, 0, SCREEN_HT / 2 - 1, SCREEN_WD - 1, SCREEN_HT / 2);
      gDPPipeSync(dlp++);
    }
    gDPPipeSync(dlp++);
  } else {
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

  gDPFullSync(dlp++);
  gSPEndDisplayList(dlp++);

  nuGfxTaskStart(hud_display_list,
		(s32)(dlp - hud_display_list) * sizeof (Gfx),
		NU_GFX_UCODE_S2DEX,
    NU_SC_SWAPBUFFER
  );
}

void draw() {
  loaded_texture = NULL;

  if (current_screen == GAME || current_screen == INVENTORY) {
    u8 player_num;
    for (player_num = 0; player_num < active_player_count; player_num++) {
      updateVisibleColumns(player_num);
      updateCameraMatrices(player_num);
    }

    drawWorld();
    drawWireframes();
  }
  drawHUD();

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

  column_dlp = column_display_list;
  column_display_list_full = FALSE;
}
