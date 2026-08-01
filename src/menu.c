#include "menu.h"
#include "graphics.h"
#include "font.h"
#include "storage.h"
#include "player.h"
#include "math.h"
#include "mods.h"
#include "main.h"

enum Screen current_screen = MENU;
u32 save_message_cooldown = 0;
u8 save_failed_message = 0;
/* Set when a mesh rebuild ran out of arena space, so the world on screen is
   missing columns.  Latched rather than timed: it stays true until a later
   build fits, because the terrain stays wrong for exactly that long. */
u8 world_incomplete_message = 0;
u8 player_joined_message = 0;
u8 player_joined_number = 0;
/* A control scheme changing under the player needs to say so, or the stick
   simply stops doing what they expect. */
u8 stick_turns_message = 0;
u8 stick_turns_enabled_message = 0;

static char *menu_text[] = {
  "MINE64",
  "Choose a world",
  "World 1",
  "World 2",
  "World 3"
};

static u8 option_lines[] = {2, 3, 4};

static u8 selected_option = 0;
/* The renderer uses this to swap the live world behind the selector. */
static u8 menu_preview_requested = TRUE;
/* The picker shows a cheap surface-only mesh.  Entering a world has to compile
   the full cave and ore mesh first, which callbackGfx does once no graphics
   task is in flight -- the stable preview stays on screen until then. */
static u8 menu_game_requested = FALSE;
/*
 * Bumped by every choice that makes the world on screen the wrong world: a
 * different slot, a different terrain shape, a switch the generator reads, a
 * rerolled seed.  The job driver carries the token it started with and drops
 * a build the moment the two disagree, so a change costs the seconds it has
 * spent so far rather than the seconds it has left.
 */
static u8 menu_preview_token = 1;
/*
 * The seed the next generated world will be made from.  Owned here rather
 * than drawn inside beginWorldGeneration so the setup card can show the
 * player the number before they commit to it, and so rerolling is a restart
 * of the same job instead of a second source of entropy.
 */
static u32 pending_seed;
/*
 * A press of A or START that arrived while the world was still being built.
 * Dropping it is what made the picker feel broken -- several seconds where
 * the controller did nothing and nothing said why -- so it is held here and
 * spent the moment the preview lands.
 */
static u8 menu_act_pending;
static u8 setup_row;

/*
 * Committing a named world writes ~200 KB through the cart in one
 * uninterruptible go (see saveGame), which is the last stall in the front
 * end.  It cannot start until a frame saying so is actually on screen, or
 * the console simply stops with the keyboard still up and no explanation --
 * so the stage only advances when drawMenu has built that frame, and the job
 * driver starts the write on the following callback, once the frame has
 * drained.  The stage doubles as the re-entrancy guard: START pressed again
 * during the wait used to trigger a second full save, which is the worst
 * possible answer to "did that register?".
 */
#define WORLD_COMMIT_IDLE 0
#define WORLD_COMMIT_REQUESTED 1
#define WORLD_COMMIT_SHOWN 2
#define WORLD_COMMIT_DONE 3
static u8 world_commit_stage;

static char world_name_edit[WORLD_NAME_LENGTH + 1];
static u8 world_name_cursor;
static u8 world_name_key_row;
static u8 world_name_key_column;

static const char *world_name_keyboard[] = {
  "ABCDEFGHIJ",
  "KLMNOPQRST",
  "UVWXYZ0123",
  "456789"
};

static char *info_text[] = {
  "Mine64 v0.4",
  "",
  "Walk: Stick up / down",
  "Turn: Stick left / right",
  "Sprint: Left shoulder",
  "Look around: Analog stick + Z trigger",
  "Use / place / eat: A button",
  "Mine block: Hold B button",
  "Pickaxe gathers rock / mines faster",
  "Items: C left / right",
  "Camera: C up",
  "Turn or step sideways: Z trigger + C up",
  "Pack: START or C down",
  "Jump: Right shoulder",
  "Save game: D-Pad",
  "Co-op: Controllers 2-4 press START"
};

static char *generating_text[] = {
  "Generating world..."
};

static char *loading_text[] = {
  "Loading world..."
};

static char *saved_text[] = {
  "World saved"
};

/* Capitals: the font atlas has no lowercase 'i' (it lands on a colon), so a
   mixed-case "Saving" would read as "SAV:NG" on the console. */
static char *saving_text[] = {
  "SAVING WORLD"
};

static char *save_failed_text[] = {
  "Save failed"
};

static char *stick_turns_on_text[] = {
  "Left and right turn you"
};

static char *stick_turns_off_text[] = {
  "Left and right step sideways"
};

static char player_joined_text[] = "Player 1 joined";
static char *player_joined_lines[] = {player_joined_text};

static Gfx menu_setup_display_list[] = {
  /*
   * Text can follow terrain primitives, a FILL-mode clear, or the inventory
   * panel, and every one of those leaves the RDP mid-pipe with different
   * attributes.  Changing cycle type, render mode, combine mode and the loaded
   * tile while a primitive is still in flight is an RDP hazard: emulators
   * tolerate it, hardware locks up, and whether it bites depends on how busy
   * the pipe still is -- which is why it tracked world complexity.
   */
  gsDPPipeSync(),
  gsDPSetCycleType(G_CYC_1CYCLE),
  gsDPSetRenderMode(G_RM_NOOP, G_RM_NOOP2),
  gsDPSetCombineMode(G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM),
  gsDPSetPrimColor(0,0,255,255,255,255),
  gsDPSetTexturePersp(G_TP_NONE),
  gsDPSetTextureLUT(G_TT_NONE),
  gsDPLoadTextureTile_4b(font_texture, G_IM_FMT_I, 128, 64,
        0, 0, 32 << 2, 16 << 2,
        0, G_TX_CLAMP, G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK,
        G_TX_NOLOD, G_TX_NOLOD),
  gsSPEndDisplayList()
};

