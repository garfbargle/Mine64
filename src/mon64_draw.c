#include "mon64.h"

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
 *
 * It draws one thing that is not a creature.  A trainer is another player, so
 * they are a rig and a palette like everything else here, but the rig is
 * Steve's measurements and the head wears Steve's own face sheet: see
 * drawTrainer.  That is the whole reason drawRig takes a rig and a skin
 * rather than a species id.
 */

/* Where creatures may be seen at all.  Shorter than the mob distance: a
   creature the player cannot walk up to and interact with is only spending
   frame budget. */
#define CREATURE_RENDER_DISTANCE (BLOCK_SIZE * 26.f)

static Mtx creature_translate[NUM_DISPLAY_LISTS][MON_RENDER_SLOTS][MON_MAX_PARTS];
static Mtx creature_rotate[NUM_DISPLAY_LISTS][MON_RENDER_SLOTS][MON_MAX_PARTS];
static Vtx creature_verts[NUM_DISPLAY_LISTS][MON_RENDER_SLOTS][MON_MAX_PARTS][8];

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
 * Build one box.  One face carries the palette colour and the opposite face a
 * darkened copy, which is the whole lighting model for entities in this game:
 * there is no RSP light in the entity pass, so a box has to carry its own
 * sense of which way is toward the viewer or it reads as a flat sticker.
 *
 * `front_lit` picks which of the two the -Z face gets.  Creatures put the
 * shadow on their front, which is what gives an animal walking at you a
 * darker face than its flank.  Steve does the opposite, and anything wearing
 * his face sheet has to agree with it: the sheet's own colours are authored
 * for a lit head, so a shaded one would show a bright face on a dark skull.
 */
static void buildBox(Vtx *verts, s16 x0, s16 y0, s16 z0, s16 x1, s16 y1,
    s16 z1, const u8 *color, u8 front_lit) {
  u8 shaded[3];
  const u8 *front;
  const u8 *back;

  shaded[0] = (u8) (((u32) color[0] * 180) / 255);
  shaded[1] = (u8) (((u32) color[1] * 180) / 255);
  shaded[2] = (u8) (((u32) color[2] * 180) / 255);
  front = front_lit ? color : (const u8 *) shaded;
  back = front_lit ? (const u8 *) shaded : color;
  setBoxVertex(&verts[0], x0, y1, z1, back);
  setBoxVertex(&verts[1], x1, y1, z1, back);
  setBoxVertex(&verts[2], x1, y0, z1, back);
  setBoxVertex(&verts[3], x0, y0, z1, back);
  setBoxVertex(&verts[4], x1, y1, z0, front);
  setBoxVertex(&verts[5], x0, y1, z0, front);
  setBoxVertex(&verts[6], x0, y0, z0, front);
  setBoxVertex(&verts[7], x1, y0, z0, front);
}

/*
 * What a box needs that the rig does not carry: the three colours its tone
 * selects, and the size the rig is drawn at.  A species fills this in from
 * the roster and a trainer from a look, which is what lets one draw path
 * serve both without the trainer needing a species to impersonate.
 */
typedef struct {
  const u8 *primary;
  const u8 *secondary;
  const u8 *accent;
  /* Only a trainer has one; a species points this at a colour it already
     uses, because no box in the roster ever asks for it. */
  const u8 *hair;
  u8 scale;
  u8 bulk;
  u8 maturity;
  /* See buildBox: a rig that wears the face sheet is lit like Steve. */
  u8 front_lit;
} CreatureSkin;

static const u8 *toneColor(const CreatureSkin *skin, u8 tone) {
  if (tone == MON_TONE_HAIR) {
    return skin->hair;
  }
  if (tone == MON_TONE_SECONDARY) {
    return skin->secondary;
  }
  if (tone == MON_TONE_ACCENT) {
    return skin->accent;
  }
  return skin->primary;
}

/*
 * A pose is three sine waves and an offset.
 *
 * Parts move; nothing rotates.  That is the same choice the sheep and the pig
 * made, and it is the reason a creature costs two matrices per box instead of
 * three: the yaw matrix is shared by every box and the pose is folded into
 * the translation the box was going to need anyway.
 */
