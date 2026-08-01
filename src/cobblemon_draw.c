#include "cobblemon.h"

#include <stddef.h>

#include "camera.h"
#include "graphics.h"
#include "items.h"
#include "math.h"
#include "menu.h"
#include "mods.h"
#include "world.h"

/*
 * Creature models and the battle interface.
 *
 * Models are built the way everything else alive in this game is built: a
 * handful of shaded boxes with vertex colours, no texture, no atlas and no
 * skeleton.  What is different is that a creature's boxes are not baked at
 * compile time -- they are written into a double-buffered scratch each frame
 * from the rig table and the species palette.
 *
 * That trade is deliberate.  Eighteen species times eight boxes of static
 * Vtx would be roughly ten kilobytes of RDRAM in a build with about a
 * hundred and fifty free, and it would fix every creature's proportions to
 * whatever the table said forever.  Rebuilding costs fifty-six vertex writes
 * per creature per frame, and there are never more than two on screen: it is
 * cheaper than the matrices those same boxes need, and it buys per-species
 * size and colour for nothing.
 *
 * The draw path itself is exactly the mob path -- two matrices, one
 * gSPVertex, one shared display list per box -- so a creature costs what a
 * sheep costs.
 */

/* Where creatures may be seen at all.  Shorter than the mob distance: a
   creature the player cannot walk up to and interact with is only spending
   frame budget. */
#define CREATURE_RENDER_DISTANCE (BLOCK_SIZE * 26.f)

static Mtx creature_translate[NUM_DISPLAY_LISTS][COBBLE_RENDER_SLOTS][COBBLE_MAX_PARTS];
static Mtx creature_rotate[NUM_DISPLAY_LISTS][COBBLE_RENDER_SLOTS][COBBLE_MAX_PARTS];
static Vtx creature_verts[NUM_DISPLAY_LISTS][COBBLE_RENDER_SLOTS][COBBLE_MAX_PARTS][8];

/* The same triangle order every box model in the game uses, so the vertex
   layout below is the layout drawSteve and drawMob already produce. */
static Gfx creature_box_display_list[] = {
  gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0),
  gsSP2Triangles(4, 5, 6, 0, 4, 6, 7, 0),
  gsSP2Triangles(1, 4, 7, 0, 1, 7, 2, 0),
  gsSP2Triangles(5, 0, 3, 0, 5, 3, 6, 0),
  gsSP2Triangles(5, 4, 1, 0, 5, 1, 0, 0),
  gsSP2Triangles(3, 2, 7, 0, 3, 7, 6, 0),
  gsSPEndDisplayList()
};

typedef struct {
  float walk_time;
  float lunge;    /* frames remaining in an attack lunge */
  float hurt;     /* frames remaining in a recoil */
  float faint;    /* zero, or how far through falling over */
  /* Which way an attack lunges, in local -Z units. */
  float reach;
} CreaturePose;

static void setBoxVertex(Vtx *vertex, s16 x, s16 y, s16 z, const u8 *rgb) {
  vertex->v.ob[0] = x;
  vertex->v.ob[1] = y;
  vertex->v.ob[2] = z;
  vertex->v.flag = 0;
  vertex->v.tc[0] = 0;
  vertex->v.tc[1] = 0;
  vertex->v.cn[0] = rgb[0];
  vertex->v.cn[1] = rgb[1];
  vertex->v.cn[2] = rgb[2];
  vertex->v.cn[3] = 255;
}

/*
 * Build one box.  The +Z face carries the species colour and the -Z face a
 * darkened copy, which is the whole lighting model for entities in this game:
 * there is no RSP light in the entity pass, so a box has to carry its own
 * sense of which way is toward the viewer or it reads as a flat sticker.
 */