u32 charWidth(char chr) {
  if (chr == 'i' || chr == ':' || chr == '.' || chr == ' ' ||
      chr == '\'' || chr == ',') {
    return 3;
  } else if (chr == 'l' || chr == '!') {
    return 4;
  } else if (chr == 't') {
    return 5;
  } else if (chr == 'k') {
    return 6;
  } else {
    return 7;
  }
}

void drawChar(char chr, u32 x, u32 y) {
  u8 idx = chr - ' ';
  u32 cx = idx % 16;
  u32 cy = (idx / 16) + 2;

  gSPTextureRectangle(dlp++,
    x << 2, y << 2,
    ((x + 8) << 2) - 2, ((y + 8) << 2) - 2,
    G_TX_RENDERTILE,
    (cx * 8) << 5, (cy * 8) << 5,
    1 << 10, 1 << 10);
}

static void drawLargeChar(char chr, u32 x, u32 y, u8 scale) {
  u8 idx = chr - ' ';
  u32 cx = idx % 16;
  u32 cy = (idx / 16) + 2;

  gSPTextureRectangle(dlp++,
    x << 2, y << 2,
    ((x + 8 * scale) << 2) - 2, ((y + 8 * scale) << 2) - 2,
    G_TX_RENDERTILE,
    (cx * 8) << 5, (cy * 8) << 5,
    (1 << 10) / scale, (1 << 10) / scale);
}

static void drawLargeString(const char *text, u32 x, u32 y, u8 scale) {
  while (*text) {
    if (*text != ' ') {
      drawLargeChar(*text, x, y, scale);
    }
    x += charWidth(*text) * scale;
    text++;
  }
}

static void drawMenuTitle() {
  const char *title = "MINE64";
  const char *version = "v0.4";
  u32 width = 0;
  u32 i = 0;

  while (title[i]) {
    width += charWidth(title[i]) * 3;
    i++;
  }
  drawLargeString(title, (SCREEN_WD - width) / 2, 20, 3);

  width = 0;
  for (i = 0; version[i]; i++) {
    width += charWidth(version[i]);
  }
  drawString(version, (SCREEN_WD - width) / 2, 51);
}

static u32 stringWidth(const char *text) {
  u32 width = 0;

  while (*text) {
    width += charWidth(*text);
    text++;
  }
  return width;
}

void drawString(const char *text, u32 x, u32 y) {
  while (*text) {
    if (*text != ' ') {
      drawChar(*text, x, y);
    }
    x += charWidth(*text);
    text++;
  }
}

static void setMenuFillColor(u8 r, u8 g, u8 b) {
  u16 color = GPACK_RGBA5551(r, g, b, 1);
  gDPSetFillColor(dlp++, (color << 16) | color);
}

static void drawCenteredString(const char *text, u32 y) {
  drawString(text, (SCREEN_WD - stringWidth(text)) / 2, y);
}

/*
 * A slim bar rather than a percentage: it reads at a glance from a couch and
 * costs two fill rectangles.  Drawn from both screens a world can be built
 * from -- the picker and the naming keyboard -- because the wait is the same
 * wait, and a naming screen that simply stopped responding to the controller
 * for several seconds was indistinguishable from a hang.  Defined below,
 * beside the rest of the menu drawing.
 */
static void drawWorldJobBar(u32 y);

static u8 worldNameLength() {
  u8 length = WORLD_NAME_LENGTH;

  while (length > 0 && world_name_edit[length - 1] == ' ') {
    length--;
  }
  return length;
}

static u8 worldNameKeyColumns(u8 row) {
  u8 columns = 0;

  while (world_name_keyboard[row][columns]) {
    columns++;
  }
  return columns;
}

static void drawMenuButton(u32 x, u32 y, u8 red, u8 green, u8 blue,
    u8 light_red, u8 light_green, u8 light_blue) {
  /* Pixel circles read more like N64 controller buttons than a modern round
     rectangle, while still being crisp on the console's low resolution. */
  setMenuFillColor(20, 22, 18);
  gDPFillRectangle(dlp++, x - 4, y - 9, x + 4, y + 9);
  gDPFillRectangle(dlp++, x - 7, y - 6, x + 7, y + 6);
  gDPFillRectangle(dlp++, x - 9, y - 3, x + 9, y + 3);
  setMenuFillColor(red, green, blue);
  gDPFillRectangle(dlp++, x - 3, y - 7, x + 3, y + 7);
  gDPFillRectangle(dlp++, x - 6, y - 4, x + 6, y + 4);
  gDPFillRectangle(dlp++, x - 7, y - 2, x + 7, y + 2);
  setMenuFillColor(light_red, light_green, light_blue);
  gDPFillRectangle(dlp++, x - 3, y - 6, x + 3, y - 4);
  gDPFillRectangle(dlp++, x - 5, y - 3, x - 3, y - 2);
}

static void drawMenuStick(u32 x, u32 y) {
  setMenuFillColor(20, 22, 18);
  gDPFillRectangle(dlp++, x - 7, y - 8, x + 7, y + 3);
  gDPFillRectangle(dlp++, x - 4, y - 11, x + 4, y + 6);
  setMenuFillColor(101, 105, 97);
  gDPFillRectangle(dlp++, x - 5, y - 7, x + 5, y + 1);
  gDPFillRectangle(dlp++, x - 3, y - 9, x + 3, y + 4);
  setMenuFillColor(160, 165, 150);
  gDPFillRectangle(dlp++, x - 3, y - 7, x + 3, y - 5);
  gDPFillRectangle(dlp++, x - 1, y + 5, x + 1, y + 9);
}

