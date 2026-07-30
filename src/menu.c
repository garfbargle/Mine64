#include "menu.h"
#include "graphics.h"
#include "font.h"
#include "storage.h"
#include "player.h"

enum Screen current_screen = MENU;
u32 save_message_cooldown = 0;
u8 save_failed_message = 0;
u8 player_joined_message = 0;
u8 player_joined_number = 0;

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
  "Mine64 v0.3",
  "",
  "Walk: Analog stick",
  "Sprint: Left shoulder",
  "Look around: Analog stick + Z trigger",
  "Place block: A button (items are limited)",
  "Mine block: Hold B button",
  "Pickaxe gathers rock / mines faster",
  "Select block: C buttons left/right",
  "Camera: C button up",
  "Inventory: START button",
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

static char player_joined_text[] = "Player 1 joined";
static char *player_joined_lines[] = {player_joined_text};

static char *inventory_text[] = {
  "Inventory",
  "Craft",
  "Output",
  "Items",
  "A: Move Stack",
  "B: Move One",
  "D Pad C Stick: Move",
  "Hold a direction: Repeat",
  "Yellow Cursor Green Equipped",
  "Start: Close"
};

static Gfx menu_setup_display_list[] = {
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
  const char *version = "v0.3";
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

static void drawWorldNaming() {
  u8 i;
  u8 row;
  u8 column;
  u32 slot_x = 58;
  u32 key_x = 75;
  u32 key_y = 116;

  /* This is intentionally a solid, arcade-like card: it stays legible over
     every terrain preview while letting the spinning world remain visible. */
  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  setMenuFillColor(8, 19, 33);
  gDPFillRectangle(dlp++, 29, 26, 290, 214);
  setMenuFillColor(72, 168, 190);
  gDPFillRectangle(dlp++, 29, 26, 290, 28);
  gDPFillRectangle(dlp++, 29, 212, 290, 214);
  gDPFillRectangle(dlp++, 29, 26, 31, 214);
  gDPFillRectangle(dlp++, 288, 26, 290, 214);

  setMenuFillColor(16, 43, 62);
  gDPFillRectangle(dlp++, 47, 67, 272, 98);
  setMenuFillColor(104, 204, 210);
  gDPFillRectangle(dlp++, 47, 67, 272, 68);
  gDPFillRectangle(dlp++, 47, 97, 272, 98);

  for (i = 0; i < WORLD_NAME_LENGTH; i++) {
    u32 x = slot_x + i * 17;
    /* At the 12-character limit, the insertion point sits just beyond the
       last slot; retaining the final highlight keeps that state visible. */
    u8 selected = i == world_name_cursor ||
      (world_name_cursor == WORLD_NAME_LENGTH &&
        i == WORLD_NAME_LENGTH - 1);

    setMenuFillColor(selected ? 230 : 34, selected ? 212 : 79,
      selected ? 90 : 102);
    gDPFillRectangle(dlp++, x, 76, x + 14, 92);
    setMenuFillColor(selected ? 82 : 12, selected ? 72 : 29,
      selected ? 21 : 44);
    gDPFillRectangle(dlp++, x + 2, 78, x + 12, 90);
  }

  for (row = 0; row < sizeof(world_name_keyboard) / sizeof(char *); row++) {
    for (column = 0; world_name_keyboard[row][column]; column++) {
      u32 x = key_x + column * 17;
      u32 y = key_y + row * 16;
      u8 selected = row == world_name_key_row &&
        column == world_name_key_column;

      setMenuFillColor(selected ? 102 : 31, selected ? 214 : 91,
        selected ? 192 : 122);
      gDPFillRectangle(dlp++, x, y, x + 15, y + 13);
      setMenuFillColor(selected ? 20 : 10, selected ? 53 : 25,
        selected ? 48 : 37);
      gDPFillRectangle(dlp++, x + 2, y + 2, x + 13, y + 11);
    }
  }
  gDPPipeSync(dlp++);

  beginText();
  drawCenteredString("NAME WORLD", 39);
  drawCenteredString("YOUR WORLD NAME", 57);
  for (i = 0; i < WORLD_NAME_LENGTH; i++) {
    if (world_name_edit[i] != ' ') {
      drawChar(world_name_edit[i], slot_x + i * 17 + 4, 80);
    }
  }
  drawCenteredString("PICK A LETTER", 103);
  for (row = 0; row < sizeof(world_name_keyboard) / sizeof(char *); row++) {
    for (column = 0; world_name_keyboard[row][column]; column++) {
      drawChar(world_name_keyboard[row][column], key_x + column * 17 + 4,
        key_y + row * 16 + 3);
    }
  }
  drawCenteredString("A: ADD   B: DELETE", 184);
  drawCenteredString("C LR: CURSOR   START: CREATE", 197);
  if (!saving_available) {
    drawCenteredString("NO CART SAVE DEVICE", 205);
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
      } else if (save_message_cooldown > 0) {
        text = saved_text;
        n_lines = sizeof(saved_text) / sizeof(char *);
      } else {
        player_joined_text[7] = '0' + player_joined_number;
        text = player_joined_lines;
        n_lines = 1;
      }
      y_start = SCREEN_HT / 3;
      break;
    case INVENTORY:
      text = inventory_text;
      n_lines = sizeof(inventory_text) / sizeof(char *);
      y_start = 26;
      break;
    default:
      return;
  }

  if (current_screen == GAME && save_message_cooldown == 0 &&
      save_failed_message == 0 && player_joined_message == 0) {
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

  beginText();

  for (i = 0; i < n_lines; i++) {
    if (current_screen == MENU && i == 0) {
      drawMenuTitle();
      continue;
    }
    if (current_screen == INVENTORY && i == 0) {
      static char inventory_title[] = "P1 Inventory";
      inventory_title[1] = '1' + inventory_player;
      text_line = inventory_title;
    } else if (current_screen == WORLD_NAMING && !saving_available && i == 5) {
      text_line = "No cart save device";
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
        if (current_screen == INVENTORY && i == 1) {
          drawChar(chr, x + (players[inventory_player].crafting_table_open ? 40 : 48), 43);
        } else if (current_screen == INVENTORY && i == 2) {
          drawChar(chr, x + (players[inventory_player].crafting_table_open ? 104 : 86), 43);
        } else if (current_screen == INVENTORY && i == 3) {
          drawChar(chr, x + (players[inventory_player].crafting_table_open ? 145 : 115), 43);
        } else if (current_screen == INVENTORY && i > 3) {
          drawChar(chr, x + 42, (i - 4) * 10 + 142);
        } else {
          drawChar(chr, x + SCREEN_WD / 2 - center, i * 12 + y_start);
        }
      }

      x += charWidth(chr);
      j++;
    }
  }

  if (current_screen == MENU) {
    option_y = option_lines[selected_option] * 12 + y_start;
    drawChar('>', SCREEN_WD / 2 - 40 - charWidth('>'), option_y);
    drawChar('<', SCREEN_WD / 2 + 40, option_y);
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
      /* The selected save is already loaded for its live preview. */
      current_screen = GAME;
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
     Saving here makes that candidate a real slot without regenerating it. */
  if (saving_available && !saveGame()) {
    save_failed_message = 120;
  }
  current_screen = GAME;
}
