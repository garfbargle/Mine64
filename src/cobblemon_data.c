#include "cobblemon.h"

/*
 * The roster, the move list, the type chart and the six body rigs.
 *
 * All of it is const, so it lives in the code segment and costs no writable
 * RDRAM -- which is the whole reason a creature is a table row rather than a
 * subclass.  Adding a species is one entry here and nothing anywhere else:
 * no new model file, no new texture, no new draw path.
 */

/* Move ids, used by the learnsets below.  Names rather than raw indices,
   because a learnset is the one place a typo would be silent. */
#define MV_TACKLE 0
#define MV_HEADBUTT 1
#define MV_RUSH 2
#define MV_LEAFCUT 3
#define MV_SEEDSHOT 4
#define MV_SAPROOT 5
#define MV_BLOOM 6
#define MV_EMBER 7
#define MV_SEAR 8
#define MV_FLAREUP 9
#define MV_SPLASH 10
#define MV_TIDEWAVE 11
#define MV_MISTVEIL 12
#define MV_MUDSLAP 13
#define MV_QUAKE 14
#define MV_JOLT 15
#define MV_OVERLOAD 16
#define MV_CHARGE 17
#define MV_PEBBLE 18
#define MV_BOULDER 19
#define MV_RUBBLE 20
#define MV_GUST 21

/* Species ids, for the evolution links. */
#define SP_SPRIGLET 0
#define SP_BRAMBOK 1
#define SP_THORNVALE 2
#define SP_EMBERKIT 3
#define SP_CINDERPAW 4
#define SP_BLAZEMANE 5
#define SP_DRIPLET 6
#define SP_BROOKFIN 7
#define SP_TIDEMAW 8
#define SP_MUDLING 9
#define SP_LOAMBACK 10
#define SP_TERRALITH 11
#define SP_ZAPLING 12
#define SP_VOLTHOP 13
#define SP_STORMHOOF 14
#define SP_PEBBLIN 15
#define SP_COBBLOX 16
#define SP_GRANITON 17

#define RIG_QUADRUPED 0
#define RIG_BIPED 1
#define RIG_BLOB 2
#define RIG_SERPENT 3
#define RIG_BIRD 4
#define RIG_BRUTE 5

/*
 * Six silhouettes, each authored once and worn by three creatures.
 *
 * This is the compromise that makes eighteen species affordable: a rig is
 * 8 boxes of geometry and a species is a palette and a percentage, so the
 * roster costs about a kilobyte of tables instead of ten of vertices, and a
 * family reads as a family because it literally shares a body.
 *
 * Offsets are box centres above the ground contact point; extents are halves.
 * Nothing rotates -- a pose moves boxes, exactly as the sheep and pig do --
 * so the animation cost per creature is an addition per box.
 */