static void drawStartButton(u32 x, u32 y) {
  setMenuFillColor(20, 22, 18);
  gDPFillRectangle(dlp++, x - 28, y - 7, x + 28, y + 7);
  setMenuFillColor(100, 105, 96);
  gDPFillRectangle(dlp++, x - 26, y - 5, x + 26, y + 5);
  setMenuFillColor(151, 156, 141);
  gDPFillRectangle(dlp++, x - 24, y - 4, x + 24, y - 2);
}

static void setMenuTextColor(u8 red, u8 green, u8 blue) {
  gDPSetPrimColor(dlp++, 0, 0, red, green, blue, 255);
}

/*
 * The setup card.
 *
 * A tall panel down the left rather than a dialog across the middle, because
 * the world being described is orbiting to the right of it and that is the
 * entire point: every row here changes what is on screen, and the player
 * should be watching the world rather than a menu that covers it.  The two
 * sections mirror the two kinds of choice -- one shape, any number of
 * switches -- so the marker shape alone says which is which before a word is
 * read.
 */
#define SETUP_PANEL_LEFT 12
#define SETUP_PANEL_RIGHT 140
#define SETUP_PANEL_TOP 14
#define SETUP_PANEL_BOTTOM 196
#define SETUP_TEXT_X 20
#define SETUP_MARKER_X 122
#define SETUP_STRIP_LEFT 12
#define SETUP_STRIP_RIGHT 308
#define SETUP_STRIP_TOP 200
#define SETUP_STRIP_BOTTOM 234

static const LegendEntry world_setup_legend[] = {
  { BUTTON_ICON_A, BUTTON_ICON_NONE, "PICK" },
  { BUTTON_ICON_R, BUTTON_ICON_NONE, "REROLL" },
  { BUTTON_ICON_B, BUTTON_ICON_NONE, "BACK" },
  { BUTTON_ICON_START, BUTTON_ICON_NONE, "CREATE" }
};

#define WORLD_SETUP_LEGEND_COUNT \
  (sizeof (world_setup_legend) / sizeof (LegendEntry))
#define WORLD_SETUP_LEGEND_Y 215

static u32 worldSetupLegendX(void) {
  return (SETUP_STRIP_LEFT + SETUP_STRIP_RIGHT -
    legendWidth(world_setup_legend, WORLD_SETUP_LEGEND_COUNT)) / 2;
}
#define SETUP_ROW_HEIGHT 12
#define SETUP_TERRAIN_TOP 46
#define SETUP_EXTRAS_HEADER_Y 94
#define SETUP_EXTRAS_TOP 104
/* The lowest a row may sit before its highlight would reach the seed slot. */
#define SETUP_EXTRAS_LAST 170
#define SETUP_EXTRAS_COUNT (WORLD_MOD_COUNT - WORLD_MOD_TERRAIN_COUNT)

/*
 * Rows are spaced to fit rather than laid out at a fixed pitch.
 *
 * The card is a fixed-height panel with a seed slot pinned to its bottom, so
 * every switch the game gains has to come out of the same band.  Deriving the
 * pitch from the table's length means adding a mod is one table row and
 * nothing else -- the alternative is that whoever adds the eleventh switch
 * discovers it by finding it drawn on top of the seed.
 */
static u32 setupRowY(u8 index) {
  u32 pitch = SETUP_EXTRAS_COUNT > 1 ?
    (u32) ((SETUP_EXTRAS_LAST - SETUP_EXTRAS_TOP) /
      (SETUP_EXTRAS_COUNT - 1)) :
    (u32) SETUP_ROW_HEIGHT;

  if (pitch > SETUP_ROW_HEIGHT) {
    pitch = SETUP_ROW_HEIGHT;
  }
  return index < WORLD_MOD_TERRAIN_COUNT ?
    (u32) (SETUP_TERRAIN_TOP + index * SETUP_ROW_HEIGHT) :
    (u32) SETUP_EXTRAS_TOP + (index - WORLD_MOD_TERRAIN_COUNT) * pitch;
}

/* A diamond for one-of-four, a box for a switch: the shape carries the rule,
   so a player who never reads the section headings still cannot expect two
   terrain shapes at once. */
static void drawSetupDiamond(u32 x, u32 y, u8 on, u8 dim) {
  setMenuFillColor(24, 27, 23);
  gDPFillRectangle(dlp++, x + 2, y, x + 6, y + 8);
  gDPFillRectangle(dlp++, x, y + 2, x + 8, y + 6);
  if (!on) {
    return;
  }
  setMenuFillColor(dim ? 92 : 232, dim ? 95 : 196, dim ? 86 : 79);
  gDPFillRectangle(dlp++, x + 3, y + 2, x + 5, y + 6);
  gDPFillRectangle(dlp++, x + 2, y + 3, x + 6, y + 5);
}

static void drawSetupBox(u32 x, u32 y, u8 on, u8 dim) {
  setMenuFillColor(dim ? 48 : 86, dim ? 50 : 90, dim ? 45 : 80);
  gDPFillRectangle(dlp++, x, y, x + 8, y + 8);
  setMenuFillColor(24, 27, 23);
  gDPFillRectangle(dlp++, x + 1, y + 1, x + 7, y + 7);
  if (!on) {
    return;
  }
  /* A stepped tick rather than a solid block: at this size a filled square
     reads as "greyed out" against the unfilled one beside it. */
  setMenuFillColor(dim ? 92 : 232, dim ? 95 : 196, dim ? 86 : 79);
  gDPFillRectangle(dlp++, x + 2, y + 4, x + 4, y + 6);
  gDPFillRectangle(dlp++, x + 3, y + 3, x + 5, y + 5);
  gDPFillRectangle(dlp++, x + 4, y + 2, x + 6, y + 4);
}

