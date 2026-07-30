#include <nusys.h>
#include <assert.h>
#include "graphics.h"
#include "geometry.h"
#include "camera.h"
#include "player.h"
#include "menu.h"
#include "quads.h"
#include "cube.h"
#include "textures.h"

#define CROSSHAIR_SIZE 10

Gfx *dlp;
u32 dl_no = 0;

Gfx column_display_list[DISPLAY_LIST_SIZE];
Gfx *column_dlp;
Gfx *column_starts[NUM_TEXTURES][CHUNKS_X * CHUNKS_Z];

#define BLOCKS_PER_CHUNK (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)

static Mtx c_models[NUM_BLOCKS / BLOCKS_PER_CHUNK];
static Mtx b_models[CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE];

static Mtx marker_model;

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

static Mtx steve_translate[MAX_PLAYERS][STEVE_PART_COUNT];
static Mtx steve_rotate[MAX_PLAYERS][STEVE_PART_COUNT];

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

static OSTime lt;

static u8 render_x;
static u8 render_y;
static u8 render_z;

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

static Gfx crosshair_display_list[] = {
  gsDPSetCycleType(G_CYC_FILL),
  gsDPSetRenderMode (G_RM_NOOP, G_RM_NOOP2),
  gsDPSetFillColor((GPACK_RGBA5551(255, 255, 255, 1) << 16 | 
				GPACK_RGBA5551(255, 255, 255, 1))),
  gsDPFillRectangle((SCREEN_WD - CROSSHAIR_SIZE) / 2, SCREEN_HT / 2 - 1,
        (SCREEN_WD + CROSSHAIR_SIZE) / 2 - 1, SCREEN_HT / 2),
  gsDPFillRectangle(SCREEN_WD / 2 - 1, (SCREEN_HT - CROSSHAIR_SIZE) / 2,
        SCREEN_WD / 2, (SCREEN_HT + CROSSHAIR_SIZE) / 2 - 1),
  gsDPPipeSync(),
  gsDPSetCycleType(G_CYC_1CYCLE),
  gsDPSetRenderMode(G_RM_NOOP, G_RM_NOOP2),
  gsDPSetCombineMode(G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM),
  gsDPSetPrimColor(0,0,255,255,255,255),
  gsDPSetTexturePersp(G_TP_NONE),
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
    column_dlp = column_display_list;
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

  guTranslate(&steve_translate[player_num][part], player->position.x + offset.x,
    player->position.y + offset.y, player->position.z + offset.z);
  guRotateRPY(&steve_rotate[player_num][part], pitch, yaw, 0);
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
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&steve_translate[player_num][part]),
    G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&steve_rotate[player_num][part]),
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
    if (player_num != viewer_num && players[player_num].active) {
      drawSteve(player_num);
    }
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
    if (active_player_count == 2) {
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
  }

  gDPFullSync(dlp++);
  gSPEndDisplayList(dlp++);

  nuGfxTaskStart(line_display_list,
		(s32)(dlp - line_display_list) * sizeof (Gfx),
		NU_GFX_UCODE_L3DEX,
    NU_SC_NOSWAPBUFFER
  );
}

static void drawCrosshair(u32 x, u32 y) {
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  gDPSetFillColor(dlp++, (GPACK_RGBA5551(255, 255, 255, 1) << 16 | GPACK_RGBA5551(255, 255, 255, 1)));
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

void drawHUD() {
  u8 player_num;
  dlp = hud_display_list;

  gSPDisplayList(dlp++, setup_display_list);

  if (current_screen != GAME) {
    clearBuffers(GPACK_RGBA5551(0, 0, 0, 1));
  } else {
    loaded_texture = NULL;
    for (player_num = 0; player_num < active_player_count; player_num++) {
      u32 y_offset = active_player_count == 2 ? player_num * (SCREEN_HT / 2) : 0;
      drawCrosshair(SCREEN_WD / 2, y_offset + (active_player_count == 2 ? SCREEN_HT / 4 : SCREEN_HT / 2));
      loadTexture(preview_textures[players[player_num].held_block]);
      gSPTextureRectangle(dlp++,
        10 << 2, (10 + y_offset) << 2,
        (26 << 2) - 2, ((26 + y_offset) << 2) - 2,
        G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);
    }
    if (active_player_count == 2) {
      gDPSetCycleType(dlp++, G_CYC_FILL);
      gDPSetFillColor(dlp++, (GPACK_RGBA5551(0, 0, 0, 1) << 16 | GPACK_RGBA5551(0, 0, 0, 1)));
      gDPFillRectangle(dlp++, 0, SCREEN_HT / 2 - 1, SCREEN_WD - 1, SCREEN_HT / 2);
      gDPPipeSync(dlp++);
    }
    gDPPipeSync(dlp++);
  }
  drawMenu();

  gDPFullSync(dlp++);
  gSPEndDisplayList(dlp++);

  nuGfxTaskStart(hud_display_list,
		(s32)(dlp - hud_display_list) * sizeof (Gfx),
		NU_GFX_UCODE_S2DEX,
    osTvType == OS_TV_NTSC? NU_SC_NOSWAPBUFFER : NU_SC_SWAPBUFFER
  );
}

void draw() {
  char conbuf[20];
  OSTime t;
  loaded_texture = NULL;

  if (current_screen == GAME) {
    u8 player_num;
    for (player_num = 0; player_num < active_player_count; player_num++) {
      updateVisibleColumns(player_num);
      updateCameraMatrices(player_num);
    }

    drawWorld();
    drawWireframes();
  }
  drawHUD();

  if (osTvType == OS_TV_NTSC) {
    // nuDebTaskPerfBar0EX2(4, 20, NU_SC_NOSWAPBUFFER);
    // t = osGetTime();

    // nuDebConTextPos(0,0,0);
    // sprintf(conbuf,"%llu", 1000000 / OS_CYCLES_TO_USEC(t - lt));
    // nuDebConCPuts(0, conbuf);

    // lt = t;
      
    nuDebConDisp(NU_SC_SWAPBUFFER);
  }

  /* Switch display list buffers */
  dl_no ^= 1;
}

void initGraphics() {
  int x, y, z, i;

  render_x = 0;
  render_y = 0;
  render_z = 0;

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
}