static void poseOffset(const MonPart *part, const CreaturePose *pose,
    u8 scale, float *out_x, float *out_y, float *out_z) {
  float step = sinf(pose->walk_time) * 3.f;
  float breathe = sinf(pose->walk_time * .3f) * 1.2f;
  float shake = pose->hurt > 0 ?
    sinf(pose->hurt * 90.f * M_DTOR) * 4.f : 0;
  float x = (float) part->x * scale / 100.f;
  float y = (float) part->y * scale / 100.f;
  float z = (float) part->z * scale / 100.f;

  switch (part->role) {
    case MON_ROLE_BODY:
      y += breathe;
      x += shake;
      break;
    case MON_ROLE_HEAD:
      y += breathe * 1.4f;
      x += shake;
      break;
    case MON_ROLE_LEG_A:
    case MON_ROLE_ARM_A:
      y += step;
      z -= step * .4f;
      break;
    case MON_ROLE_LEG_B:
    case MON_ROLE_ARM_B:
      y -= step;
      z += step * .4f;
      break;
    case MON_ROLE_TAIL:
      x += sinf(pose->walk_time * .8f) * 4.f;
      y += breathe;
      break;
    case MON_ROLE_WING:
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
 * One rig into the display list.
 *
 * `slot` selects a render slot's matrices and vertex scratch, and both are
 * double-buffered by dl_no like every other RSP-referenced structure in the
 * game: the RSP may still be walking last frame's copy while this one is
 * written.
 *
 * `face_part` is the box that wears the player's face sheet, or MON_NONE.
 * No creature has one; a trainer does.
 */
static void drawRig(u8 slot, const MonRig *rig, const CreatureSkin *skin,
    u8 face_part, Vector3 position, float yaw, const CreaturePose *pose) {
  u8 part_index;

  if (slot >= MON_RENDER_SLOTS) {
    return;
  }

  for (part_index = 0; part_index < rig->part_count; part_index++) {
    const MonPart *part = &rig->parts[part_index];
    Vtx *verts = creature_verts[dl_no][slot][part_index];
    const u8 *color = toneColor(skin, part->tone);
    float ox;
    float oy;
    float oz;
    Vector3 offset;
    /* Bulk widens and deepens without heightening, so an elder reads as a
       heavier animal rather than a taller one. */
    s16 sx = (s16) ((part->sx * skin->scale * skin->bulk) / 10000);
    s16 sy = (s16) ((part->sy * skin->scale) / 100);
    s16 sz = (s16) ((part->sz * skin->scale * skin->bulk) / 10000);

    /* Boxes the creature has not grown into yet. */
    if (part->stage > skin->maturity) {
      continue;
    }
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;
    if (sz < 1) sz = 1;
    buildBox(verts, (s16) -sx, (s16) -sy, (s16) -sz, sx, sy, sz, color,
      skin->front_lit);

    poseOffset(part, pose, skin->scale, &ox, &oy, &oz);
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
    /* The face rides the head's own matrices, so it costs a vertex load and
       a display list and no third transform.  It is authored for an unscaled
       32-unit head, which is why the rig that wears one is drawn at 100. */
    if (part_index == face_part) {
      gSPVertex(dlp++, steve_face_verts, 24, 0);
      gSPDisplayList(dlp++, steve_face_display_list);
    }
  }
}

static void drawCreature(u8 slot, u8 species_id, Vector3 position, float yaw,
    const CreaturePose *pose) {
  const MonSpecies *species;
  CreatureSkin skin;

  if (species_id >= MON_SPECIES_COUNT) {
    return;
  }
  species = &mon_species[species_id];
  skin.primary = species->primary;
  skin.secondary = species->secondary;
  skin.accent = species->accent;
  skin.hair = species->accent;
  skin.scale = species->scale;
  skin.bulk = species->bulk;
  skin.maturity = species->maturity;
  skin.front_lit = FALSE;
  drawRig(slot, &mon_rigs[species->rig], &skin, MON_NONE, position, yaw,
    pose);
}

/*
 * A trainer, who is another player rather than another animal.
 *
 * Which of the two, and what they are wearing, comes from the seed the roamer
 * was spawned with -- the seed their team already comes from, so the trainer
 * who is the same fight twice is the same person twice.  Unscaled, always:
 * the face sheet is authored for a head of one size.
 */
static void drawTrainer(u8 slot, u32 seed, Vector3 position, float yaw,
    const CreaturePose *pose) {
  const MonTrainer *who = mon64TrainerFromSeed(seed);
  CreatureSkin skin;

  skin.primary = who->look.primary;
  skin.secondary = who->look.secondary;
  skin.accent = who->look.accent;
  skin.hair = who->look.hair;
  skin.scale = 100;
  skin.bulk = 100;
  skin.maturity = 3;
  skin.front_lit = TRUE;
  drawRig(slot, &mon_trainer_bodies[who->body], &skin, MON_TRAINER_HEAD_PART,
    position, yaw, pose);
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
  return side == 0 ? mon_battle.facing : mon_battle.facing + 180.f;
}

static void drawBattleCreatures(u8 viewer_num) {
  u8 side;

  for (side = 0; side < 2; side++) {
    CreaturePose pose;
    MonFighter *fighter = &mon_battle.fighter[side];

    if (fighter->species >= MON_SPECIES_COUNT) {
      continue;
    }
    if (!creatureVisible(viewer_num, mon_battle.stand[side])) {
      continue;
    }
    pose.walk_time = mon_battle.scene_time * .06f;
    pose.lunge = mon_battle.lunge[side];
    pose.hurt = mon_battle.hurt[side];
    pose.faint = mon_battle.faint[side] > 0 ?
      mon_battle.faint[side] : 0;
    pose.reach = BLOCK_SIZE * .7f;
    drawCreature(side, fighter->species, mon_battle.stand[side],
      battleFacing(side), &pose);
  }
}

static void drawRoamers(u8 viewer_num) {
  u8 drawn[MON_MAX_ROAMERS];
  u8 index;
  u8 slot;

  for (index = 0; index < MON_MAX_ROAMERS; index++) {
    drawn[index] = FALSE;
  }

  /*
   * Nearest first, capped at the render slots.  The cap is the point: a
   * creature the player has walked past is never worth the frame time of the
   * one they are walking toward, and the pool is small enough that picking
   * the nearest twice is cheaper than sorting it.
   */
  for (slot = 0; slot < MON_RENDER_SLOTS; slot++) {
    u8 best = MON_MAX_ROAMERS;
    float best_distance = 0;
    CreaturePose pose;

    for (index = 0; index < MON_MAX_ROAMERS; index++) {
      MonRoamer *roamer = &mon_roamers[index];
      Vector3 offset;
      float distance;

      if (drawn[index] || !roamer->active ||
          !creatureVisible(viewer_num, roamer->position)) {
        continue;
      }
      offset = add(roamer->position, mul(players[viewer_num].position, -1.f));
      distance = dot(offset, offset);
      if (best == MON_MAX_ROAMERS || distance < best_distance) {
        best = index;
        best_distance = distance;
      }
    }
    if (best == MON_MAX_ROAMERS) {
      break;
    }
    drawn[best] = TRUE;
    pose.walk_time = mon_roamers[best].walk_time;
    pose.lunge = 0;
    pose.hurt = 0;
    pose.faint = 0;
    pose.reach = 0;
    if (mon_roamers[best].kind == MON_ROAMER_TRAINER) {
      drawTrainer(slot, mon_roamers[best].seed, mon_roamers[best].position,
        mon_roamers[best].yaw, &pose);
    } else {
      drawCreature(slot, mon_roamers[best].species,
        mon_roamers[best].position, mon_roamers[best].yaw, &pose);
    }
  }
}

void mon64DrawForPlayer(u8 viewer_num) {
  if (!mon64Enabled()) {
    return;
  }
  gSPTexture(dlp++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
  setCreatureCombine();
  /* The box models have deliberately minimal geometry; culling off keeps
     every face reliable from any angle, exactly as the mob pass does. */
  gSPClearGeometryMode(dlp++, G_CULL_BACK);
  if (mon_battle.active) {
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
  const u8 *color = mon_type_color[type > MON_TYPE_COUNT ?
    MON_TYPE_COUNT : type];

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
  u8 phase = mon_battle.phase;
  u8 side = mon_battle.acting_side;
  MonFighter *mine = &mon_battle.fighter[side];
  MonFighter *theirs = &mon_battle.fighter[side ^ 1];

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
    case MON_PHASE_COMMAND: {
      u8 index;
      /* A divider, so the message half and the command half do not read as
         one long row of words. */
      fillColor(36, 39, 34);
      gDPFillRectangle(dlp++, 112, BATTLE_BOX_TOP + 6, 114,
        BATTLE_BOX_BOTTOM - 6);
      for (index = 0; index < 4; index++) {
        drawGridCell(gridX(index), gridY(index), 76,
          index == mon_battle.command_cursor);
      }
      break;
    }

    case MON_PHASE_MOVE: {
      u8 index;

      for (index = 0; index < mine->move_count; index++) {
        drawGridCell(moveGridX(index), moveGridY(index), MOVE_CELL_WIDTH,
          index == mon_battle.move_cursor);
      }
      if (mine->move_count > 0) {
        drawTypeSwatch(MOVE_INFO_X + 44, 178,
          mon_moves[mine->move[mon_battle.move_cursor]].type);
      }
      break;
    }

    case MON_PHASE_TEAM: {
      u8 index;
      u8 player_num = mon_battle.side_player[side];

      for (index = 0; index < MON_PARTY_SIZE; index++) {
        PartyMon *mon = &mon_party[player_num][index];
        u32 row_y = BATTLE_BOX_TOP + 8 + index * 10;

        if (index == mon_battle.team_cursor) {
          fillColor(216, 191, 77);
          gDPFillRectangle(dlp++, 14, row_y - 2, 306, row_y + 8);
        }
        if (mon->species == MON_NONE) {
          continue;
        }
        drawHealthBar(200, row_y, 60, mon->hp,
          monMaxHealth(mon->species, mon->level));
      }
      break;
    }

    case MON_PHASE_BAG: {
      u8 index;

      for (index = 0; index < 2; index++) {
        u32 row_y = BATTLE_BOX_TOP + 12 + index * 20;
        if (index == mon_battle.bag_cursor) {
          fillColor(216, 191, 77);
          gDPFillRectangle(dlp++, 14, row_y - 3, 306, row_y + 9);
        }
      }
      break;
    }

    case MON_PHASE_CATCH: {
      /* Three lamps: one lights per successful shake, so the wobble the
         player is waiting through is visible rather than implied. */
      u8 index;
      u8 lit = (u8) (mon_battle.catch_time / 20.f);

      for (index = 0; index < 3; index++) {
        u32 x = 140 + index * 16;
        fillColor(24, 27, 23);
        gDPFillRectangle(dlp++, x, 196, x + 12, 208);
        if (index < lit && index < mon_battle.catch_shakes) {
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
  u8 phase = mon_battle.phase;
  u8 side = mon_battle.acting_side;
  MonFighter *mine = &mon_battle.fighter[side];
  MonFighter *theirs = &mon_battle.fighter[side ^ 1];

  beginText();

  textColor(240, 226, 198);
  if (theirs->species < MON_SPECIES_COUNT) {
    drawString(mon_species[theirs->species].name, 18, 20);
    drawLevel(theirs->level, 112, 20);
  }
  if (mine->species < MON_SPECIES_COUNT) {
    drawString(mon_species[mine->species].name, 176, 20);
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
    case MON_PHASE_MESSAGE:
    case MON_PHASE_CATCH: {
      const char *line = mon_battle.message[mon_battle.message_head];
      u32 revealed = mon_battle.message_reveal / 4u;
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
      if (mon_battle.message_count > 0 && line[index] == 0) {
        textColor(150, 155, 142);
        drawString("A CONTINUE", BATTLE_TEXT_X, 210);
      }
      break;
    }

    case MON_PHASE_COMMAND: {
      u8 index;

      textColor(240, 226, 198);
      drawString("WHAT WILL", 18, 178);
      if (mine->species < MON_SPECIES_COUNT) {
        drawString(mon_species[mine->species].name, 18, 192);
      }
      drawString("DO?", 18, 206);
      for (index = 0; index < 4; index++) {
        /* Brighter, not darker.  The font draws each glyph cell with an
           opaque background, so dark text on the yellow bar comes out as a
           black block with a black letter inside it. */
        textColor(index == mon_battle.command_cursor ? 255 : 176,
          index == mon_battle.command_cursor ? 244 : 180,
          index == mon_battle.command_cursor ? 214 : 168);
        drawString(command_labels[index], gridX(index), gridY(index));
      }
      break;
    }

    case MON_PHASE_MOVE: {
      u8 index;

      for (index = 0; index < mine->move_count; index++) {
        u8 selected = index == mon_battle.move_cursor;
        textColor(selected ? 255 : 176, selected ? 244 : 180,
          selected ? 214 : 168);
        drawString(mon_moves[mine->move[index]].name, moveGridX(index),
          moveGridY(index));
      }
      if (mine->move_count > 0) {
        const MonMove *move =
          &mon_moves[mine->move[mon_battle.move_cursor]];

        /* Type and remaining uses of whatever is highlighted: the two
           numbers the choice actually turns on. */
        textColor(200, 204, 190);
        drawString(mon_type_name[move->type > MON_TYPE_COUNT ?
          MON_TYPE_COUNT : move->type], MOVE_INFO_X, 178);
        drawString("PP", MOVE_INFO_X, 194);
        {
          u32 x = drawNumber(mine->pp[mon_battle.move_cursor],
            MOVE_INFO_X + 18, 194);
          drawChar('/', x, 194);
          drawNumber(move->pp, x + charWidth('/'), 194);
        }
      }
      textColor(150, 155, 142);
      drawString("B BACK", MOVE_INFO_X, 210);
      break;
    }

    case MON_PHASE_TEAM: {
      u8 index;
      u8 player_num = mon_battle.side_player[side];

      for (index = 0; index < MON_PARTY_SIZE; index++) {
        PartyMon *mon = &mon_party[player_num][index];
        u32 row_y = BATTLE_BOX_TOP + 8 + index * 10;
        u8 selected = index == mon_battle.team_cursor;

        textColor(selected ? 255 : 176, selected ? 244 : 180,
          selected ? 214 : 168);
        if (mon->species == MON_NONE) {
          drawString("-", 20, row_y);
          continue;
        }
        drawString(mon_species[mon->species].name, 20, row_y);
        drawLevel(mon->level, 116, row_y);
        if (mon->hp == 0) {
          drawString("DOWN", 160, row_y);
        }
      }
      break;
    }

    case MON_PHASE_BAG: {
      u8 index;
      u8 player_num = mon_battle.side_player[side];

      for (index = 0; index < 2; index++) {
        u8 item = battleBagItem(index);
        u32 row_y = BATTLE_BOX_TOP + 12 + index * 20;
        u8 selected = index == mon_battle.bag_cursor;

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

void mon64DrawBattleInterface(void) {
  if (!mon_battle.active) {
    return;
  }
  drawBattleFills();
  drawBattleText();
}

/*
 * The overworld prompt.
 *
 * One line, above the hotbar, naming what is in front of the player.  This is
 * the entire encounter interface: the game never takes the player out of what
 * they were doing, so the badge appearing at all has to be the invitation, or
 * walking past a creature you wanted becomes a thing that happens.
 *
 * A engages, but the badge does not say so.  The button glyph sat directly in
 * front of the species name, where it read as an article rather than a button
 * -- "A EMBEAR LV 8" -- and A is the only thing the player has ever pressed
 * to act on what is in front of them.
 */
void mon64DrawPrompt(u8 player_num, u32 center_x, u32 bottom_y) {
  u8 target;
  MonRoamer *roamer;
  const char *label;
  u32 width;
  u32 left;
  u32 top;
  u32 x;

  if (!mon64Enabled() || mon_battle.active) {
    return;
  }
  target = mon64TargetRoamer(player_num);
  if (target == MON_NONE) {
    return;
  }
  roamer = &mon_roamers[target];
  if (roamer->species >= MON_SPECIES_COUNT) {
    return;
  }
  /* A person is named, an animal is a species.  The badge used to say TRAINER
     for everyone, which was the right word back when they were all drawn as
     the same borrowed animal; now that the one in front of you has their own
     face and their own clothes, the badge may as well say who they are.  Both
     come from the seed, so the name always belongs to the body wearing it. */
  label = roamer->kind == MON_ROAMER_TRAINER ?
    mon64TrainerFromSeed(roamer->seed)->name :
    mon_species[roamer->species].name;

  /* Sized to its contents rather than fixed, so a four-player viewport gets
     a badge that fits inside it and a nine-letter species still does. */
  width = 20;
  for (x = 0; label[x]; x++) {
    width += charWidth(label[x]);
  }
  if (roamer->kind != MON_ROAMER_TRAINER) {
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
  /* A trainer has no element of their own -- their team is drawn from the
     seed, not from the species they were spawned beside -- so the badge shows
     the plain swatch rather than asserting a type the fight will not have. */
  drawTypeSwatch(left + 4, top + 4, roamer->kind == MON_ROAMER_TRAINER ?
    MON_TYPE_PLAIN : mon_species[roamer->species].type);
  gDPPipeSync(dlp++);

  beginText();
  textColor(240, 226, 198);
  x = left + 16;
  drawString(label, x, top + 4);
  if (roamer->kind != MON_ROAMER_TRAINER) {
    x += 4;
    while (*label) {
      x += charWidth(*label++);
    }
    drawLevel(roamer->level, x, top + 4);
  }
  textColor(255, 255, 255);
}