static void formatSeed(char *out, u32 value) {
  u8 digit;

  for (digit = 0; digit < 8; digit++) {
    out[digit] = "0123456789ABCDEF"[(value >> ((7 - digit) * 4)) & 15];
  }
  out[8] = 0;
}

static void drawWorldSetup() {
  u8 index;
  char seed_text[9];

  formatSeed(seed_text, menuPendingSeed());

  /* Fills first, then every string: text needs the RDP in a completely
     different configuration, and swapping back and forth mid-card is the
     hazard that locks the console (see menu_setup_display_list). */
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);

  /*
   * The panel is near-black, unlike the cobblestone naming card, and that is
   * a legibility decision rather than a style one: the menu font writes its
   * intensity straight into the framebuffer with no blending, so every glyph
   * carries an opaque black cell with it.  On a light panel each word sits in
   * its own dark box; on this one the boxes are the panel.
   */
  setMenuFillColor(16, 17, 15);
  gDPFillRectangle(dlp++, SETUP_PANEL_LEFT, SETUP_PANEL_TOP,
    SETUP_PANEL_RIGHT, SETUP_PANEL_BOTTOM);
  setMenuFillColor(143, 148, 133);
  gDPFillRectangle(dlp++, SETUP_PANEL_LEFT, SETUP_PANEL_TOP,
    SETUP_PANEL_RIGHT, SETUP_PANEL_TOP + 2);
  gDPFillRectangle(dlp++, SETUP_PANEL_LEFT, SETUP_PANEL_TOP,
    SETUP_PANEL_LEFT + 2, SETUP_PANEL_BOTTOM);
  setMenuFillColor(24, 27, 23);
  gDPFillRectangle(dlp++, SETUP_PANEL_LEFT, SETUP_PANEL_BOTTOM - 2,
    SETUP_PANEL_RIGHT, SETUP_PANEL_BOTTOM);
  gDPFillRectangle(dlp++, SETUP_PANEL_RIGHT - 2, SETUP_PANEL_TOP,
    SETUP_PANEL_RIGHT, SETUP_PANEL_BOTTOM);
  /* A wood plank behind the title, the same board the naming card names the
     world on. */
  setMenuFillColor(77, 48, 26);
  gDPFillRectangle(dlp++, SETUP_PANEL_LEFT + 4, 17, SETUP_PANEL_RIGHT - 4, 30);
  setMenuFillColor(181, 133, 73);
  gDPFillRectangle(dlp++, SETUP_PANEL_LEFT + 4, 17, SETUP_PANEL_RIGHT - 4, 18);
  setMenuFillColor(42, 27, 17);
  gDPFillRectangle(dlp++, SETUP_PANEL_LEFT + 4, 29, SETUP_PANEL_RIGHT - 4, 30);

  for (index = 0; index < WORLD_MOD_COUNT; index++) {
    const WorldMod *mod = &world_mod_table[index];
    u32 row_y = setupRowY(index);
    u8 dim = !worldModAvailable(mod->bit);

    if (index == setup_row) {
      /* A gold edge around the row rather than a gold fill behind it -- see
         the panel colour above for why the middle has to stay dark. */
      setMenuFillColor(216, 191, 77);
      gDPFillRectangle(dlp++, SETUP_PANEL_LEFT + 4, row_y - 2,
        SETUP_PANEL_RIGHT - 4, row_y + 10);
      setMenuFillColor(0, 0, 0);
      gDPFillRectangle(dlp++, SETUP_PANEL_LEFT + 5, row_y - 1,
        SETUP_PANEL_RIGHT - 5, row_y + 9);
    }
    if (mod->group == 0) {
      drawSetupBox(SETUP_MARKER_X, row_y, worldModRowOn(index), dim);
    } else {
      drawSetupDiamond(SETUP_MARKER_X, row_y, worldModRowOn(index), dim);
    }
  }

  /* The seed's own slot, cut into the panel like the name slots opposite. */
  setMenuFillColor(70, 74, 64);
  gDPFillRectangle(dlp++, 62, 182, 136, 194);
  setMenuFillColor(0, 0, 0);
  gDPFillRectangle(dlp++, 63, 183, 135, 193);

  setMenuFillColor(20, 22, 18);
  gDPFillRectangle(dlp++, SETUP_STRIP_LEFT, SETUP_STRIP_TOP,
    SETUP_STRIP_RIGHT, SETUP_STRIP_BOTTOM);
  setMenuFillColor(50, 54, 47);
  gDPFillRectangle(dlp++, SETUP_STRIP_LEFT, SETUP_STRIP_TOP,
    SETUP_STRIP_RIGHT, SETUP_STRIP_TOP + 1);
  if (worldJobActive()) {
    u32 width = (worldJobProgress() * (SETUP_STRIP_RIGHT - SETUP_STRIP_LEFT - 12))
      / 100;

    setMenuFillColor(36, 39, 34);
    gDPFillRectangle(dlp++, SETUP_STRIP_LEFT + 6, 228,
      SETUP_STRIP_RIGHT - 6, 232);
    if (width > 0) {
      setMenuFillColor(143, 148, 133);
      gDPFillRectangle(dlp++, SETUP_STRIP_LEFT + 6, 228,
        SETUP_STRIP_LEFT + 6 + width, 232);
    }
  }
  if (!worldJobActive()) {
    drawLegendIcons(world_setup_legend, WORLD_SETUP_LEGEND_COUNT,
      worldSetupLegendX(), WORLD_SETUP_LEGEND_Y);
  }
  gDPPipeSync(dlp++);

  beginText();
  setMenuTextColor(240, 226, 198);
  drawString("CREATE WORLD",
    (SETUP_PANEL_LEFT + SETUP_PANEL_RIGHT - stringWidth("CREATE WORLD")) / 2,
    20);
  setMenuTextColor(181, 148, 96);
  drawString("WORLD", SETUP_TEXT_X, 34);
  drawString("EXTRAS", SETUP_TEXT_X, SETUP_EXTRAS_HEADER_Y);
  drawString("SEED", SETUP_TEXT_X, 184);

  for (index = 0; index < WORLD_MOD_COUNT; index++) {
    const WorldMod *mod = &world_mod_table[index];

    if (index == setup_row) {
      setMenuTextColor(255, 236, 170);
    } else if (!worldModAvailable(mod->bit)) {
      setMenuTextColor(104, 108, 98);
    } else {
      setMenuTextColor(222, 226, 214);
    }
    drawString(mod->name, SETUP_TEXT_X, setupRowY(index));
  }

  setMenuTextColor(216, 191, 77);
  drawString(seed_text, 68, 184);

  /* The blurb belongs to whatever the cursor is on, so reading down the list
     is how the game explains itself.  There is nowhere else it does. */
  setMenuTextColor(200, 204, 190);
  drawString(world_mod_table[setup_row].blurb, SETUP_STRIP_LEFT + 6, 205);

  if (worldJobActive()) {
    setMenuTextColor(216, 191, 77);
    drawString(menuActPending() ? "BUILDING WORLD  -  START HELD" :
      "BUILDING WORLD", SETUP_STRIP_LEFT + 6, 217);
  } else {
    setMenuTextColor(150, 155, 142);
    drawLegendLabels(world_setup_legend, WORLD_SETUP_LEGEND_COUNT,
      worldSetupLegendX(), WORLD_SETUP_LEGEND_Y);
  }
  setMenuTextColor(255, 255, 255);
}

