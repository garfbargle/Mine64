#include "humanoid.h"

#include "blocks.h"
#include "graphics.h"
#include "items.h"

/*
 * The one body everybody in this world who is a person is drawn from.  The
 * why is in humanoid.h; this is the geometry, the transforms and the pool.
 */

/* Two matrices per anchor, double-buffered by dl_no like every other
   RSP-referenced structure: the RSP may still be walking last frame's copy
   while this one is written. */
static Mtx humanoid_translate[NUM_DISPLAY_LISTS][HUMANOID_SLOTS][HUMANOID_PART_COUNT];
static Mtx humanoid_rotate[NUM_DISPLAY_LISTS][HUMANOID_SLOTS][HUMANOID_PART_COUNT];

/* How many NPC slots this viewport has handed out. */
static u8 claimed_npc_slots;

/*
 * Shading, and only shading.  White is the lit face and grey the far one, so
 * the garment colour can arrive as the primitive colour instead of being
 * baked in eight vertices at a time.  180/255 is the same ratio the creature
 * boxes use, and it sits in the middle of the hand-picked pairs the player's
 * old baked vertices had.
 */
#define SHADE_LIT 255
#define SHADE_DIM 180

/* Grey written out per channel, so these arrays read like every other model
   table in the project and the offline preview parses them by the same rule. */
#define HUMANOID_VERTEX(x, y, z, r, g, b) {x, y, z, 0, 0, 0, r, g, b, 255}

/* Front is -Z, matching every other model in the game. */
#define HUMANOID_BOX(name, x0, y0, z0, x1, y1, z1) \
static Vtx name[] = { \
  HUMANOID_VERTEX(x0, y1, z1, SHADE_DIM, SHADE_DIM, SHADE_DIM), \
  HUMANOID_VERTEX(x1, y1, z1, SHADE_DIM, SHADE_DIM, SHADE_DIM), \
  HUMANOID_VERTEX(x1, y0, z1, SHADE_DIM, SHADE_DIM, SHADE_DIM), \
  HUMANOID_VERTEX(x0, y0, z1, SHADE_DIM, SHADE_DIM, SHADE_DIM), \
  HUMANOID_VERTEX(x1, y1, z0, SHADE_LIT, SHADE_LIT, SHADE_LIT), \
  HUMANOID_VERTEX(x0, y1, z0, SHADE_LIT, SHADE_LIT, SHADE_LIT), \
  HUMANOID_VERTEX(x0, y0, z0, SHADE_LIT, SHADE_LIT, SHADE_LIT), \
  HUMANOID_VERTEX(x1, y0, z0, SHADE_LIT, SHADE_LIT, SHADE_LIT) \
}

/* The body.  Limbs hang from y = 0 so a pitch pivots at the shoulder and the
   hip rather than spinning the limb about its middle. */
HUMANOID_BOX(humanoid_body_verts, -18, -22, -9, 18, 22, 9);
HUMANOID_BOX(humanoid_head_verts, -16, -16, -16, 16, 16, 16);
HUMANOID_BOX(humanoid_arm_verts, -7, -44, -7, 7, 0, 7);
HUMANOID_BOX(humanoid_leg_verts, -8, -44, -8, 8, 0, 8);

/*
 * Hair, in head-local units: the head box is 32 across, so its faces are at
 * +-16 and anything at 17 or 18 stands proud of the skull.
 *
 * Short is a crown and a panel down the nape.  Long is a deeper crown that
 * clothes the sides and the whole back of the skull, a fringe, a strand down
 * each temple, and a ponytail.  The long style's three front boxes are what
 * let it be a colour of its own: they cover the face sheet's painted fringe
 * and sideburns, which are a fixed brown and would otherwise sit in the
 * middle of a blonde head.
 */
HUMANOID_BOX(humanoid_crown_verts, -17, 10, -18, 17, 18, 18);
HUMANOID_BOX(humanoid_nape_verts, -17, -12, 14, 17, 12, 18);
HUMANOID_BOX(humanoid_long_crown_verts, -17, -7, -12, 17, 19, 20);
/* Level with the top of the eyes and no lower; a unit further down and a
   fringe reads as a blindfold.  Full crown width and height, so no step
   shows where the two meet. */
HUMANOID_BOX(humanoid_fringe_verts, -17, 9, -20, 17, 19, -10);
HUMANOID_BOX(humanoid_left_strand_verts, -17, -7, -20, -11, 11, 4);
HUMANOID_BOX(humanoid_right_strand_verts, 11, -7, -20, 17, 11, 4);
/* The ponytail alone rides the hair anchor, so it swings a little behind the
   head that carries it. */
HUMANOID_BOX(humanoid_ponytail_verts, -5, -21, 18, 5, 11, 28);