const CobbleRig cobble_rigs[COBBLE_RIG_COUNT] = {
  /* Quadruped: the grazing shape the world already has a vocabulary for. */
  {8, 46, {
    {  0, 30,   0, 16, 11, 22, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_BODY},
    {  0, 34, -26, 11, 10, 10, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_HEAD},
    {  0, 30, -38,  6,  5,  5, COBBLE_TONE_ACCENT,    COBBLE_ROLE_HEAD},
    {-11, 10, -13,  5, 10,  5, COBBLE_TONE_SECONDARY, COBBLE_ROLE_LEG_A},
    { 11, 10, -13,  5, 10,  5, COBBLE_TONE_SECONDARY, COBBLE_ROLE_LEG_B},
    {-11, 10,  13,  5, 10,  5, COBBLE_TONE_SECONDARY, COBBLE_ROLE_LEG_B},
    { 11, 10,  13,  5, 10,  5, COBBLE_TONE_SECONDARY, COBBLE_ROLE_LEG_A},
    {  0, 34,  26,  4,  4, 10, COBBLE_TONE_ACCENT,    COBBLE_ROLE_TAIL}
  }},
  /* Biped: upright, with a crest that carries most of the family colour. */
  {8, 60, {
    {  0, 30,   0, 12, 14,  8, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_BODY},
    {  0, 52,  -2, 12, 11, 11, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_HEAD},
    {  0, 64,  -2,  5,  4, 12, COBBLE_TONE_ACCENT,    COBBLE_ROLE_HEAD},
    {-16, 32,   0,  4, 12,  4, COBBLE_TONE_SECONDARY, COBBLE_ROLE_ARM_A},
    { 16, 32,   0,  4, 12,  4, COBBLE_TONE_SECONDARY, COBBLE_ROLE_ARM_B},
    { -7,  8,   0,  5,  8,  5, COBBLE_TONE_SECONDARY, COBBLE_ROLE_LEG_A},
    {  7,  8,   0,  5,  8,  5, COBBLE_TONE_SECONDARY, COBBLE_ROLE_LEG_B},
    {  0, 22,  14,  4,  4, 10, COBBLE_TONE_ACCENT,    COBBLE_ROLE_TAIL}
  }},
  /* Blob: the starter shape.  Two stacked volumes read as a body and a head
     without needing either, and the eyes do all the character work. */
  {7, 40, {
    {  0, 18,   0, 18, 18, 18, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_BODY},
    {  0, 34,   0, 11,  8, 11, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_HEAD},
    { -8, 24, -19,  3,  4,  2, COBBLE_TONE_ACCENT,    COBBLE_ROLE_HEAD},
    {  8, 24, -19,  3,  4,  2, COBBLE_TONE_ACCENT,    COBBLE_ROLE_HEAD},
    {-18,  8,   0,  5,  5,  5, COBBLE_TONE_SECONDARY, COBBLE_ROLE_LEG_A},
    { 18,  8,   0,  5,  5,  5, COBBLE_TONE_SECONDARY, COBBLE_ROLE_LEG_B},
    {  0, 43,   0,  5,  5,  5, COBBLE_TONE_ACCENT,    COBBLE_ROLE_HEAD}
  }},
  /* Serpent: tapering segments.  Its height is authored low on purpose -- it
     is long, not tall, and the battle camera should frame it that way. */
  {8, 34, {
    {  0, 22, -30, 10,  9, 10, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_HEAD},
    {  0, 18, -14,  9,  9,  9, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_BODY},
    {  0, 16,   2,  8,  8,  9, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_BODY},
    {  0, 14,  18,  7,  7,  9, COBBLE_TONE_SECONDARY, COBBLE_ROLE_TAIL},
    {  0, 12,  32,  5,  5,  8, COBBLE_TONE_SECONDARY, COBBLE_ROLE_TAIL},
    { -5, 25, -39,  2,  3,  2, COBBLE_TONE_ACCENT,    COBBLE_ROLE_HEAD},
    {  5, 25, -39,  2,  3,  2, COBBLE_TONE_ACCENT,    COBBLE_ROLE_HEAD},
    {  0, 30,  -8,  2,  7,  8, COBBLE_TONE_ACCENT,    COBBLE_ROLE_BODY}
  }},
  /* Bird: wings are the only parts with their own animation role, and they
     are what makes this rig recognisable from across a field. */
  {8, 44, {
    {  0, 24,   0, 10, 11, 14, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_BODY},
    {  0, 40,  -8,  8,  8,  8, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_HEAD},
    {  0, 38, -18,  3,  3,  6, COBBLE_TONE_ACCENT,    COBBLE_ROLE_HEAD},
    {-13, 26,   0,  3,  9, 12, COBBLE_TONE_SECONDARY, COBBLE_ROLE_WING},
    { 13, 26,   0,  3,  9, 12, COBBLE_TONE_SECONDARY, COBBLE_ROLE_WING},
    {  0, 24,  16,  6,  3, 10, COBBLE_TONE_ACCENT,    COBBLE_ROLE_TAIL},
    { -5,  6,   0,  3,  6,  3, COBBLE_TONE_ACCENT,    COBBLE_ROLE_LEG_A},
    {  5,  6,   0,  3,  6,  3, COBBLE_TONE_ACCENT,    COBBLE_ROLE_LEG_B}
  }},
  /* Brute: the final-stage shape.  Heavy arms swing instead of legs, so it
     is obviously slower than everything it evolved from. */
  {8, 62, {
    {  0, 34,   0, 20, 16, 14, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_BODY},
    {  0, 58,  -6, 13, 11, 12, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_HEAD},
    {  0, 67, -10, 14,  3,  6, COBBLE_TONE_ACCENT,    COBBLE_ROLE_HEAD},
    {-24, 36,   0,  7, 16,  7, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_ARM_A},
    { 24, 36,   0,  7, 16,  7, COBBLE_TONE_PRIMARY,   COBBLE_ROLE_ARM_B},
    {-24, 15,   0,  8,  7,  8, COBBLE_TONE_ACCENT,    COBBLE_ROLE_ARM_A},
    { 24, 15,   0,  8,  7,  8, COBBLE_TONE_ACCENT,    COBBLE_ROLE_ARM_B},
    {  0,  9,   0, 16,  9, 11, COBBLE_TONE_SECONDARY, COBBLE_ROLE_STATIC}
  }}
};