static void drawWorldNaming() {
  u8 i;
  u8 row;
  u8 column;
  u32 slot_x = 58;
  u32 key_x = 75;
  u32 key_y = 116;

  /* Cobblestone, wood, and block keys mirror the game's own materials while
     the compact card still leaves the terrain preview in view. */
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setMenuFillColor(50, 54, 47);
  gDPFillRectangle(dlp++, 29, 26, 290, 214);
  setMenuFillColor(143, 148, 133);
  gDPFillRectangle(dlp++, 29, 26, 290, 28);
  gDPFillRectangle(dlp++, 29, 26, 31, 214);
  setMenuFillColor(24, 27, 23);
  gDPFillRectangle(dlp++, 29, 212, 290, 214);
  gDPFillRectangle(dlp++, 288, 26, 290, 214);
  /* Cobblestone seams and chips in the exposed border. */
  setMenuFillColor(36, 39, 34);
  gDPFillRectangle(dlp++, 32, 51, 45, 53);
  gDPFillRectangle(dlp++, 32, 116, 45, 118);
  gDPFillRectangle(dlp++, 32, 180, 45, 182);
  gDPFillRectangle(dlp++, 274, 67, 287, 69);
  gDPFillRectangle(dlp++, 274, 132, 287, 134);
  gDPFillRectangle(dlp++, 274, 195, 287, 197);

  setMenuFillColor(77, 48, 26);
  gDPFillRectangle(dlp++, 47, 67, 272, 98);
  setMenuFillColor(181, 133, 73);
  gDPFillRectangle(dlp++, 47, 67, 272, 68);
  gDPFillRectangle(dlp++, 47, 67, 49, 98);
  setMenuFillColor(42, 27, 17);
  gDPFillRectangle(dlp++, 47, 97, 272, 98);
  gDPFillRectangle(dlp++, 270, 67, 272, 98);
  setMenuFillColor(109, 69, 35);
  gDPFillRectangle(dlp++, 52, 70, 70, 72);
  gDPFillRectangle(dlp++, 235, 93, 260, 95);

  for (i = 0; i < WORLD_NAME_LENGTH; i++) {
    u32 x = slot_x + i * 17;
    /* At the 12-character limit, the insertion point sits just beyond the
       last slot; retaining the final highlight keeps that state visible. */
    u8 selected = i == world_name_cursor ||
      (world_name_cursor == WORLD_NAME_LENGTH &&
        i == WORLD_NAME_LENGTH - 1);

    setMenuFillColor(selected ? 230 : 42, selected ? 204 : 45,
      selected ? 80 : 38);
    gDPFillRectangle(dlp++, x, 76, x + 14, 92);
    setMenuFillColor(selected ? 91 : 20, selected ? 72 : 23,
      selected ? 17 : 17);
    gDPFillRectangle(dlp++, x + 2, 78, x + 12, 90);
  }

  for (row = 0; row < sizeof(world_name_keyboard) / sizeof(char *); row++) {
    for (column = 0; world_name_keyboard[row][column]; column++) {
      u32 x = key_x + column * 17;
      u32 y = key_y + row * 16;
      u8 selected = row == world_name_key_row &&
        column == world_name_key_column;

      setMenuFillColor(selected ? 216 : 49, selected ? 191 : 53,
        selected ? 77 : 47);
      gDPFillRectangle(dlp++, x, y, x + 15, y + 13);
      setMenuFillColor(selected ? 93 : 102, selected ? 78 : 106,
        selected ? 28 : 96);
      gDPFillRectangle(dlp++, x + 2, y + 2, x + 13, y + 11);
      setMenuFillColor(selected ? 143 : 154, selected ? 122 : 159,
        selected ? 43 : 145);
      gDPFillRectangle(dlp++, x + 3, y + 3, x + 12, y + 4);
    }
  }
  drawMenuButton(63, 188, 54, 145, 66, 105, 203, 95);
  drawMenuButton(167, 188, 175, 48, 41, 232, 87, 71);
  drawMenuStick(48, 204);
  drawMenuButton(120, 204, 183, 142, 43, 235, 196, 79);
  drawMenuButton(140, 204, 183, 142, 43, 235, 196, 79);
  drawStartButton(244, 204);
  gDPPipeSync(dlp++);

  beginText();
  drawCenteredString("CREATE WORLD", 39);
  drawCenteredString("YOUR WORLD NAME", 57);
  for (i = 0; i < WORLD_NAME_LENGTH; i++) {
    if (world_name_edit[i] != ' ') {
      drawChar(world_name_edit[i], slot_x + i * 17 + 4, 80);
    }
  }
  drawCenteredString("SELECT A BLOCK LETTER", 103);
  for (row = 0; row < sizeof(world_name_keyboard) / sizeof(char *); row++) {
    for (column = 0; world_name_keyboard[row][column]; column++) {
      drawChar(world_name_keyboard[row][column], key_x + column * 17 + 4,
        key_y + row * 16 + 3);
    }
  }
  drawChar('B', 60, 184);
  drawString("DELETE", 77, 184);
  drawChar('A', 164, 184);
  drawString("ADD", 181, 184);
  drawString("Z BACK", 216, 184);
  drawString("KEY", 65, 200);
  drawChar('C', 117, 200);
  drawChar('C', 137, 200);
  drawString("CURSOR", 151, 200);
  drawString("START", 227, 200);
  if (!saving_available) {
    drawCenteredString(storageStatusText(), 205);
  }
}