/*
 * The people.
 *
 * Eight, each with a name for whatever badge is over their head and a look.
 * Nobody wears the player's blue over the player's own skin: a person walking
 * toward you in co-op must not read as the other player.
 */
const HumanoidPerson humanoid_people[HUMANOID_PEOPLE] = {
  {"SAM",   {{ 96, 168,  96}, { 74,  86, 104}, {205, 145,  98}, { 72,  48,  28},
    HUMANOID_HAIR_SHORT}},
  {"TOBY",  {{226, 140,  66}, { 96,  74,  52}, {232, 186, 148}, {198, 150,  72},
    HUMANOID_HAIR_SHORT}},
  {"MILO",  {{ 92, 104, 190}, { 60,  64,  78}, {138,  94,  66}, { 40,  34,  32},
    HUMANOID_HAIR_SHORT}},
  {"ELI",   {{ 80, 164, 158}, { 70,  74,  80}, {170, 118,  82}, {120,  72,  44},
    HUMANOID_HAIR_SHORT}},
  {"KALIA", {{206,  92,  86}, { 72,  66,  90}, {232, 186, 148}, {158, 118,  74},
    HUMANOID_HAIR_LONG}},
  {"MAYA",  {{158, 110, 190}, { 64,  60,  84}, {138,  94,  66}, { 44,  38,  44},
    HUMANOID_HAIR_LONG}},
  {"NORA",  {{224, 196,  88}, {104,  80,  56}, {205, 145,  98}, {186,  96,  52},
    HUMANOID_HAIR_LONG}},
  {"IVY",   {{ 72, 150, 120}, { 56,  72,  60}, {170, 118,  82}, { 96,  66,  42},
    HUMANOID_HAIR_LONG}}
};

/* The blue shirt, the darker trousers and the hairline brown the face sheet
   paints, which is what makes the crown the front of the fringe rather than a
   stripe above it. */
const HumanoidLook humanoid_player_look = {
  { 64, 150, 198}, { 61,  82, 174}, {205, 145,  98}, { 72,  48,  28},
  HUMANOID_HAIR_SHORT
};

const HumanoidPerson *humanoidPersonFromSeed(u32 seed) {
  return &humanoid_people[seed % HUMANOID_PEOPLE];
}

void humanoidBeginViewport(void) {
  claimed_npc_slots = 0;
}

u8 humanoidClaimSlot(void) {
  if (claimed_npc_slots >= HUMANOID_NPC_SLOTS) {
    return HUMANOID_NO_SLOT;
  }
  return (u8) (MAX_PLAYERS + claimed_npc_slots++);
}

void humanoidWalkPose(HumanoidPose *pose, Vector3 ground, float body_yaw,
    float head_yaw, float walk_time, float swing) {
  /* The same 28 degrees the player swings through, so an NPC crossing a field
     walks like the person watching them. */
  float step = sinf(walk_time) * 28.f * swing;

  pose->position = ground;
  pose->body_yaw = body_yaw;
  pose->head_yaw = head_yaw;
  pose->head_pitch = 0;
  pose->left_arm_pitch = step;
  pose->right_arm_pitch = -step;
  pose->left_leg_pitch = -step;
  pose->right_leg_pitch = step;
  pose->bob = 0;
  pose->walk_time = walk_time;
  pose->held_item = AIR;
}

/*
 * One anchor.  The offset is measured from the ground the feet stand on and
 * turns with the body before it is added, exactly as the player's own
 * transforms have always done.
 */
static void setPartTransform(u8 slot, u8 part, Vector3 position,
    float body_yaw, Vector3 local_offset, float pitch, float yaw) {
  Vector3 offset = rotateY(local_offset, -body_yaw);

  guTranslate(&humanoid_translate[dl_no][slot][part],
    position.x + offset.x - render_origin_units_x,
    position.y + offset.y,
    position.z + offset.z - render_origin_units_z);
  guRotateRPY(&humanoid_rotate[dl_no][slot][part], pitch, yaw, 0);
}

static void bindPart(u8 slot, u8 part) {
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&humanoid_translate[dl_no][slot][part]),
    G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
  gSPMatrix(dlp++, OS_K0_TO_PHYSICAL(&humanoid_rotate[dl_no][slot][part]),
    G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
}

/*
 * A garment colour, dimmed by the time of day.
 *
 * The tint the pass loaded is the primitive colour, and the primitive colour
 * is also how a person says what they are wearing, so the two are multiplied
 * here rather than one replacing the other.  Three multiplies per part, and
 * nobody's clothes stay noon-bright at dusk.
 */
static void setGarment(const u8 *rgb) {
  u8 tint[3];

  graphicsEntityTintRGB(tint);
  gDPSetPrimColor(dlp++, 0, 0,
    (u8) (((u32) rgb[0] * tint[0]) / 255),
    (u8) (((u32) rgb[1] * tint[1]) / 255),
    (u8) (((u32) rgb[2] * tint[2]) / 255), 255);
}