/*
 * Moves.  Eight characters each, because four of them have to fit a two-wide
 * grid on a 320-pixel screen, and every effect is a single sentence the log
 * can actually print.
 */
const CobbleMove cobble_moves[COBBLE_MOVE_COUNT] = {
  {"TACKLE",   COBBLE_TYPE_PLAIN, 40, 100, 25, COBBLE_EFFECT_NONE},
  {"HEADBUTT", COBBLE_TYPE_PLAIN, 55,  95, 20, COBBLE_EFFECT_NONE},
  {"RUSH",     COBBLE_TYPE_PLAIN, 70,  85, 12, COBBLE_EFFECT_RECOIL},
  {"LEAFCUT",  COBBLE_GRASS,      50, 100, 20, COBBLE_EFFECT_NONE},
  {"SEEDSHOT", COBBLE_GRASS,      65,  95, 15, COBBLE_EFFECT_NONE},
  {"SAPROOT",  COBBLE_GRASS,      40, 100, 15, COBBLE_EFFECT_DRAIN},
  {"BLOOM",    COBBLE_GRASS,       0, 100, 10, COBBLE_EFFECT_HEAL},
  {"EMBER",    COBBLE_FIRE,       45, 100, 20, COBBLE_EFFECT_NONE},
  {"SEAR",     COBBLE_FIRE,       55,  95, 15, COBBLE_EFFECT_HIGH_CRIT},
  {"FLAREUP",  COBBLE_FIRE,       80,  85,  8, COBBLE_EFFECT_NONE},
  {"SPLASH",   COBBLE_WATER,      45, 100, 20, COBBLE_EFFECT_NONE},
  {"TIDEWAVE", COBBLE_WATER,      75,  90, 10, COBBLE_EFFECT_NONE},
  {"MISTVEIL", COBBLE_WATER,       0, 100, 10, COBBLE_EFFECT_BUFF_DEF},
  {"MUDSLAP",  COBBLE_EARTH,      45, 100, 20, COBBLE_EFFECT_NONE},
  {"QUAKE",    COBBLE_EARTH,      80,  85,  8, COBBLE_EFFECT_NONE},
  {"JOLT",     COBBLE_SPARK,      45, 100, 20, COBBLE_EFFECT_NONE},
  {"OVERLOAD", COBBLE_SPARK,      85,  80,  6, COBBLE_EFFECT_RECOIL},
  {"CHARGE",   COBBLE_SPARK,       0, 100, 10, COBBLE_EFFECT_BUFF_ATK},
  {"PEBBLE",   COBBLE_STONE,      45, 100, 20, COBBLE_EFFECT_NONE},
  {"BOULDER",  COBBLE_STONE,      80,  85,  8, COBBLE_EFFECT_NONE},
  {"RUBBLE",   COBBLE_STONE,      28,  90, 15, COBBLE_EFFECT_MULTI},
  {"GUST",     COBBLE_TYPE_PLAIN, 50,  95, 18, COBBLE_EFFECT_NONE}
};

/*
 * Effectiveness in quarters: 8 doubles, 4 is neutral, 2 halves.
 *
 * Two triangles and two cross links -- eight arrows in total, each of which
 * survives being said out loud.  Fire burns grass, grass drinks water, water
 * drowns fire; earth swallows spark, spark shatters stone, stone crushes
 * earth; and separately spark finds water while grass splits earth.  Every
 * arrow is symmetric: if it doubles one way it halves the other, so a player
 * only ever has to remember the eight.
 */
const u8 cobble_type_chart[COBBLE_TYPE_COUNT][COBBLE_TYPE_COUNT] = {
  /*            GRASS FIRE WATER EARTH SPARK STONE */
  /* GRASS */ {    4,   2,    8,    8,    4,    4},
  /* FIRE  */ {    8,   4,    2,    4,    4,    4},
  /* WATER */ {    2,   8,    4,    4,    2,    4},
  /* EARTH */ {    2,   4,    4,    4,    8,    2},
  /* SPARK */ {    4,   4,    8,    2,    4,    8},
  /* STONE */ {    4,   4,    4,    8,    2,    4}
};

const u8 cobble_type_color[COBBLE_TYPE_COUNT + 1][3] = {
  {110, 180,  90},
  {226, 110,  58},
  { 86, 156, 214},
  {156, 120,  80},
  {238, 208,  86},
  {150, 152, 148},
  {200, 200, 190}
};