void beginText() {
  gSPDisplayList(dlp++, menu_setup_display_list);
}

static void drawWorldJobBar(u32 y) {
  u32 width = (worldJobProgress() * 120) / 100;

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setMenuFillColor(24, 27, 23);
  gDPFillRectangle(dlp++, 99, y, 221, y + 6);
  setMenuFillColor(143, 148, 133);
  if (width > 0) {
    gDPFillRectangle(dlp++, 100, y + 1, 100 + width, y + 5);
  }
  gDPPipeSync(dlp++);
}

void drawMenu() {
  u32 i, j, x, center;
  char chr;
  u8 option_y;
  char **text;
  char *text_line;
  u32 n_lines;
  u32 y_start = SCREEN_HT / 6;

  switch (current_screen) {
    case MENU:
      text = menu_text;
      n_lines = sizeof(menu_text) / sizeof(char *);
      y_start = 170;
      break;
    case INFO:
      text = info_text;
      n_lines = sizeof(info_text) / sizeof(char *);
      break;
    case GENERATING:
      text = generating_text;
      n_lines = sizeof(generating_text) / sizeof(char *);
      y_start = SCREEN_HT / 3;
      break;
    case LOADING:
      text = loading_text;
      n_lines = sizeof(loading_text) / sizeof(char *);
      y_start = SCREEN_HT / 3;
      break;
    case LOADING_PREVIEW:
      text = loading_text;
      n_lines = sizeof(loading_text) / sizeof(char *);
      y_start = 18;
      break;
    case WORLD_SETUP:
      drawWorldSetup();
      return;
    case WORLD_NAMING:
      /*
       * START has been pressed and the world is being committed.  The name is
       * already taken and every key on the card is inert, so leaving it up
       * makes a dead screen look like a live one -- and it covers the world
       * that is being written, which is the only thing here worth watching.
       * The bar sits where the picker puts it, so walking title -> setup ->
       * name -> commit never moves it.
       *
       * Building this frame is also what releases the write: saveGame stops
       * the console for as long as the cart takes, so it may not start until
       * the player can see why.  See WORLD_COMMIT_* above.
       */
      if (world_commit_stage != WORLD_COMMIT_IDLE) {
        if (world_commit_stage == WORLD_COMMIT_REQUESTED) {
          world_commit_stage = WORLD_COMMIT_SHOWN;
        }
        beginText();
        drawCenteredString("CREATING WORLD", 124);
        drawCenteredString(world_names[selected_option], 140);
        if (worldJobActive()) {
          drawWorldJobBar(158);
        } else if (saving_available) {
          drawCenteredString("SAVING TO CART", 158);
        }
        beginText();
        return;
      }
      drawWorldNaming();
      return;
    case GAME:
      if (worldJobActive()) {
        /* The only long job that can run under gameplay is a save.  It takes
           a couple of seconds now instead of stopping the console, which
           means it needs to say it is happening. */
        text = saving_text;
        n_lines = 1;
      } else if (save_failed_message > 0) {
        text = save_failed_text;
        n_lines = sizeof(save_failed_text) / sizeof(char *);
      } else if (save_message_cooldown > 0) {
        text = saved_text;
        n_lines = sizeof(saved_text) / sizeof(char *);
      } else if (stick_turns_message > 0) {
        text = stick_turns_enabled_message ?
          stick_turns_on_text : stick_turns_off_text;
        n_lines = 1;
      } else {
        player_joined_text[7] = '0' + player_joined_number;
        text = player_joined_lines;
        n_lines = 1;
      }
      y_start = SCREEN_HT / 3;
      break;
    case INVENTORY:
      /* Inventory owns its complete visual hierarchy in graphics.c. */
      return;
    default:
      return;
  }

  if (current_screen == GAME && !worldJobActive() &&
      save_message_cooldown == 0 &&
      save_failed_message == 0 &&
      player_joined_message == 0 && stick_turns_message == 0) {
    return;
  }

  if (save_message_cooldown > 0) {
    save_message_cooldown--;
  }
  if (save_failed_message > 0) {
    save_failed_message--;
  }
  if (player_joined_message > 0) {
    player_joined_message--;
  }
  if (stick_turns_message > 0) {
    stick_turns_message--;
  }

  beginText();

  for (i = 0; i < n_lines; i++) {
    if (current_screen == MENU && i == 0) {
      drawMenuTitle();
      continue;
    }
    if (current_screen == WORLD_NAMING && !saving_available && i == 5) {
      text_line = storageStatusText();
    } else if (current_screen == MENU && i >= option_lines[0]) {
      u8 world = i - option_lines[0];
      text_line = files_present[world] ? world_names[world] : "New World";
    } else {
      text_line = text[i];
    }

    j = 0;
    center = 0;
    while (text_line[j]) {
      chr = text_line[j];
      center += charWidth(chr);

      if (chr == ':') {
        break;
      }

      j++;
    }

    if (text_line[j]) {
      center += charWidth(' ') / 2;
    } else {
      center /= 2;
    }

    j = 0;
    x = 0;
    while (text_line[j]) {
      chr = text_line[j];
      if (chr != ' ') {
        drawChar(chr, x + SCREEN_WD / 2 - center, i * 12 + y_start);
      }

      x += charWidth(chr);
      j++;
    }
  }

  if (current_screen == MENU) {
    option_y = option_lines[selected_option] * 12 + y_start;
    drawChar('>', SCREEN_WD / 2 - 40 - charWidth('>'), option_y);
    drawChar('<', SCREEN_WD / 2 + 40, option_y);
    if (worldJobActive()) {
      drawWorldJobBar(147);
      beginText();
      if (menuActPending()) {
        /* The player has already committed to this slot.  Saying so is the
           difference between a console that is working and one that has
           stopped listening. */
        drawCenteredString("ENTERING WHEN READY", 132);
      }
    } else if (world_incomplete_message) {
      drawCenteredString("TERRAIN TOO DETAILED TO DRAW", 148);
    } else if (frame_overflows > 0) {
      drawCenteredString("FRAME BUDGET EXCEEDED", 148);
    } else {
      /* What the world orbiting behind the list is made of.  For an empty
         slot that is the setup card's current choice, which is how the
         feature announces itself before anyone opens the card. */
      drawCenteredString(worldTerrainName(), 148);
    }
  }
}

