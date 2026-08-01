#include "mon64.h"

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

/*
 * Species ids, for the evolution links.
 *
 * Six families, one animal each.  A name is a blend where the element word
 * and the animal word already share their sounds -- EMBEAR because "ember"
 * and "bear" are most of the same word, TOADSTOOL because it is one -- rather
 * than an element prefix glued onto a species.  A name that has to be
 * explained is a name that failed.
 */
#define SP_TADPOLLEN 0
#define SP_LEAFROG 1
#define SP_TOADSTOOL 2
#define SP_EMBEAR 3
#define SP_GRIZZLE 4
#define SP_PYREBRUIN 5
#define SP_DRIZZEEL 6
#define SP_TIDEEL 7
#define SP_WHIRLEEL 8
#define SP_MUDLET 9
#define SP_MUDGUANA 10
#define SP_IGUANEOUS 11
#define SP_SPAROWLET 12
#define SP_SPAROWL 13
#define SP_THUNDOWL 14
#define SP_PEBBOON 15
#define SP_GRANILLA 16
#define SP_BABOULDER 17

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
const MonRig mon_rigs[MON_RIG_COUNT] = {
  /* Bear.  Ears from the first day -- a cub without them is not cute -- then
     a shoulder hump at the second stage and a heavy brow at the third. */
  {11, 46, {
    {  0, 30,   0, 16, 11, 22, MON_TONE_PRIMARY,   MON_ROLE_BODY,   1},
    {  0, 34, -26, 11, 10, 10, MON_TONE_PRIMARY,   MON_ROLE_HEAD,   1},
    {  0, 30, -38,  6,  5,  5, MON_TONE_ACCENT,    MON_ROLE_HEAD,   1},
    {-11, 10, -13,  5, 10,  5, MON_TONE_SECONDARY, MON_ROLE_LEG_A,  1},
    { 11, 10, -13,  5, 10,  5, MON_TONE_SECONDARY, MON_ROLE_LEG_B,  1},
    {-11, 10,  13,  5, 10,  5, MON_TONE_SECONDARY, MON_ROLE_LEG_B,  1},
    { 11, 10,  13,  5, 10,  5, MON_TONE_SECONDARY, MON_ROLE_LEG_A,  1},
    { -8, 43, -25,  4,  4,  3, MON_TONE_PRIMARY,   MON_ROLE_HEAD,   1},
    {  8, 43, -25,  4,  4,  3, MON_TONE_PRIMARY,   MON_ROLE_HEAD,   1},
    {  0, 43,  -6, 12,  6, 12, MON_TONE_SECONDARY, MON_ROLE_BODY,   2},
    {  0, 43, -32, 12,  4,  6, MON_TONE_ACCENT,    MON_ROLE_HEAD,   3}
  }},
  /* Iguana.  Upright and long-tailed, with a crest along its head and back
     frills an adult has grown into. */
  {11, 60, {
    {  0, 30,   0, 12, 14,  8, MON_TONE_PRIMARY,   MON_ROLE_BODY,   1},
    {  0, 52,  -2, 12, 11, 11, MON_TONE_PRIMARY,   MON_ROLE_HEAD,   1},
    {  0, 64,  -2,  5,  4, 12, MON_TONE_ACCENT,    MON_ROLE_HEAD,   1},
    {-16, 32,   0,  4, 12,  4, MON_TONE_SECONDARY, MON_ROLE_ARM_A,  1},
    { 16, 32,   0,  4, 12,  4, MON_TONE_SECONDARY, MON_ROLE_ARM_B,  1},
    { -7,  8,   0,  5,  8,  5, MON_TONE_SECONDARY, MON_ROLE_LEG_A,  1},
    {  7,  8,   0,  5,  8,  5, MON_TONE_SECONDARY, MON_ROLE_LEG_B,  1},
    {  0, 22,  14,  4,  4, 10, MON_TONE_ACCENT,    MON_ROLE_TAIL,   1},
    {-13, 40,   4,  3,  8,  3, MON_TONE_ACCENT,    MON_ROLE_BODY,   2},
    { 13, 40,   4,  3,  8,  3, MON_TONE_ACCENT,    MON_ROLE_BODY,   2},
    {  0, 42,   9,  3, 10,  4, MON_TONE_SECONDARY, MON_ROLE_BODY,   3}
  }},
  /* Frog.  Two stacked volumes and a pair of eyes do all the work; the leaves
     on its back and the crown above them arrive with age. */
  {10, 40, {
    {  0, 18,   0, 18, 18, 18, MON_TONE_PRIMARY,   MON_ROLE_BODY,   1},
    {  0, 34,   0, 11,  8, 11, MON_TONE_PRIMARY,   MON_ROLE_HEAD,   1},
    { -8, 24, -19,  3,  4,  2, MON_TONE_ACCENT,    MON_ROLE_HEAD,   1},
    {  8, 24, -19,  3,  4,  2, MON_TONE_ACCENT,    MON_ROLE_HEAD,   1},
    {-18,  8,   0,  5,  5,  5, MON_TONE_SECONDARY, MON_ROLE_LEG_A,  1},
    { 18,  8,   0,  5,  5,  5, MON_TONE_SECONDARY, MON_ROLE_LEG_B,  1},
    {  0, 43,   0,  5,  5,  5, MON_TONE_ACCENT,    MON_ROLE_HEAD,   1},
    {-13, 28,  14,  6,  3,  7, MON_TONE_SECONDARY, MON_ROLE_BODY,   2},
    { 13, 28,  14,  6,  3,  7, MON_TONE_SECONDARY, MON_ROLE_BODY,   2},
    {  0, 50,   0,  8,  3,  8, MON_TONE_ACCENT,    MON_ROLE_HEAD,   3}
  }},
  /* Eel.  Tapering segments, authored low on purpose -- it is long, not
     tall.  Side fins come with the second stage, a tail fan with the third. */
  {11, 34, {
    {  0, 22, -30, 10,  9, 10, MON_TONE_PRIMARY,   MON_ROLE_HEAD,   1},
    {  0, 18, -14,  9,  9,  9, MON_TONE_PRIMARY,   MON_ROLE_BODY,   1},
    {  0, 16,   2,  8,  8,  9, MON_TONE_PRIMARY,   MON_ROLE_BODY,   1},
    {  0, 14,  18,  7,  7,  9, MON_TONE_SECONDARY, MON_ROLE_TAIL,   1},
    {  0, 12,  32,  5,  5,  8, MON_TONE_SECONDARY, MON_ROLE_TAIL,   1},
    { -5, 25, -39,  2,  3,  2, MON_TONE_ACCENT,    MON_ROLE_HEAD,   1},
    {  5, 25, -39,  2,  3,  2, MON_TONE_ACCENT,    MON_ROLE_HEAD,   1},
    {  0, 30,  -8,  2,  7,  8, MON_TONE_ACCENT,    MON_ROLE_BODY,   1},
    {-11, 17,  -6,  6,  2,  7, MON_TONE_SECONDARY, MON_ROLE_BODY,   2},
    { 11, 17,  -6,  6,  2,  7, MON_TONE_SECONDARY, MON_ROLE_BODY,   2},
    {  0, 12,  42,  2,  7,  6, MON_TONE_ACCENT,    MON_ROLE_TAIL,   3}
  }},
  /* Owl.  The wings are what make it readable from across a field; the ear
     tufts and chest ruff are what make an old one look old. */
  {11, 44, {
    {  0, 24,   0, 10, 11, 14, MON_TONE_PRIMARY,   MON_ROLE_BODY,   1},
    {  0, 40,  -8,  8,  8,  8, MON_TONE_PRIMARY,   MON_ROLE_HEAD,   1},
    {  0, 38, -18,  3,  3,  6, MON_TONE_ACCENT,    MON_ROLE_HEAD,   1},
    {-13, 26,   0,  3,  9, 12, MON_TONE_SECONDARY, MON_ROLE_WING,   1},
    { 13, 26,   0,  3,  9, 12, MON_TONE_SECONDARY, MON_ROLE_WING,   1},
    {  0, 24,  16,  6,  3, 10, MON_TONE_ACCENT,    MON_ROLE_TAIL,   1},
    { -5,  6,   0,  3,  6,  3, MON_TONE_ACCENT,    MON_ROLE_LEG_A,  1},
    {  5,  6,   0,  3,  6,  3, MON_TONE_ACCENT,    MON_ROLE_LEG_B,  1},
    { -6, 48,  -6,  3,  4,  3, MON_TONE_PRIMARY,   MON_ROLE_HEAD,   2},
    {  6, 48,  -6,  3,  4,  3, MON_TONE_PRIMARY,   MON_ROLE_HEAD,   2},
    {  0, 30,  -9,  9,  4,  4, MON_TONE_ACCENT,    MON_ROLE_BODY,   3}
  }},
  /* Ape.  Heavy arms swing instead of legs, so it is obviously slower than
     everything else; stone shoulders and a back slab pile on with age. */
  {11, 62, {
    {  0, 34,   0, 20, 16, 14, MON_TONE_PRIMARY,   MON_ROLE_BODY,   1},
    {  0, 58,  -6, 13, 11, 12, MON_TONE_PRIMARY,   MON_ROLE_HEAD,   1},
    {  0, 67, -10, 14,  3,  6, MON_TONE_ACCENT,    MON_ROLE_HEAD,   1},
    {-24, 36,   0,  7, 16,  7, MON_TONE_PRIMARY,   MON_ROLE_ARM_A,  1},
    { 24, 36,   0,  7, 16,  7, MON_TONE_PRIMARY,   MON_ROLE_ARM_B,  1},
    {-24, 15,   0,  8,  7,  8, MON_TONE_ACCENT,    MON_ROLE_ARM_A,  1},
    { 24, 15,   0,  8,  7,  8, MON_TONE_ACCENT,    MON_ROLE_ARM_B,  1},
    {  0,  9,   0, 16,  9, 11, MON_TONE_SECONDARY, MON_ROLE_STATIC, 1},
    {-19, 48,   0,  8,  5,  9, MON_TONE_SECONDARY, MON_ROLE_ARM_A,  2},
    { 19, 48,   0,  8,  5,  9, MON_TONE_SECONDARY, MON_ROLE_ARM_B,  2},
    {  0, 40,  12, 12, 10,  4, MON_TONE_ACCENT,    MON_ROLE_BODY,   3}
  }}
};