const char *cobble_type_name[COBBLE_TYPE_COUNT + 1] = {
  "GRASS", "FIRE", "WATER", "EARTH", "SPARK", "STONE", "PLAIN"
};

/*
 * The roster.  Six families of three, one per type, each family sharing a
 * rig and growing in size and saturation as it evolves.
 *
 * Only the first two stages spawn wild (spawn_weight zero means "reachable
 * by raising one, never by finding one"), which is what makes a final stage
 * mean something in a world with no fixed size and no gym badges.  The
 * middle stages are aggressive after dark, so a PEACEFUL world is genuinely
 * a gentler world here too rather than an unrelated switch.
 */
const CobbleSpecies cobble_species[COBBLE_SPECIES_COUNT] = {
  {"SPRIGLET", RIG_BLOB, COBBLE_GRASS, 70,
    45, 40, 42, 38, 200, 18, 30,
    COBBLE_HAB_GRASS, SP_BRAMBOK, 14, FALSE,
    {124, 186,  92}, { 86, 140,  66}, {232, 214, 110},
    {MV_TACKLE, MV_LEAFCUT, MV_SAPROOT, MV_SEEDSHOT}, {1, 1, 9, 17}},

  {"BRAMBOK", RIG_BIPED, COBBLE_GRASS, 90,
    60, 58, 58, 52, 110, 34, 8,
    COBBLE_HAB_GRASS, SP_THORNVALE, 30, TRUE,
    { 86, 150,  72}, { 62, 108,  54}, {206,  92,  86},
    {MV_LEAFCUT, MV_HEADBUTT, MV_SEEDSHOT, MV_BLOOM}, {1, 1, 1, 20}},

  {"THORNVALE", RIG_BRUTE, COBBLE_GRASS, 100,
    82, 80, 78, 58, 55, 62, 0,
    COBBLE_HAB_GRASS, COBBLE_NONE, 0, FALSE,
    { 58, 118,  58}, { 40,  84,  42}, {226, 196,  96},
    {MV_SEEDSHOT, MV_RUSH, MV_BLOOM, MV_SAPROOT}, {1, 1, 1, 1}},

  {"EMBERKIT", RIG_QUADRUPED, COBBLE_FIRE, 65,
    40, 48, 36, 50, 195, 19, 22,
    COBBLE_HAB_SAND | COBBLE_HAB_STONE, SP_CINDERPAW, 16, FALSE,
    {232, 140,  66}, {186,  96,  48}, {255, 214, 110},
    {MV_TACKLE, MV_EMBER, MV_SEAR, MV_HEADBUTT}, {1, 1, 11, 19}},

  {"CINDERPAW", RIG_QUADRUPED, COBBLE_FIRE, 95,
    55, 66, 48, 66, 105, 35, 6,
    COBBLE_HAB_SAND | COBBLE_HAB_STONE, SP_BLAZEMANE, 32, TRUE,
    {222, 112,  52}, {166,  70,  36}, {255, 196,  86},
    {MV_EMBER, MV_SEAR, MV_HEADBUTT, MV_FLAREUP}, {1, 1, 1, 24}},

  {"BLAZEMANE", RIG_QUADRUPED, COBBLE_FIRE, 118,
    74, 88, 62, 82, 50, 64, 0,
    COBBLE_HAB_SAND, COBBLE_NONE, 0, FALSE,
    {206,  74,  40}, {140,  44,  26}, {255, 176,  64},
    {MV_FLAREUP, MV_SEAR, MV_RUSH, MV_EMBER}, {1, 1, 1, 1}},

  {"DRIPLET", RIG_BLOB, COBBLE_WATER, 65,
    46, 36, 44, 40, 200, 18, 26,
    COBBLE_HAB_SHORE, SP_BROOKFIN, 15, FALSE,
    { 98, 176, 220}, { 62, 124, 178}, {226, 244, 250},
    {MV_TACKLE, MV_SPLASH, MV_MISTVEIL, MV_TIDEWAVE}, {1, 1, 10, 18}},

  {"BROOKFIN", RIG_SERPENT, COBBLE_WATER, 90,
    60, 52, 58, 60, 108, 34, 7,
    COBBLE_HAB_SHORE, SP_TIDEMAW, 31, TRUE,
    { 72, 150, 206}, { 44, 104, 160}, {196, 232, 244},
    {MV_SPLASH, MV_MISTVEIL, MV_HEADBUTT, MV_TIDEWAVE}, {1, 1, 1, 22}},

  {"TIDEMAW", RIG_SERPENT, COBBLE_WATER, 122,
    84, 74, 76, 70, 48, 63, 0,
    COBBLE_HAB_SHORE, COBBLE_NONE, 0, FALSE,
    { 46, 116, 182}, { 28,  78, 132}, {170, 214, 238},
    {MV_TIDEWAVE, MV_SPLASH, MV_RUSH, MV_MISTVEIL}, {1, 1, 1, 1}},

  {"MUDLING", RIG_BLOB, COBBLE_EARTH, 75,
    52, 38, 50, 28, 205, 18, 24,
    COBBLE_HAB_GRASS | COBBLE_HAB_STONE, SP_LOAMBACK, 15, FALSE,
    {156, 120,  82}, {110,  84,  56}, {196, 168, 118},
    {MV_TACKLE, MV_MUDSLAP, MV_PEBBLE, MV_QUAKE}, {1, 1, 12, 20}},

  {"LOAMBACK", RIG_QUADRUPED, COBBLE_EARTH, 100,
    68, 56, 66, 40, 106, 34, 7,
    COBBLE_HAB_GRASS | COBBLE_HAB_STONE, SP_TERRALITH, 32, TRUE,
    {140, 106,  70}, { 96,  72,  46}, {176, 150, 102},
    {MV_MUDSLAP, MV_PEBBLE, MV_HEADBUTT, MV_QUAKE}, {1, 1, 1, 23}},

  {"TERRALITH", RIG_BRUTE, COBBLE_EARTH, 112,
    92, 76, 88, 44, 45, 65, 0,
    COBBLE_HAB_STONE, COBBLE_NONE, 0, FALSE,
    {118,  92,  62}, { 78,  60,  40}, {162, 142,  96},
    {MV_QUAKE, MV_MUDSLAP, MV_RUBBLE, MV_RUSH}, {1, 1, 1, 1}},

  {"ZAPLING", RIG_BIRD, COBBLE_SPARK, 65,
    38, 44, 34, 58, 190, 19, 18,
    COBBLE_HAB_GRASS | COBBLE_HAB_DAY, SP_VOLTHOP, 16, FALSE,
    {240, 214,  88}, {196, 166,  54}, { 86,  78,  52},
    {MV_TACKLE, MV_JOLT, MV_GUST, MV_CHARGE}, {1, 1, 10, 18}},

  {"VOLTHOP", RIG_BIRD, COBBLE_SPARK, 92,
    52, 60, 46, 78, 100, 36, 5,
    COBBLE_HAB_GRASS, SP_STORMHOOF, 33, TRUE,
    {238, 202,  64}, {190, 150,  38}, { 72,  66,  44},
    {MV_JOLT, MV_GUST, MV_CHARGE, MV_OVERLOAD}, {1, 1, 1, 25}},

  {"STORMHOOF", RIG_QUADRUPED, COBBLE_SPARK, 112,
    70, 82, 60, 96, 42, 66, 0,
    COBBLE_HAB_GRASS, COBBLE_NONE, 0, FALSE,
    {232, 190,  52}, {166, 128,  30}, { 60,  56,  40},
    {MV_OVERLOAD, MV_JOLT, MV_RUSH, MV_CHARGE}, {1, 1, 1, 1}},

  {"PEBBLIN", RIG_BLOB, COBBLE_STONE, 70,
    50, 42, 56, 26, 205, 18, 26,
    COBBLE_HAB_STONE, SP_COBBLOX, 15, FALSE,
    {152, 156, 150}, {110, 114, 110}, { 86,  90,  86},
    {MV_TACKLE, MV_PEBBLE, MV_RUBBLE, MV_BOULDER}, {1, 1, 12, 21}},

  {"COBBLOX", RIG_BIPED, COBBLE_STONE, 95,
    64, 58, 72, 38, 104, 35, 7,
    COBBLE_HAB_STONE, SP_GRANITON, 32, TRUE,
    {138, 142, 136}, { 98, 102,  98}, { 74,  78,  74},
    {MV_PEBBLE, MV_RUBBLE, MV_HEADBUTT, MV_BOULDER}, {1, 1, 1, 24}},

  {"GRANITON", RIG_BRUTE, COBBLE_STONE, 122,
    88, 78, 92, 42, 44, 66, 0,
    COBBLE_HAB_STONE, COBBLE_NONE, 0, FALSE,
    {124, 128, 124}, { 86,  90,  86}, {168, 120,  72},
    {MV_BOULDER, MV_RUBBLE, MV_RUSH, MV_PEBBLE}, {1, 1, 1, 1}}
};