/*
 * Declare the world on screen out of date.  Every caller has just changed
 * something the generator reads, so the running build -- if any -- is already
 * producing the wrong terrain and the driver will drop it on its next
 * callback.
 */
static void invalidatePreview() {
  menu_preview_token++;
  menu_preview_requested = TRUE;
}

/* Moving onto an empty slot draws it a world of its own, so scrolling off a
   candidate and back is itself a reroll. */
static void selectWorld(u8 option) {
  selected_option = option;
  if (!files_present[selected_option]) {
    pending_seed = (u32) osGetTime();
  }
  invalidatePreview();
}

void menuDown() {
  if (current_screen == MENU) {
    selectWorld(selected_option == 2 ? 0 : selected_option + 1);
  }
}

void menuUp() {
  if (current_screen == MENU) {
    selectWorld(selected_option == 0 ? 2 : selected_option - 1);
  }
}

void menuAct() {
  if (current_screen == MENU) {
    /*
     * The terrain drawn must always be the terrain this slot will give, so a
     * slot whose preview is still building cannot be entered yet -- but the
     * press is kept rather than thrown away.  Dropping it silently is what
     * made several seconds of every visit to this screen feel like a dead
     * controller.
     */
    if (menu_preview_requested) {
      menu_act_pending = TRUE;
      return;
    }
    menu_act_pending = FALSE;
    game_file_num = selected_option + 1;
    if (files_present[selected_option]) {
      /* The selected save is already loaded, meshed and orbiting: entering it
         is a change of screen and lens, nothing more. */
      menu_game_requested = TRUE;
    } else {
      beginWorldSetup();
    }
  } else if (current_screen == WORLD_SETUP) {
    if (menu_preview_requested) {
      menu_act_pending = TRUE;
      return;
    }
    menu_act_pending = FALSE;
    beginWorldNaming();
  } else if (current_screen == INFO) {
    current_screen = MENU;
  }
}

void menuBack() {
  if (current_screen == WORLD_SETUP) {
    menu_act_pending = FALSE;
    current_screen = MENU;
  } else if (current_screen == WORLD_NAMING) {
    /* Only until the world is committed; after that there is a file on the
       cart and nothing to go back to. */
    if (world_commit_stage == WORLD_COMMIT_IDLE) {
      current_screen = WORLD_SETUP;
    }
  } else if (current_screen == INFO) {
    current_screen = MENU;
  }
}

u8 menuPreviewRequested() {
  return menu_preview_requested;
}

u8 menuGameRequested() {
  return menu_game_requested;
}

u8 menuPreviewToken() {
  return menu_preview_token;
}

u32 menuPendingSeed() {
  /* Drawn on demand rather than at startup, because the boot sequence has no
     entropy worth the name yet and the first preview begins before the player
     has touched anything.  Zero is the "not yet drawn" marker, which costs
     one seed out of four billion. */
  if (pending_seed == 0) {
    pending_seed = (u32) osGetTime();
  }
  return pending_seed;
}

u8 menuActPending() {
  return menu_act_pending;
}

void menuGameStarted() {
  menu_game_requested = FALSE;
  menu_act_pending = FALSE;
  current_screen = GAME;
}

void menuPreviewLoaded() {
  menu_preview_requested = FALSE;
  if (!menu_act_pending) {
    return;
  }
  /* Spend the held press on the screen that took it.  menuAct now finds the
     preview ready and goes through, which is what the player asked for
     however many seconds ago. */
  menu_act_pending = FALSE;
  if (current_screen == MENU || current_screen == WORLD_SETUP) {
    menuAct();
  }
}