/*
 * Moves.  Eight characters each, because four of them have to fit a two-wide
 * grid on a 320-pixel screen, and every effect is a single sentence the log
 * can actually print.
 */
const MonMove mon_moves[MON_MOVE_COUNT] = {
  {"TACKLE",   MON_TYPE_PLAIN, 40, 100, 25, MON_EFFECT_NONE},
  {"HEADBUTT", MON_TYPE_PLAIN, 55,  95, 20, MON_EFFECT_NONE},
  {"RUSH",     MON_TYPE_PLAIN, 70,  85, 12, MON_EFFECT_RECOIL},
  {"LEAFCUT",  MON_GRASS,      50, 100, 20, MON_EFFECT_NONE},
  {"SEEDSHOT", MON_GRASS,      65,  95, 15, MON_EFFECT_NONE},
  {"SAPROOT",  MON_GRASS,      40, 100, 15, MON_EFFECT_DRAIN},
  {"BLOOM",    MON_GRASS,       0, 100, 10, MON_EFFECT_HEAL},
  {"EMBER",    MON_FIRE,       45, 100, 20, MON_EFFECT_NONE},
  {"SEAR",     MON_FIRE,       55,  95, 15, MON_EFFECT_HIGH_CRIT},
  {"FLAREUP",  MON_FIRE,       80,  85,  8, MON_EFFECT_NONE},
  {"SPLASH",   MON_WATER,      45, 100, 20, MON_EFFECT_NONE},
  {"TIDEWAVE", MON_WATER,      75,  90, 10, MON_EFFECT_NONE},
  {"MISTVEIL", MON_WATER,       0, 100, 10, MON_EFFECT_BUFF_DEF},
  {"MUDSLAP",  MON_EARTH,      45, 100, 20, MON_EFFECT_NONE},
  {"QUAKE",    MON_EARTH,      80,  85,  8, MON_EFFECT_NONE},
  {"JOLT",     MON_SPARK,      45, 100, 20, MON_EFFECT_NONE},
  {"OVERLOAD", MON_SPARK,      85,  80,  6, MON_EFFECT_RECOIL},
  {"CHARGE",   MON_SPARK,       0, 100, 10, MON_EFFECT_BUFF_ATK},
  {"PEBBLE",   MON_STONE,      45, 100, 20, MON_EFFECT_NONE},
  {"BOULDER",  MON_STONE,      80,  85,  8, MON_EFFECT_NONE},
  {"RUBBLE",   MON_STONE,      28,  90, 15, MON_EFFECT_MULTI},
  {"GUST",     MON_TYPE_PLAIN, 50,  95, 18, MON_EFFECT_NONE}
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
const u8 mon_type_chart[MON_TYPE_COUNT][MON_TYPE_COUNT] = {
  /*            GRASS FIRE WATER EARTH SPARK STONE */
  /* GRASS */ {    4,   2,    8,    8,    4,    4},
  /* FIRE  */ {    8,   4,    2,    4,    4,    4},
  /* WATER */ {    2,   8,    4,    4,    2,    4},
  /* EARTH */ {    2,   4,    4,    4,    8,    2},
  /* SPARK */ {    4,   4,    8,    2,    4,    8},
  /* STONE */ {    4,   4,    4,    8,    2,    4}
};

const u8 mon_type_color[MON_TYPE_COUNT + 1][3] = {
  {110, 180,  90},
  {226, 110,  58},
  { 86, 156, 214},
  {156, 120,  80},
  {238, 208,  86},
  {150, 152, 148},
  {200, 200, 190}
};

const char *mon_type_name[MON_TYPE_COUNT + 1] = {
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
const MonSpecies mon_species[MON_SPECIES_COUNT] = {
  {"TADPOLLEN", RIG_BLOB, MON_GRASS, 68, 100, 1,
    45, 40, 42, 38, 200, 18, 30,
    MON_HAB_GRASS, SP_LEAFROG, 14, FALSE,
    {124, 186,  92}, { 86, 140,  66}, {232, 214, 110},
    {MV_TACKLE, MV_LEAFCUT, MV_SAPROOT, MV_SEEDSHOT}, {1, 1, 9, 17}},

  {"LEAFROG", RIG_BLOB, MON_GRASS, 95, 106, 2,
    60, 58, 58, 52, 110, 34, 8,
    MON_HAB_GRASS, SP_TOADSTOOL, 30, TRUE,
    { 86, 150,  72}, { 58, 104,  50}, {206,  92,  86},
    {MV_LEAFCUT, MV_HEADBUTT, MV_SEEDSHOT, MV_BLOOM}, {1, 1, 1, 20}},

  {"TOADSTOOL", RIG_BLOB, MON_GRASS, 118, 120, 3,
    82, 80, 78, 58, 55, 62, 0,
    MON_HAB_GRASS, MON_NONE, 0, FALSE,
    { 58, 118,  58}, { 34,  78,  38}, {226, 108,  96},
    {MV_SEEDSHOT, MV_RUSH, MV_BLOOM, MV_SAPROOT}, {1, 1, 1, 1}},

  {"EMBEAR", RIG_QUADRUPED, MON_FIRE, 68, 100, 1,
    40, 48, 36, 50, 195, 19, 22,
    MON_HAB_SAND | MON_HAB_STONE, SP_GRIZZLE, 16, FALSE,
    {232, 140,  66}, {186,  96,  48}, {255, 214, 110},
    {MV_TACKLE, MV_EMBER, MV_SEAR, MV_HEADBUTT}, {1, 1, 11, 19}},

  {"GRIZZLE", RIG_QUADRUPED, MON_FIRE, 96, 108, 2,
    55, 66, 48, 66, 105, 35, 6,
    MON_HAB_SAND | MON_HAB_STONE, SP_PYREBRUIN, 32, TRUE,
    {222, 112,  52}, {166,  70,  36}, {255, 196,  86},
    {MV_EMBER, MV_SEAR, MV_HEADBUTT, MV_FLAREUP}, {1, 1, 1, 24}},

  {"PYREBRUIN", RIG_QUADRUPED, MON_FIRE, 118, 122, 3,
    74, 88, 62, 82, 50, 64, 0,
    MON_HAB_SAND, MON_NONE, 0, FALSE,
    {206,  74,  40}, {140,  44,  26}, {255, 176,  64},
    {MV_FLAREUP, MV_SEAR, MV_RUSH, MV_EMBER}, {1, 1, 1, 1}},

  {"DRIZZEEL", RIG_SERPENT, MON_WATER, 70, 100, 1,
    46, 36, 44, 40, 200, 18, 26,
    MON_HAB_SHORE, SP_TIDEEL, 15, FALSE,
    { 98, 176, 220}, { 62, 124, 178}, {226, 244, 250},
    {MV_TACKLE, MV_SPLASH, MV_MISTVEIL, MV_TIDEWAVE}, {1, 1, 10, 18}},

  {"TIDEEL", RIG_SERPENT, MON_WATER, 98, 106, 2,
    60, 52, 58, 60, 108, 34, 7,
    MON_HAB_SHORE, SP_WHIRLEEL, 31, TRUE,
    { 72, 150, 206}, { 44, 104, 160}, {196, 232, 244},
    {MV_SPLASH, MV_MISTVEIL, MV_HEADBUTT, MV_TIDEWAVE}, {1, 1, 1, 22}},

  {"WHIRLEEL", RIG_SERPENT, MON_WATER, 125, 120, 3,
    84, 74, 76, 70, 48, 63, 0,
    MON_HAB_SHORE, MON_NONE, 0, FALSE,
    { 46, 116, 182}, { 28,  78, 132}, {170, 214, 238},
    {MV_TIDEWAVE, MV_SPLASH, MV_RUSH, MV_MISTVEIL}, {1, 1, 1, 1}},

  {"MUDLET", RIG_BIPED, MON_EARTH, 70, 100, 1,
    52, 38, 50, 28, 205, 18, 24,
    MON_HAB_GRASS | MON_HAB_STONE, SP_MUDGUANA, 15, FALSE,
    {156, 120,  82}, {110,  84,  56}, {196, 168, 118},
    {MV_TACKLE, MV_MUDSLAP, MV_PEBBLE, MV_QUAKE}, {1, 1, 12, 20}},

  {"MUDGUANA", RIG_BIPED, MON_EARTH, 96, 108, 2,
    68, 56, 66, 40, 106, 34, 7,
    MON_HAB_GRASS | MON_HAB_STONE, SP_IGUANEOUS, 32, TRUE,
    {166,  98,  72}, {114,  66,  46}, {206, 152, 104},
    {MV_MUDSLAP, MV_PEBBLE, MV_HEADBUTT, MV_QUAKE}, {1, 1, 1, 23}},

  {"IGUANEOUS", RIG_BIPED, MON_EARTH, 118, 122, 3,
    92, 76, 88, 44, 45, 65, 0,
    MON_HAB_STONE, MON_NONE, 0, FALSE,
    {138,  84,  58}, { 86,  50,  34}, {236, 138,  74},
    {MV_QUAKE, MV_MUDSLAP, MV_RUBBLE, MV_RUSH}, {1, 1, 1, 1}},

  {"SPAROWLET", RIG_BIRD, MON_SPARK, 68, 100, 1,
    38, 44, 34, 58, 190, 19, 18,
    MON_HAB_GRASS | MON_HAB_DAY, SP_SPAROWL, 16, FALSE,
    {240, 214,  88}, {196, 166,  54}, { 86,  78,  52},
    {MV_TACKLE, MV_JOLT, MV_GUST, MV_CHARGE}, {1, 1, 10, 18}},

  {"SPAROWL", RIG_BIRD, MON_SPARK, 96, 108, 2,
    52, 60, 46, 78, 100, 36, 5,
    MON_HAB_GRASS, SP_THUNDOWL, 33, TRUE,
    {238, 202,  64}, {190, 150,  38}, { 72,  66,  44},
    {MV_JOLT, MV_GUST, MV_CHARGE, MV_OVERLOAD}, {1, 1, 1, 25}},

  {"THUNDOWL", RIG_BIRD, MON_SPARK, 120, 122, 3,
    70, 82, 60, 96, 42, 66, 0,
    MON_HAB_GRASS, MON_NONE, 0, FALSE,
    {232, 190,  52}, {158, 118,  28}, { 60,  56,  40},
    {MV_OVERLOAD, MV_JOLT, MV_RUSH, MV_CHARGE}, {1, 1, 1, 1}},

  {"PEBBOON", RIG_BRUTE, MON_STONE, 66, 100, 1,
    50, 42, 56, 26, 205, 18, 26,
    MON_HAB_STONE, SP_GRANILLA, 15, FALSE,
    {152, 156, 150}, {110, 114, 110}, { 86,  90,  86},
    {MV_TACKLE, MV_PEBBLE, MV_RUBBLE, MV_BOULDER}, {1, 1, 12, 21}},

  {"GRANILLA", RIG_BRUTE, MON_STONE, 94, 108, 2,
    64, 58, 72, 38, 104, 35, 7,
    MON_HAB_STONE, SP_BABOULDER, 32, TRUE,
    {138, 142, 136}, { 92,  96,  92}, {168, 120,  72},
    {MV_PEBBLE, MV_RUBBLE, MV_HEADBUTT, MV_BOULDER}, {1, 1, 1, 24}},

  {"BABOULDER", RIG_BRUTE, MON_STONE, 120, 124, 3,
    88, 78, 92, 42, 44, 66, 0,
    MON_HAB_STONE, MON_NONE, 0, FALSE,
    {118, 122, 118}, { 74,  78,  74}, {198, 138,  76},
    {MV_BOULDER, MV_RUBBLE, MV_RUSH, MV_PEBBLE}, {1, 1, 1, 1}}
};