static void buildBox(Vtx *verts, s16 x0, s16 y0, s16 z0, s16 x1, s16 y1,
    s16 z1, const u8 *color) {
  u8 dark[3];

  dark[0] = (u8) (((u32) color[0] * 180) / 255);
  dark[1] = (u8) (((u32) color[1] * 180) / 255);
  dark[2] = (u8) (((u32) color[2] * 180) / 255);
  setBoxVertex(&verts[0], x0, y1, z1, color);
  setBoxVertex(&verts[1], x1, y1, z1, color);
  setBoxVertex(&verts[2], x1, y0, z1, color);
  setBoxVertex(&verts[3], x0, y0, z1, color);
  setBoxVertex(&verts[4], x1, y1, z0, dark);
  setBoxVertex(&verts[5], x0, y1, z0, dark);
  setBoxVertex(&verts[6], x0, y0, z0, dark);
  setBoxVertex(&verts[7], x1, y0, z0, dark);
}

static const u8 *toneColor(const CobbleSpecies *species, u8 tone) {
  if (tone == COBBLE_TONE_SECONDARY) {
    return species->secondary;
  }
  if (tone == COBBLE_TONE_ACCENT) {
    return species->accent;
  }
  return species->primary;
}

/*
 * A pose is three sine waves and an offset.
 *
 * Parts move; nothing rotates.  That is the same choice the sheep and the pig
 * made, and it is the reason a creature costs two matrices per box instead of
 * three: the yaw matrix is shared by every box and the pose is folded into
 * the translation the box was going to need anyway.
 */
static void poseOffset(const CobblePart *part, const CreaturePose *pose,
    u8 scale, float *out_x, float *out_y, float *out_z) {
  float step = sinf(pose->walk_time) * 3.f;
  float breathe = sinf(pose->walk_time * .3f) * 1.2f;
  float shake = pose->hurt > 0 ?
    sinf(pose->hurt * 90.f * M_DTOR) * 4.f : 0;
  float x = (float) part->x * scale / 100.f;
  float y = (float) part->y * scale / 100.f;
  float z = (float) part->z * scale / 100.f;

  switch (part->role) {
    case COBBLE_ROLE_BODY:
      y += breathe;
      x += shake;
      break;
    case COBBLE_ROLE_HEAD:
      y += breathe * 1.4f;
      x += shake;
      break;
    case COBBLE_ROLE_LEG_A:
    case COBBLE_ROLE_ARM_A:
      y += step;
      z -= step * .4f;
      break;
    case COBBLE_ROLE_LEG_B:
    case COBBLE_ROLE_ARM_B:
      y -= step;
      z += step * .4f;
      break;
    case COBBLE_ROLE_TAIL:
      x += sinf(pose->walk_time * .8f) * 4.f;
      y += breathe;
      break;
    case COBBLE_ROLE_WING:
      y += sinf(pose->walk_time * 1.7f) * 5.f;
      break;
    default:
      break;
  }
  /* An attack is the whole creature leaning in, which reads at this scale
     where a swinging limb would not. */
  if (pose->lunge > 0) {
    z -= pose->reach * (pose->lunge / 16.f);
  }
  /* Fainting sinks the model.  There is no ragdoll to fall over with, and a
     creature that simply vanished would make a knockout feel like a bug. */
  if (pose->faint > 0) {
    y -= pose->faint * 2.f;
  }
  *out_x = x;
  *out_y = y;
  *out_z = z;
}

/*
 * One creature into the display list.
 *
 * `slot` selects a render slot's matrices and vertex scratch, and both are
 * double-buffered by dl_no like every other RSP-referenced structure in the
 * game: the RSP may still be walking last frame's copy while this one is
 * written.
 */