u8 menuSelectedWorld() {
  return selected_option;
}

void beginWorldSetup() {
  setup_row = 0;
  world_commit_stage = WORLD_COMMIT_IDLE;
  current_screen = WORLD_SETUP;
}

void worldSetupUp() {
  setup_row = setup_row == 0 ? WORLD_MOD_COUNT - 1 : setup_row - 1;
}

void worldSetupDown() {
  setup_row = (u8) ((setup_row + 1) % WORLD_MOD_COUNT);
}

void worldSetupToggle() {
  /* Only a change the generator would read costs a rebuild; the behaviour
     mods are read while the world is played, so flipping one is instant. */
  if (toggleWorldMod(setup_row) && world_mod_table[setup_row].regenerates) {
    invalidatePreview();
  }
}

void worldSetupReroll() {
  pending_seed = (u32) osGetTime();
  invalidatePreview();
}

u8 worldSetupRow() {
  return setup_row;
}

u8 menuCommitReady() {
  return world_commit_stage == WORLD_COMMIT_SHOWN;
}

/*
 * A save has finished.  The same write serves two callers and they want
 * opposite things from it: a commit is the last step of creating a world and
 * moves the player into it, while an in-game save is an interruption that
 * should say so briefly and leave everything as it was.
 */
void menuSaveFinished(u8 ok) {
  if (world_commit_stage == WORLD_COMMIT_SHOWN) {
    /* The name belongs to the terrain that has been orbiting since the setup
       card, so there is nothing left to build: the world is already meshed
       and the player drops into the shot they were looking at. */
    world_commit_stage = WORLD_COMMIT_DONE;
    if (!ok) {
      save_failed_message = 120;
    }
    menu_game_requested = TRUE;
    return;
  }
  if (ok) {
    save_message_cooldown = 60;
  } else {
    saving_available = FALSE;
    save_failed_message = 120;
  }
}

void beginWorldNaming() {
  u8 i;
  const char *default_name = "NEW WORLD";

  for (i = 0; i < WORLD_NAME_LENGTH; i++) {
    world_name_edit[i] = ' ';
  }
  /* Empty save slots inherit a storage-side "World N" label for the menu.
     A new-world editor should instead start with an obvious editable name. */
  for (i = 0; i < WORLD_NAME_LENGTH && default_name[i]; i++) {
    world_name_edit[i] = default_name[i];
  }
  world_name_edit[WORLD_NAME_LENGTH] = 0;
  world_name_cursor = worldNameLength();
  world_name_key_row = 0;
  world_name_key_column = 0;
  current_screen = WORLD_NAMING;
}

void worldNameKeyboardLeft() {
  u8 columns = worldNameKeyColumns(world_name_key_row);

  world_name_key_column = world_name_key_column == 0 ? columns - 1 :
    world_name_key_column - 1;
}

void worldNameKeyboardRight() {
  u8 columns = worldNameKeyColumns(world_name_key_row);

  world_name_key_column = (world_name_key_column + 1) % columns;
}

void worldNameKeyboardUp() {
  if (world_name_key_row == 0) {
    world_name_key_row = sizeof(world_name_keyboard) / sizeof(char *) - 1;
  } else {
    world_name_key_row--;
  }
  if (world_name_key_column >= worldNameKeyColumns(world_name_key_row)) {
    world_name_key_column = worldNameKeyColumns(world_name_key_row) - 1;
  }
}

void worldNameKeyboardDown() {
  world_name_key_row = (world_name_key_row + 1) %
    (sizeof(world_name_keyboard) / sizeof(char *));
  if (world_name_key_column >= worldNameKeyColumns(world_name_key_row)) {
    world_name_key_column = worldNameKeyColumns(world_name_key_row) - 1;
  }
}

void worldNameCursorLeft() {
  if (world_name_cursor > 0) {
    world_name_cursor--;
  }
}

void worldNameCursorRight() {
  if (world_name_cursor < worldNameLength()) {
    world_name_cursor++;
  }
}

void worldNameInsertCharacter() {
  u8 i;

  if (worldNameLength() >= WORLD_NAME_LENGTH) {
    return;
  }
  for (i = WORLD_NAME_LENGTH - 1; i > world_name_cursor; i--) {
    world_name_edit[i] = world_name_edit[i - 1];
  }
  world_name_edit[world_name_cursor] =
    world_name_keyboard[world_name_key_row][world_name_key_column];
  world_name_cursor++;
}

void worldNameErase() {
  u8 i;

  if (world_name_cursor == 0) {
    return;
  }
  world_name_cursor--;
  for (i = world_name_cursor; i < WORLD_NAME_LENGTH - 1; i++) {
    world_name_edit[i] = world_name_edit[i + 1];
  }
  world_name_edit[WORLD_NAME_LENGTH - 1] = ' ';
}

void confirmWorldName() {
  /* Once is enough.  This used to run the whole save again on every extra
     press, and extra presses are exactly what a player does when a screen
     stops responding -- so the honest answer to "did that register?" was to
     make the wait several times longer. */
  if (world_commit_stage != WORLD_COMMIT_IDLE) {
    return;
  }
  setWorldName(selected_option, world_name_edit);
  game_file_num = selected_option + 1;
  /* The mask is final now, and the write below is what puts the pack on the
     cart, so the starting items have to be in it before the save runs. */
  resetStartingInventories();
  /* Announce only.  drawMenu puts the card up and the job driver starts the
     write once that frame has been shown; see WORLD_COMMIT_*. */
  world_commit_stage = WORLD_COMMIT_REQUESTED;
}