/* Back to the pass's own tint, for geometry that carries its own colours:
   the face sheet, and anything held in the hand. */
static void setPlainTint(void) {
  u8 tint[3];

  graphicsEntityTintRGB(tint);
  gDPSetPrimColor(dlp++, 0, 0, tint[0], tint[1], tint[2], 255);
}

static void drawBox(Vtx *verts) {
  gSPVertex(dlp++, verts, 8, 0);
  gSPDisplayList(dlp++, box_display_list);
}

static void drawPart(u8 slot, u8 part, Vtx *verts) {
  bindPart(slot, part);
  drawBox(verts);
}

/* Everything above the neck: the face sheet, then whichever hair the look
   asks for.  All of it rides the head's own matrices, except the ponytail. */
static void drawHead(u8 slot, const HumanoidLook *look) {
  bindPart(slot, HUMANOID_HEAD);
  setPlainTint();
  gSPVertex(dlp++, steve_face_verts, 24, 0);
  gSPDisplayList(dlp++, steve_face_display_list);

  setGarment(look->hair);
  if (look->style == HUMANOID_HAIR_LONG) {
    drawBox(humanoid_long_crown_verts);
    drawBox(humanoid_fringe_verts);
    drawBox(humanoid_left_strand_verts);
    drawBox(humanoid_right_strand_verts);
    bindPart(slot, HUMANOID_HAIR);
    drawBox(humanoid_ponytail_verts);
  } else {
    drawBox(humanoid_crown_verts);
    drawBox(humanoid_nape_verts);
  }
}

void humanoidDraw(u8 slot, const HumanoidLook *look, const HumanoidPose *pose) {
  Vector3 position = pose->position;
  float bob = pose->bob;
  /* Long hair lags the head it hangs from.  Six degrees is enough to read as
     weight at this scale and small enough never to leave the neck. */
  float hair_yaw = pose->head_yaw + sinf(pose->walk_time * .5f) * 6.f;

  if (slot >= HUMANOID_SLOTS) {
    return;
  }

  setPartTransform(slot, HUMANOID_BODY, position, pose->body_yaw,
    (Vector3) {bob, 66, 0}, 0, pose->body_yaw);
  /* The head turns on its own: a person looks at you without swinging their
     shoulders round to do it. */
  setPartTransform(slot, HUMANOID_HEAD, position, pose->body_yaw,
    (Vector3) {bob, 104, 0}, pose->head_pitch, pose->head_yaw);
  setPartTransform(slot, HUMANOID_HAIR, position, pose->body_yaw,
    (Vector3) {bob, 104, 0}, pose->head_pitch, hair_yaw);
  setPartTransform(slot, HUMANOID_LEFT_ARM, position, pose->body_yaw,
    (Vector3) {-25 + bob, 88, 0}, pose->left_arm_pitch, pose->body_yaw);
  setPartTransform(slot, HUMANOID_RIGHT_ARM, position, pose->body_yaw,
    (Vector3) {25 + bob, 88, 0}, pose->right_arm_pitch, pose->body_yaw);
  /* What is held shares the right hand's origin and its pitch, so walking and
     attacks carry it through the same arc the arm takes. */
  setPartTransform(slot, HUMANOID_HAND, position, pose->body_yaw,
    (Vector3) {25 + bob, 44, 0}, pose->right_arm_pitch, pose->body_yaw);
  setPartTransform(slot, HUMANOID_LEFT_LEG, position, pose->body_yaw,
    (Vector3) {-10 + bob, 44, 0}, pose->left_leg_pitch, pose->body_yaw);
  setPartTransform(slot, HUMANOID_RIGHT_LEG, position, pose->body_yaw,
    (Vector3) {10 + bob, 44, 0}, pose->right_leg_pitch, pose->body_yaw);

  setGarment(look->shirt);
  drawPart(slot, HUMANOID_BODY, humanoid_body_verts);
  setGarment(look->skin);
  drawPart(slot, HUMANOID_LEFT_ARM, humanoid_arm_verts);
  drawPart(slot, HUMANOID_RIGHT_ARM, humanoid_arm_verts);
  if (itemIsTool(pose->held_item)) {
    bindPart(slot, HUMANOID_HAND);
    setPlainTint();
    drawToolGeometry(pose->held_item);
  }
  setGarment(look->trousers);
  drawPart(slot, HUMANOID_LEFT_LEG, humanoid_leg_verts);
  drawPart(slot, HUMANOID_RIGHT_LEG, humanoid_leg_verts);
  setGarment(look->skin);
  drawPart(slot, HUMANOID_HEAD, humanoid_head_verts);
  drawHead(slot, look);

  /* The pass loaded a plain tint and expects to still have one: the mobs and
     the creatures drawn after this carry their colours in their vertices. */
  setPlainTint();
}
