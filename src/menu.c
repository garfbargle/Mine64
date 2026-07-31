#include "menu.h"
#include "graphics.h"
#include "font.h"
#include "storage.h"
#include "player.h"
#include "main.h"

enum Screen current_screen = MENU;
u32 save_message_cooldown = 0;
u8 save_failed_message = 0;
u8 save_far_message = 0;
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

static char *save_failed_text[] = {
  "Save failed"
};

/* Saving still writes the whole original extent (see saveGame), so it only
   works while all of it is loaded -- near the world's spawn area. */
static char *save_far_text[] = {
  "Too far from spawn to save"
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
  if (chr == 'i' || chr == ':' || chr == '.' || chr == ' ') {
    return 3;
  } else if (chr == 'l') {
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
    case WORLD_NAMING:
      drawWorldNaming();
      return;
    case GAME:
      if (save_failed_message > 0) {
        text = save_failed_text;
        n_lines = sizeof(save_failed_text) / sizeof(char *);
      } else if (save_far_message > 0) {
        text = save_far_text;
        n_lines = sizeof(save_far_text) / sizeof(char *);
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

  if (current_screen == GAME && save_message_cooldown == 0 &&
      save_failed_message == 0 && save_far_message == 0 &&
      player_joined_message == 0 && stick_turns_message == 0) {
    return;
  }

  if (save_message_cooldown > 0) {
    save_message_cooldown--;
  }
  if (save_failed_message > 0) {
    save_failed_message--;
  }
  if (save_far_message > 0) {
    save_far_message--;
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
      /* A slim bar rather than a percentage: it reads at a glance from a
         couch and costs two fill rectangles. */
      u32 width = (worldJobProgress() * 120) / 100;
      gDPPipeSync(dlp++);
      gDPSetCycleType(dlp++, G_CYC_FILL);
      gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
      setMenuFillColor(24, 27, 23);
      gDPFillRectangle(dlp++, 99, 147, 221, 153);
      setMenuFillColor(143, 148, 133);
      if (width > 0) {
        gDPFillRectangle(dlp++, 100, 148, 100 + width, 152);
      }
      gDPPipeSync(dlp++);
      beginText();
    } else if (world_incomplete_message) {
      drawCenteredString("TERRAIN TOO DETAILED TO DRAW", 148);
    } else if (frame_overflows > 0) {
      drawCenteredString("FRAME BUDGET EXCEEDED", 148);
    }
  }
}

void menuDown() {
  if (current_screen == MENU) {
    if (selected_option == 2) {
      selected_option = 0;
    } else {
      selected_option++;
    }
    menu_preview_requested = TRUE;
  }
}

void menuUp() {
  if (current_screen == MENU) {
    if (selected_option == 0) {
      selected_option = 2;
    } else {
      selected_option--;
    }
    menu_preview_requested = TRUE;
  }
}

void menuAct() {
  if (current_screen == MENU) {
    /* Do not launch while the selection is still preparing its replacement
       preview; the currently drawn terrain must always match this slot. */
    if (menu_preview_requested) {
      return;
    }
    game_file_num = selected_option + 1;
    if (files_present[selected_option]) {
      /* The selected save is already loaded for its live preview; only its
         display lists still need the full-detail pass. */
      menu_game_requested = TRUE;
    } else {
      beginWorldNaming();
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

void menuGameStarted() {
  menu_game_requested = FALSE;
  current_screen = GAME;
}

void menuPreviewLoaded() {
  menu_preview_requested = FALSE;
}

u8 menuSelectedWorld() {
  return selected_option;
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
  setWorldName(selected_option, world_name_edit);
  game_file_num = selected_option + 1;
  /* The name belongs to the terrain currently orbiting behind this dialog.
     Saving here makes that candidate a real slot without regenerating it --
     only its display lists are rebuilt at full detail. */
  if (saving_available && !saveGame()) {
    save_failed_message = 120;
  }
  menu_game_requested = TRUE;
}