static void drawCreature(u8 slot, u8 species_id, Vector3 position, float yaw,
    const CreaturePose *pose) {
  const CobbleSpecies *species;
  const CobbleRig *rig;
  u8 part_index;

  if (species_id >= COBBLE_SPECIES_COUNT || slot >= COBBLE_RENDER_SLOTS) {
    return;
  }
  species = &cobble_species[species_id];
  rig = &cobble_rigs[species->rig];

  for (part_index = 0; part_index < rig->part_count; part_index++) {
    const CobblePart *part = &rig->parts[part_index];
    Vtx *verts = creature_verts[dl_no][slot][part_index];
    const u8 *color = toneColor(species, part->tone);
    float ox;
    float oy;
    float oz;
    Vector3 offset;
    s16 sx = (s16) ((part->sx * species->scale) / 100);
    s16 sy = (s16) ((part->sy * species->scale) / 100);
    s16 sz = (s16) ((part->sz * species->scale) / 100);

    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;
    if (sz < 1) sz = 1;
    buildBox(verts, (s16) -sx, (s16) -sy, (s16) -sz, sx, sy, sz, color);

    poseOffset(part, pose, species->scale, &ox, &oy, &oz);
    offset.x = ox;
    offset.y = oy;
    offset.z = oz;
    offset = rotateY(offset, -yaw);
    guTranslate(&creature_translate[dl_no][slot][part_index],
      position.x + offset.x - render_origin_units_x,
      position.y + offset.y,
      position.z + offset.z - render_origin_units_z);
    guRotateRPY(&creature_rotate[dl_no][slot][part_index], 0, -yaw, 0);

    gSPMatrix(dlp++,
      OS_K0_TO_PHYSICAL(&creature_translate[dl_no][slot][part_index]),
      G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPMatrix(dlp++,
      OS_K0_TO_PHYSICAL(&creature_rotate[dl_no][slot][part_index]),
      G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
    gSPVertex(dlp++, verts, 8, 0);
    gSPDisplayList(dlp++, creature_box_display_list);
  }
}

/* PRIM * SHADE, the combiner every untextured entity in the game uses.  The
   day/night tint is already loaded as the primitive colour by the caller. */
static void setCreatureCombine(void) {
  gDPSetCombineLERP(dlp++, PRIMITIVE, 0, SHADE, 0, 0, 0, 0, SHADE,
    PRIMITIVE, 0, SHADE, 0, 0, 0, 0, SHADE);
}

static u8 creatureVisible(u8 viewer_num, Vector3 point) {
  Vector3 offset = add(point, mul(players[viewer_num].position, -1.f));
  int cx = floor(point.x / (BLOCK_SIZE * CHUNK_SIZE));
  int cz = floor(point.z / (BLOCK_SIZE * CHUNK_SIZE));

  if (dot(offset, offset) >
      CREATURE_RENDER_DISTANCE * CREATURE_RENDER_DISTANCE) {
    return FALSE;
  }
  return windowColumnResident(cx, cz) &&
    visible_columns[viewer_num][WINDOW_SLOT(cx, cz)];
}

/*
 * Which way a battler faces.
 *
 * The scene was laid out along one axis when the battle opened, so facing is
 * that axis and its reverse -- exact, rather than the four-way snap the
 * wandering creatures use, and correct however the player was standing.
 */
static float battleFacing(u8 side) {
  return side == 0 ? cobble_battle.facing : cobble_battle.facing + 180.f;
}

static void drawBattleCreatures(u8 viewer_num) {
  u8 side;

  for (side = 0; side < 2; side++) {
    CreaturePose pose;
    CobbleFighter *fighter = &cobble_battle.fighter[side];

    if (fighter->species >= COBBLE_SPECIES_COUNT) {
      continue;
    }
    if (!creatureVisible(viewer_num, cobble_battle.stand[side])) {
      continue;
    }
    pose.walk_time = cobble_battle.scene_time * .06f;
    pose.lunge = cobble_battle.lunge[side];
    pose.hurt = cobble_battle.hurt[side];
    pose.faint = cobble_battle.faint[side] > 0 ?
      cobble_battle.faint[side] : 0;
    pose.reach = BLOCK_SIZE * .7f;
    drawCreature(side, fighter->species, cobble_battle.stand[side],
      battleFacing(side), &pose);
  }
}

static void drawRoamers(u8 viewer_num) {
  u8 drawn[COBBLE_MAX_ROAMERS];
  u8 index;
  u8 slot;

  for (index = 0; index < COBBLE_MAX_ROAMERS; index++) {
    drawn[index] = FALSE;
  }

  /*
   * Nearest first, capped at the render slots.  The cap is the point: a
   * creature the player has walked past is never worth the frame time of the
   * one they are walking toward, and the pool is small enough that picking
   * the nearest twice is cheaper than sorting it.
   */
  for (slot = 0; slot < COBBLE_RENDER_SLOTS; slot++) {
    u8 best = COBBLE_MAX_ROAMERS;
    float best_distance = 0;
    CreaturePose pose;

    for (index = 0; index < COBBLE_MAX_ROAMERS; index++) {
      CobbleRoamer *roamer = &cobble_roamers[index];
      Vector3 offset;
      float distance;

      if (drawn[index] || !roamer->active ||
          !creatureVisible(viewer_num, roamer->position)) {
        continue;
      }
      offset = add(roamer->position, mul(players[viewer_num].position, -1.f));
      distance = dot(offset, offset);
      if (best == COBBLE_MAX_ROAMERS || distance < best_distance) {
        best = index;
        best_distance = distance;
      }
    }
    if (best == COBBLE_MAX_ROAMERS) {
      break;
    }
    drawn[best] = TRUE;
    pose.walk_time = cobble_roamers[best].walk_time;
    pose.lunge = 0;
    pose.hurt = 0;
    pose.faint = 0;
    pose.reach = 0;
    drawCreature(slot, cobble_roamers[best].species,
      cobble_roamers[best].position, cobble_roamers[best].yaw, &pose);
  }
}

void cobblemonDrawForPlayer(u8 viewer_num) {
  if (!cobblemonEnabled()) {
    return;
  }
  gSPTexture(dlp++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
  setCreatureCombine();
  /* The box models have deliberately minimal geometry; culling off keeps
     every face reliable from any angle, exactly as the mob pass does. */
  gSPClearGeometryMode(dlp++, G_CULL_BACK);
  if (cobble_battle.active) {
    drawBattleCreatures(viewer_num);
  } else {
    drawRoamers(viewer_num);
  }
  gSPSetGeometryMode(dlp++, G_CULL_BACK);
}

/* ------------------------------------------------------------------ */
/* The interface.                                                           */
/* ------------------------------------------------------------------ */

/*
 * Every panel here follows the card rule the rest of the game's menus
 * follow: all fill rectangles first, one gDPPipeSync, then all text.  Mixing
 * the two is the RDP hazard that locks an actual console, and it is the only
 * thing in this file that hardware punishes and an emulator forgives.
 */

#define BATTLE_BOX_LEFT 8
#define BATTLE_BOX_RIGHT 312
#define BATTLE_BOX_TOP 162
#define BATTLE_BOX_BOTTOM 232
#define BATTLE_TEXT_X 18

static void fillColor(u8 r, u8 g, u8 b) {
  u16 color = GPACK_RGBA5551(r, g, b, 1);

  gDPSetFillColor(dlp++, (color << 16) | color);
}

static void textColor(u8 r, u8 g, u8 b) {
  gDPSetPrimColor(dlp++, 0, 0, r, g, b, 255);
}

/* The same stone-and-shadow frame the setup card and the pack use, so the
   battle looks like part of the game rather than a mode bolted onto it. */
static void drawFramedPanel(u32 x0, u32 y0, u32 x1, u32 y1) {
  fillColor(50, 54, 47);
  gDPFillRectangle(dlp++, x0, y0, x1, y1);
  fillColor(143, 148, 133);
  gDPFillRectangle(dlp++, x0, y0, x1, y0 + 2);
  gDPFillRectangle(dlp++, x0, y0, x0 + 2, y1);
  fillColor(24, 27, 23);
  gDPFillRectangle(dlp++, x0, y1 - 2, x1, y1);
  gDPFillRectangle(dlp++, x1 - 2, y0, x1, y1);
}

/*
 * A health bar that changes colour rather than one that only shrinks.
 *
 * On a composite CRT at 320 pixels the difference between a third of a bar
 * and a fifth is not reliably readable, but green against red is, from the
 * other side of a room.  The thresholds are the traditional half and fifth.
 */
static void drawHealthBar(u32 x0, u32 y0, u32 width, u16 hp, u16 max_hp) {
  u32 filled;

  fillColor(24, 27, 23);
  gDPFillRectangle(dlp++, x0 - 1, y0 - 1, x0 + width + 1, y0 + 7);
  fillColor(70, 74, 66);
  gDPFillRectangle(dlp++, x0, y0, x0 + width, y0 + 6);
  if (max_hp == 0 || hp == 0) {
    return;
  }
  filled = ((u32) hp * width) / max_hp;
  if (filled == 0) {
    filled = 1;
  }
  if (hp * 2 > max_hp) {
    fillColor(96, 198, 88);
  } else if (hp * 5 > max_hp) {
    fillColor(232, 196, 79);
  } else {
    fillColor(216, 74, 58);
  }
  gDPFillRectangle(dlp++, x0, y0, x0 + filled, y0 + 6);
}

static void drawTypeSwatch(u32 x, u32 y, u8 type) {
  const u8 *color = cobble_type_color[type > COBBLE_TYPE_COUNT ?
    COBBLE_TYPE_COUNT : type];

  fillColor(24, 27, 23);
  gDPFillRectangle(dlp++, x, y, x + 8, y + 8);
  fillColor(color[0], color[1], color[2]);
  gDPFillRectangle(dlp++, x + 1, y + 1, x + 7, y + 7);
}

/* Small unsigned numbers, drawn through the menu font so they match every
   other number the game shows. */
static u32 drawNumber(u32 value, u32 x, u32 y) {
  char digits[6];
  u8 count = 0;

  do {
    digits[count++] = (char) ('0' + (value % 10u));
    value /= 10u;
  } while (value > 0 && count < 6);
  while (count > 0) {
    char digit = digits[--count];
    drawChar(digit, x, y);
    x += charWidth(digit);
  }
  return x;
}

static u32 drawLevel(u8 level, u32 x, u32 y) {
  drawString("LV", x, y);
  return drawNumber(level, x + 16, y);
}

/* The command grid, and the move grid, share a cursor style: a filled cell
   plus a caret.  Two cues, because a colour-only highlight disappears on a
   badly tuned television. */
static void drawGridCell(u32 x, u32 y, u32 width, u8 selected) {
  if (!selected) {
    return;
  }
  fillColor(216, 191, 77);
  gDPFillRectangle(dlp++, x - 4, y - 3, x + width, y + 10);
  fillColor(143, 122, 43);
  gDPFillRectangle(dlp++, x - 4, y + 8, x + width, y + 10);
}

static const char *command_labels[4] = {"FIGHT", "TEAM", "BAG", "RUN"};

/* Grid geometry for the four commands and the four moves. */
static u32 gridX(u8 index) {
  return (index & 1) ? 216u : 122u;
}

static u32 gridY(u8 index) {
  return (index & 2) ? 202u : 178u;
}

/* The move grid keeps to the left two thirds; the last third is the strip
   that describes whichever move is highlighted, and the two must not
   overlap or a selected cell paints over the numbers it is about. */
#define MOVE_CELL_WIDTH 96
#define MOVE_INFO_X 238

static u32 moveGridX(u8 index) {
  return (index & 1) ? 130u : 22u;
}

static u32 moveGridY(u8 index) {
  return (index & 2) ? 202u : 178u;
}

static u8 battleBagItem(u8 index) {
  return index == 0 ? SLIME_GEL : APPLE;
}

static u8 bagItemCount(u8 player_num, u8 item) {
  u8 index;
  u16 total = 0;

  for (index = 0; index < INVENTORY_SIZE; index++) {
    if (players[player_num].inventory[index].item == item) {
      total = (u16) (total + players[player_num].inventory[index].count);
    }
  }
  return total > 99 ? 99 : (u8) total;
}

static void drawBattleFills(void) {
  u8 phase = cobble_battle.phase;
  u8 side = cobble_battle.acting_side;
  CobbleFighter *mine = &cobble_battle.fighter[side];
  CobbleFighter *theirs = &cobble_battle.fighter[side ^ 1];

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);

  /*
   * Both status panels sit along the top, opponent left and your own right.
   *
   * The diagonal arrangement a handheld uses does not survive here: the
   * fight is happening in the world, the creatures stand on the ground a few
   * blocks away, and anything placed in the middle of the screen is placed
   * exactly where they are.  Keeping the panels in one band leaves the whole
   * strip between them and the message box clear for the battle itself.
   */
  drawFramedPanel(10, 14, 152, 58);
  drawHealthBar(18, 34, 124, theirs->hp, theirs->max_hp);
  drawFramedPanel(168, 14, 310, 58);
  drawHealthBar(176, 34, 124, mine->hp, mine->max_hp);

  drawFramedPanel(BATTLE_BOX_LEFT, BATTLE_BOX_TOP, BATTLE_BOX_RIGHT,
    BATTLE_BOX_BOTTOM);

  switch (phase) {
    case COBBLE_PHASE_COMMAND: {
      u8 index;
      /* A divider, so the message half and the command half do not read as
         one long row of words. */
      fillColor(36, 39, 34);
      gDPFillRectangle(dlp++, 112, BATTLE_BOX_TOP + 6, 114,
        BATTLE_BOX_BOTTOM - 6);
      for (index = 0; index < 4; index++) {
        drawGridCell(gridX(index), gridY(index), 76,
          index == cobble_battle.command_cursor);
      }
      break;
    }

    case COBBLE_PHASE_MOVE: {
      u8 index;

      for (index = 0; index < mine->move_count; index++) {
        drawGridCell(moveGridX(index), moveGridY(index), MOVE_CELL_WIDTH,
          index == cobble_battle.move_cursor);
      }
      if (mine->move_count > 0) {
        drawTypeSwatch(MOVE_INFO_X + 44, 178,
          cobble_moves[mine->move[cobble_battle.move_cursor]].type);
      }
      break;
    }

    case COBBLE_PHASE_TEAM: {
      u8 index;
      u8 player_num = cobble_battle.side_player[side];

      for (index = 0; index < COBBLE_PARTY_SIZE; index++) {
        CobbleMon *mon = &cobble_party[player_num][index];
        u32 row_y = BATTLE_BOX_TOP + 8 + index * 10;

        if (index == cobble_battle.team_cursor) {
          fillColor(216, 191, 77);
          gDPFillRectangle(dlp++, 14, row_y - 2, 306, row_y + 8);
        }
        if (mon->species == COBBLE_NONE) {
          continue;
        }
        drawHealthBar(200, row_y, 60, mon->hp,
          cobbleMaxHealth(mon->species, mon->level));
      }
      break;
    }

    case COBBLE_PHASE_BAG: {
      u8 index;

      for (index = 0; index < 2; index++) {
        u32 row_y = BATTLE_BOX_TOP + 12 + index * 20;
        if (index == cobble_battle.bag_cursor) {
          fillColor(216, 191, 77);
          gDPFillRectangle(dlp++, 14, row_y - 3, 306, row_y + 9);
        }
      }
      break;
    }

    case COBBLE_PHASE_CATCH: {
      /* Three lamps: one lights per successful shake, so the wobble the
         player is waiting through is visible rather than implied. */
      u8 index;
      u8 lit = (u8) (cobble_battle.catch_time / 20.f);

      for (index = 0; index < 3; index++) {
        u32 x = 140 + index * 16;
        fillColor(24, 27, 23);
        gDPFillRectangle(dlp++, x, 196, x + 12, 208);
        if (index < lit && index < cobble_battle.catch_shakes) {
          fillColor(232, 196, 79);
        } else {
          fillColor(70, 74, 66);
        }
        gDPFillRectangle(dlp++, x + 2, 198, x + 10, 206);
      }
      break;
    }

    default:
      break;
  }
  gDPPipeSync(dlp++);
}

static void drawBattleText(void) {
  u8 phase = cobble_battle.phase;
  u8 side = cobble_battle.acting_side;
  CobbleFighter *mine = &cobble_battle.fighter[side];
  CobbleFighter *theirs = &cobble_battle.fighter[side ^ 1];

  beginText();

  textColor(240, 226, 198);
  if (theirs->species < COBBLE_SPECIES_COUNT) {
    drawString(cobble_species[theirs->species].name, 18, 20);
    drawLevel(theirs->level, 112, 20);
  }
  if (mine->species < COBBLE_SPECIES_COUNT) {
    drawString(cobble_species[mine->species].name, 176, 20);
    drawLevel(mine->level, 270, 20);
    /* The player's own numbers are shown exactly; the opponent's are not,
       which is the difference between judging your own risk and being told
       the answer to the fight. */
    textColor(200, 204, 190);
    {
      u32 x = drawNumber(mine->hp, 250, 46);
      drawChar('/', x, 46);
      drawNumber(mine->max_hp, x + charWidth('/'), 46);
    }
  }

  switch (phase) {
    case COBBLE_PHASE_MESSAGE:
    case COBBLE_PHASE_CATCH: {
      const char *line = cobble_battle.message[cobble_battle.message_head];
      u32 revealed = cobble_battle.message_reveal / 4u;
      u32 x = BATTLE_TEXT_X;
      u32 index = 0;

      textColor(240, 226, 198);
      while (line[index] && index < revealed) {
        if (line[index] != ' ') {
          drawChar(line[index], x, 178);
        }
        x += charWidth(line[index]);
        index++;
      }
      if (cobble_battle.message_count > 0 && line[index] == 0) {
        textColor(150, 155, 142);
        drawString("A CONTINUE", BATTLE_TEXT_X, 210);
      }
      break;
    }

    case COBBLE_PHASE_COMMAND: {
      u8 index;

      textColor(240, 226, 198);
      drawString("WHAT WILL", 18, 178);
      if (mine->species < COBBLE_SPECIES_COUNT) {
        drawString(cobble_species[mine->species].name, 18, 192);
      }
      drawString("DO?", 18, 206);
      for (index = 0; index < 4; index++) {
        /* Brighter, not darker.  The font draws each glyph cell with an
           opaque background, so dark text on the yellow bar comes out as a
           black block with a black letter inside it. */
        textColor(index == cobble_battle.command_cursor ? 255 : 176,
          index == cobble_battle.command_cursor ? 244 : 180,
          index == cobble_battle.command_cursor ? 214 : 168);
        drawString(command_labels[index], gridX(index), gridY(index));
      }
      break;
    }

    case COBBLE_PHASE_MOVE: {
      u8 index;

      for (index = 0; index < mine->move_count; index++) {
        u8 selected = index == cobble_battle.move_cursor;
        textColor(selected ? 255 : 176, selected ? 244 : 180,
          selected ? 214 : 168);
        drawString(cobble_moves[mine->move[index]].name, moveGridX(index),
          moveGridY(index));
      }
      if (mine->move_count > 0) {
        const CobbleMove *move =
          &cobble_moves[mine->move[cobble_battle.move_cursor]];

        /* Type and remaining uses of whatever is highlighted: the two
           numbers the choice actually turns on. */
        textColor(200, 204, 190);
        drawString(cobble_type_name[move->type > COBBLE_TYPE_COUNT ?
          COBBLE_TYPE_COUNT : move->type], MOVE_INFO_X, 178);
        drawString("PP", MOVE_INFO_X, 194);
        {
          u32 x = drawNumber(mine->pp[cobble_battle.move_cursor],
            MOVE_INFO_X + 18, 194);
          drawChar('/', x, 194);
          drawNumber(move->pp, x + charWidth('/'), 194);
        }
      }
      textColor(150, 155, 142);
      drawString("B BACK", MOVE_INFO_X, 210);
      break;
    }

    case COBBLE_PHASE_TEAM: {
      u8 index;
      u8 player_num = cobble_battle.side_player[side];

      for (index = 0; index < COBBLE_PARTY_SIZE; index++) {
        CobbleMon *mon = &cobble_party[player_num][index];
        u32 row_y = BATTLE_BOX_TOP + 8 + index * 10;
        u8 selected = index == cobble_battle.team_cursor;

        textColor(selected ? 255 : 176, selected ? 244 : 180,
          selected ? 214 : 168);
        if (mon->species == COBBLE_NONE) {
          drawString("-", 20, row_y);
          continue;
        }
        drawString(cobble_species[mon->species].name, 20, row_y);
        drawLevel(mon->level, 116, row_y);
        if (mon->hp == 0) {
          drawString("DOWN", 160, row_y);
        }
      }
      break;
    }

    case COBBLE_PHASE_BAG: {
      u8 index;
      u8 player_num = cobble_battle.side_player[side];

      for (index = 0; index < 2; index++) {
        u8 item = battleBagItem(index);
        u32 row_y = BATTLE_BOX_TOP + 12 + index * 20;
        u8 selected = index == cobble_battle.bag_cursor;

        textColor(selected ? 255 : 176, selected ? 244 : 180,
          selected ? 214 : 168);
        drawString(item == SLIME_GEL ? "SLIME GEL" : "APPLE", 20, row_y);
        drawNumber(bagItemCount(player_num, item), 130, row_y);
        drawString(item == SLIME_GEL ? "THROW TO CATCH" : "HEALS 20",
          170, row_y);
      }
      textColor(150, 155, 142);
      drawString("B BACK", 246, 216);
      break;
    }

    default:
      break;
  }
  textColor(255, 255, 255);
}

void cobblemonDrawBattleInterface(void) {
  if (!cobble_battle.active) {
    return;
  }
  drawBattleFills();
  drawBattleText();
}

/*
 * The overworld prompt.
 *
 * One line, above the hotbar, naming what is in front of the player and what
 * A would do with it.  This is the entire encounter interface: the game never
 * takes the player out of what they were doing, so the prompt has to carry
 * both the invitation and the species, or walking past a creature you wanted
 * becomes a thing that happens.
 */
void cobblemonDrawPrompt(u8 player_num, u32 center_x, u32 bottom_y) {
  u8 target;
  CobbleRoamer *roamer;
  const char *label;
  u32 width;
  u32 left;
  u32 top;
  u32 x;

  if (!cobblemonEnabled() || cobble_battle.active) {
    return;
  }
  target = cobblemonTargetRoamer(player_num);
  if (target == COBBLE_NONE) {
    return;
  }
  roamer = &cobble_roamers[target];
  if (roamer->species >= COBBLE_SPECIES_COUNT) {
    return;
  }
  label = roamer->kind == COBBLE_ROAMER_TRAINER ? "TRAINER" :
    cobble_species[roamer->species].name;

  /* Sized to its contents rather than fixed, so a four-player viewport gets
     a badge that fits inside it and a nine-letter species still does. */
  width = 30;
  for (x = 0; label[x]; x++) {
    width += charWidth(label[x]);
  }
  if (roamer->kind != COBBLE_ROAMER_TRAINER) {
    width += 32;
  }
  left = center_x > width / 2 ? center_x - width / 2 : 0;
  top = bottom_y > 16 ? bottom_y - 16 : 0;

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  fillColor(24, 27, 23);
  gDPFillRectangle(dlp++, left, top, left + width, top + 16);
  fillColor(50, 54, 47);
  gDPFillRectangle(dlp++, left + 1, top + 1, left + width - 1, top + 15);
  drawTypeSwatch(left + 4, top + 4, cobble_species[roamer->species].type);
  gDPPipeSync(dlp++);

  beginText();
  textColor(240, 226, 198);
  x = left + 16;
  drawChar('A', x, top + 4);
  x += charWidth('A') + 3;
  drawString(label, x, top + 4);
  if (roamer->kind != COBBLE_ROAMER_TRAINER) {
    x += 4;
    while (*label) {
      x += charWidth(*label++);
    }
    drawLevel(roamer->level, x, top + 4);
  }
  textColor(255, 255, 255);
}
