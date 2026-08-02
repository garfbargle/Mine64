#include <nusys.h>

#include "pause.h"
#include "audio.h"
#include "camera.h"
#include "day_cycle.h"
#include "graphics.h"
#include "main.h"
#include "math.h"
#include "menu.h"
#include "mods.h"
#include "player.h"
#include "rules.h"
#include "storage.h"

/* player.c owns these; the pause pages are a second reader of the same
   bitmask rather than a second decoder of the same stick. */
#define NAV_LEFT  0x01
#define NAV_RIGHT 0x02
#define NAV_UP    0x04
#define NAV_DOWN  0x08

u8 pause_tab = PAUSE_TAB_PACK;

/*
 * The panel the pack already drew, now shared.
 *
 * The band across the top was the pack's title bar; it is the tab strip now,
 * which is why the pack's own title text went away -- the strip says PACK in
 * the same place the word used to be, and says it as one of four.
 */
#define PAUSE_PANEL_LEFT 6
#define PAUSE_PANEL_TOP 6
#define PAUSE_STRIP_LEFT 12
#define PAUSE_STRIP_RIGHT (SCREEN_WD - 13)
#define PAUSE_STRIP_TOP 12
#define PAUSE_STRIP_BOTTOM 31
#define PAUSE_TAB_PAD 6
#define PAUSE_TAB_GAP 4
/* Clear of the shoulder sprites at either end of the strip. */
#define PAUSE_SHOULDER_X 15
#define PAUSE_SHOULDER_GAP 6

/* A page's writable area, below the strip. */
#define PAGE_LEFT 16
#define PAGE_RIGHT (SCREEN_WD - 17)
#define ROW_PITCH 14
#define ROW_HEIGHT 12
#define ROW_LABEL_X 26
#define ROW_VALUE_X 186
/* An 8px glyph box centred against a 12px row. */
#define ROW_TEXT_DROP 2
#define BLURB_Y 196
#define PAUSE_LEGEND_Y 210

/* Pips for a volume, a track and a knob for the fog edge.  Both are fills, so
   both are laid out here and drawn from the fills phase only. */
#define PIP_SIZE 8
#define PIP_GAP 4
#define SLIDER_WIDTH 80
#define SLIDER_KNOB 6

static void setPauseFillColor(u8 r, u8 g, u8 b) {
  u32 color = GPACK_RGBA5551(r, g, b, 1);
  gDPSetFillColor(dlp++, (color << 16) | color);
}

static void setPauseTextColor(u8 r, u8 g, u8 b) {
  gDPSetPrimColor(dlp++, 0, 0, r, g, b, 255);
}

static u32 pauseStringWidth(const char *text) {
  u32 width = 0;

  while (*text) {
    width += charWidth(*text);
    text++;
  }
  return width;
}

static u32 drawPauseUnsigned(u32 value, u32 x, u32 y) {
  char digits[6];
  u8 count = 0;
  u8 i;

  do {
    digits[count++] = (char) ('0' + value % 10);
    value /= 10;
  } while (value > 0 && count < sizeof(digits));
  for (i = 0; i < count; i++) {
    char digit = digits[count - i - 1];

    drawChar(digit, x, y);
    x += charWidth(digit);
  }
  return x;
}

/*
 * The tab strip.
 *
 * Every label is measured from the same table in both phases, so the words
 * land inside the cells the fills drew for them.  The highlighted tab is a
 * bright edge around a dark middle rather than a bright fill: the font writes
 * its intensity straight into the framebuffer with no blending, so a glyph on
 * a light cell arrives inside an opaque black box.  The setup card learned
 * this the same way.
 */
static const char *const pause_tab_names[PAUSE_TAB_COUNT] = {
  "PACK", "OPTIONS", "WORLD", "CONTROLS"
};

static u32 tabCellWidth(u8 tab) {
  return pauseStringWidth(pause_tab_names[tab]) + PAUSE_TAB_PAD * 2;
}

static u32 tabStripX(void) {
  u32 left = PAUSE_SHOULDER_X + buttonIconWidth(BUTTON_ICON_L) +
    PAUSE_SHOULDER_GAP;
  u32 right = PAUSE_STRIP_RIGHT - 3 - buttonIconWidth(BUTTON_ICON_R) -
    PAUSE_SHOULDER_GAP;
  u32 width = 0;
  u8 i;

  for (i = 0; i < PAUSE_TAB_COUNT; i++) {
    width += tabCellWidth(i);
    if (i + 1 < PAUSE_TAB_COUNT) {
      width += PAUSE_TAB_GAP;
    }
  }
  return left + (right - left - width) / 2;
}

static u32 tabCellX(u8 tab) {
  u32 x = tabStripX();
  u8 i;

  for (i = 0; i < tab; i++) {
    x += tabCellWidth(i) + PAUSE_TAB_GAP;
  }
  return x;
}

void drawPauseFrame(void) {
  u32 x;
  u8 i;

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  /* A single full-screen workbench with strong internal grouping survives
     composite blur better than nested translucent boxes. */
  setPauseFillColor(7, 9, 12);
  gDPFillRectangle(dlp++, PAUSE_PANEL_LEFT, PAUSE_PANEL_TOP, SCREEN_WD - 7,
    SCREEN_HT - 7);
  setPauseFillColor(115, 121, 117);
  gDPFillRectangle(dlp++, 8, 8, SCREEN_WD - 9, SCREEN_HT - 9);
  setPauseFillColor(26, 31, 33);
  gDPFillRectangle(dlp++, 11, 11, SCREEN_WD - 12, SCREEN_HT - 12);
  /* Darker than the title bar it replaces: four tab labels sit on this band,
     and each glyph brings its own opaque cell with it. */
  setPauseFillColor(18, 22, 24);
  gDPFillRectangle(dlp++, PAUSE_STRIP_LEFT, PAUSE_STRIP_TOP,
    PAUSE_STRIP_RIGHT, PAUSE_STRIP_BOTTOM);
  setPauseFillColor(69, 75, 74);
  gDPFillRectangle(dlp++, PAUSE_STRIP_LEFT, PAUSE_STRIP_BOTTOM - 1,
    PAUSE_STRIP_RIGHT, PAUSE_STRIP_BOTTOM);

  for (i = 0; i < PAUSE_TAB_COUNT; i++) {
    if (i != pause_tab) {
      continue;
    }
    x = tabCellX(i);
    setPauseFillColor(216, 191, 77);
    gDPFillRectangle(dlp++, x, PAUSE_STRIP_TOP + 2, x + tabCellWidth(i),
      PAUSE_STRIP_BOTTOM - 3);
    setPauseFillColor(9, 11, 13);
    gDPFillRectangle(dlp++, x + 1, PAUSE_STRIP_TOP + 3,
      x + tabCellWidth(i) - 1, PAUSE_STRIP_BOTTOM - 4);
  }

  /* The two buttons that move between the tabs, at the two ends of the thing
     they move along.  A shoulder is 11 tall against a 20-tall band. */
  drawButtonIcon(BUTTON_ICON_L, PAUSE_SHOULDER_X, PAUSE_STRIP_TOP + 5);
  drawButtonIcon(BUTTON_ICON_R,
    PAUSE_STRIP_RIGHT - 3 - buttonIconWidth(BUTTON_ICON_R),
    PAUSE_STRIP_TOP + 5);
}

void drawPauseFrameText(void) {
  u8 i;

  for (i = 0; i < PAUSE_TAB_COUNT; i++) {
    if (i == pause_tab) {
      setPauseTextColor(255, 236, 170);
    } else {
      setPauseTextColor(139, 147, 143);
    }
    drawString(pause_tab_names[i], tabCellX(i) + PAUSE_TAB_PAD,
      PAUSE_STRIP_TOP + 6);
  }
  setPauseTextColor(255, 255, 255);
}

/* ---------------------------------------------------------------- rows -- */

#define ROW_HEADER 0
#define ROW_VALUE 1

#define SHOW_TEXT 0
#define SHOW_PIPS 1
#define SHOW_SLIDER 2
#define SHOW_ACTION 3

typedef struct {
  u8 kind;
  u8 show;
  u8 steps;
  const char *label;
  /* One line under the page for whatever the cursor is on, which is the only
     documentation any of these settings has.  Upper case and no punctuation:
     the font atlas holds capitals and digits, and anything else comes out as
     a wrong glyph. */
  const char *blurb;
} PauseRow;

static u32 rowY(u32 top, u8 index) {
  return top + index * ROW_PITCH;
}

/* Fills phase: the highlight, and any value that is drawn rather than
   spelled.  A gold edge around a dark middle, for the reason the tab cells
   are drawn that way. */
static void drawRowHighlight(u32 y) {
  setPauseFillColor(216, 191, 77);
  gDPFillRectangle(dlp++, PAGE_LEFT, y, PAGE_RIGHT, y + ROW_HEIGHT);
  setPauseFillColor(9, 11, 13);
  gDPFillRectangle(dlp++, PAGE_LEFT + 1, y + 1, PAGE_RIGHT - 1,
    y + ROW_HEIGHT - 1);
}

static void drawPips(u32 y, u8 value, u8 steps) {
  u8 i;

  for (i = 0; i < steps; i++) {
    u32 x = ROW_VALUE_X + i * (PIP_SIZE + PIP_GAP);

    setPauseFillColor(66, 72, 71);
    gDPFillRectangle(dlp++, x, y + 2, x + PIP_SIZE, y + 2 + PIP_SIZE);
    setPauseFillColor(i < value ? 230 : 15, i < value ? 185 : 18,
      i < value ? 49 : 20);
    gDPFillRectangle(dlp++, x + 1, y + 3, x + PIP_SIZE - 1,
      y + 1 + PIP_SIZE);
  }
}

static void drawSlider(u32 y, u8 value, u8 steps) {
  u32 travel = SLIDER_WIDTH - SLIDER_KNOB;
  u32 knob = ROW_VALUE_X + (steps > 1 ? value * travel / (steps - 1) : 0);

  setPauseFillColor(52, 58, 57);
  gDPFillRectangle(dlp++, ROW_VALUE_X, y + 5, ROW_VALUE_X + SLIDER_WIDTH,
    y + 7);
  setPauseFillColor(26, 31, 33);
  gDPFillRectangle(dlp++, knob, y + 1, knob + SLIDER_KNOB, y + ROW_HEIGHT - 1);
  setPauseFillColor(230, 185, 49);
  gDPFillRectangle(dlp++, knob + 1, y + 2, knob + SLIDER_KNOB - 1,
    y + ROW_HEIGHT - 2);
}

/* ------------------------------------------------------------- options -- */

#define OPTION_SOUND_HEADER 0
#define OPTION_MUSIC 1
#define OPTION_EFFECTS 2
#define OPTION_PICTURE_HEADER 3
#define OPTION_FOG 4
#define OPTION_FOG_EDGE 5
#define OPTION_DETAIL 6
#define OPTION_OVERLAY 7
#define OPTION_CONTROL_HEADER 8
#define OPTION_STEERING 9
#define OPTION_VIEW 10
#define OPTION_ROW_COUNT 11

#define OPTIONS_TOP 40

/*
 * Every one of these was already in the game, reachable only by holding Z and
 * guessing.  Nothing here is new behaviour; what is new is that a player can
 * find it.  The two volumes are the exception -- the levels were compiled in.
 */
static const PauseRow option_rows[OPTION_ROW_COUNT] = {
  {ROW_HEADER, SHOW_TEXT, 0, "SOUND", ""},
  {ROW_VALUE, SHOW_PIPS, AUDIO_VOLUME_STEPS, "MUSIC",
    "THE SONG THAT PLAYS WHILE YOU BUILD"},
  {ROW_VALUE, SHOW_PIPS, AUDIO_VOLUME_STEPS, "EFFECTS",
    "DIGGING PLACING AND PICKING UP"},
  {ROW_HEADER, SHOW_TEXT, 0, "PICTURE", ""},
  {ROW_VALUE, SHOW_TEXT, 2, "FOG",
    "HAZE THAT HIDES THE EDGE OF THE WORLD"},
  {ROW_VALUE, SHOW_SLIDER, 17, "FOG EDGE",
    "HOW CLOSE THE HAZE IS ALLOWED TO COME"},
  {ROW_VALUE, SHOW_TEXT, 3, "DETAIL",
    "HOW WIDE THE FULLY DETAILED GROUND IS"},
  {ROW_VALUE, SHOW_TEXT, 2, "OVERLAY",
    "STREAMING AND TIMING NUMBERS ON SCREEN"},
  {ROW_HEADER, SHOW_TEXT, 0, "CONTROL", ""},
  {ROW_VALUE, SHOW_TEXT, 2, "STEERING",
    "LEFT AND RIGHT TURN INSTEAD OF STEPPING"},
  {ROW_VALUE, SHOW_TEXT, 2, "VIEW",
    "WHERE THE CAMERA SITS WHILE YOU WALK"}
};

/*
 * The stable ROM is built without audio (see the AUDIO=1 variant in
 * docs/building.md), and a volume row on it would be a control that visibly
 * moves and changes nothing.  Both rows stay on the page -- the setting
 * exists, this build simply cannot reach it -- but they are dimmed and the
 * cursor walks past them, the same way the setup card handles a switch the
 * chosen world cannot offer.
 */
#ifdef ENABLE_AUDIO
#define OPTION_ROW_REACHABLE(row) TRUE
#else
#define OPTION_ROW_REACHABLE(row) \
  ((row) != OPTION_MUSIC && (row) != OPTION_EFFECTS)
#endif

static u8 option_row = OPTION_FOG;

/* The full-detail radius the DETAIL row picks between.  The Z chord's four
   presets were authored to A/B the frame rate on a CRT and move the view cap
   as well; a player is choosing one thing, so this moves one thing.  Keep
   demote = promote + 2, or the hysteresis gap that stops columns thrashing
   between the two levels of detail disappears. */
static const u8 detail_promote[3] = {1, 2, 3};

static u8 detailStep(void) {
  u8 i;

  for (i = 0; i < 3; i++) {
    if (mesh_lod_promote_radius == detail_promote[i]) {
      return i;
    }
  }
  return 0;
}

static u8 optionValue(u8 row) {
  Player *player = &players[inventory_player];

  switch (row) {
    case OPTION_MUSIC:
      return musicVolume();
    case OPTION_EFFECTS:
      return soundVolume();
    case OPTION_FOG:
      return fog_enabled ? 1 : 0;
    case OPTION_FOG_EDGE:
      /* Stored as a signed bias about zero; shown as a slider that runs from
         one end to the other, so the middle notch is the shipped default. */
      return (u8) (fog_auto_bias + 8);
    case OPTION_DETAIL:
      return detailStep();
    case OPTION_OVERLAY:
      return diagnostics_visible ? 1 : 0;
    case OPTION_STEERING:
      return player->stick_turns ? 1 : 0;
    case OPTION_VIEW:
      return player->camera_mode == CAMERA_THIRD_PERSON ? 1 : 0;
    default:
      return 0;
  }
}

static void setOptionValue(u8 row, u8 value) {
  Player *player = &players[inventory_player];

  switch (row) {
    case OPTION_MUSIC:
      setMusicVolume(value);
      break;
    case OPTION_EFFECTS:
      setSoundVolume(value);
      break;
    case OPTION_FOG:
      fog_enabled = value != 0;
      break;
    case OPTION_FOG_EDGE:
      fog_auto_bias = (s8) ((s16) value - 8);
      break;
    case OPTION_DETAIL:
      mesh_lod_promote_radius = detail_promote[value];
      mesh_lod_demote_radius = (u8) (detail_promote[value] + 2);
      break;
    case OPTION_OVERLAY:
      diagnostics_visible = value != 0;
      break;
    case OPTION_STEERING:
      player->stick_turns = value != 0;
      break;
    case OPTION_VIEW:
      player->camera_mode = value != 0 ? CAMERA_THIRD_PERSON :
        CAMERA_FIRST_PERSON;
      break;
    default:
      break;
  }
}

static const char *optionValueText(u8 row) {
  u8 value = optionValue(row);

  switch (row) {
    case OPTION_DETAIL:
      return value == 0 ? "NEAR" : (value == 1 ? "MEDIUM" : "WIDE");
    case OPTION_VIEW:
      return value == 0 ? "FIRST" : "THIRD";
    default:
      return value != 0 ? "ON" : "OFF";
  }
}

static void drawOptionsPage(void) {
  u8 i;

  for (i = 0; i < OPTION_ROW_COUNT; i++) {
    u32 y = rowY(OPTIONS_TOP, i);

    if (i == option_row) {
      drawRowHighlight(y);
    }
    if (option_rows[i].kind == ROW_HEADER) {
      continue;
    }
    if (option_rows[i].show == SHOW_PIPS) {
      drawPips(y, optionValue(i), option_rows[i].steps);
    } else if (option_rows[i].show == SHOW_SLIDER) {
      drawSlider(y, optionValue(i), option_rows[i].steps);
    }
  }
}

static void drawOptionsPageText(void) {
  u8 i;

  for (i = 0; i < OPTION_ROW_COUNT; i++) {
    u32 y = rowY(OPTIONS_TOP, i) + ROW_TEXT_DROP;

    if (option_rows[i].kind == ROW_HEADER) {
      setPauseTextColor(181, 148, 96);
      drawString(option_rows[i].label, PAGE_LEFT + 4, y);
      continue;
    }
    if (!OPTION_ROW_REACHABLE(i)) {
      setPauseTextColor(104, 108, 98);
    } else {
      setPauseTextColor(i == option_row ? 255 : 222,
        i == option_row ? 236 : 226, i == option_row ? 170 : 214);
    }
    drawString(option_rows[i].label, ROW_LABEL_X, y);
    if (option_rows[i].show == SHOW_TEXT) {
      setPauseTextColor(241, 195, 58);
      drawString(optionValueText(i), ROW_VALUE_X, y);
    }
  }
  setPauseTextColor(200, 204, 190);
  drawString(option_rows[option_row].blurb, PAGE_LEFT + 4, BLURB_Y);
}

/* --------------------------------------------------------------- world -- */

#define WORLD_ROW_MONSTERS 0
#define WORLD_ROW_SURVIVAL 1
#define WORLD_ROW_SAVE 2
#define WORLD_ROW_COUNT 3

#define WORLD_FACTS_TOP 40
#define WORLD_RULES_TOP 96
#define WORLD_MADE_HEADER_Y 140
#define WORLD_MADE_TOP 152
#define WORLD_MADE_PITCH 11
#define WORLD_MADE_COLUMN_X 168

/*
 * The rules, and only the rules.  Everything above them on this page is the
 * world's identity and cannot be argued with -- see rules.h for the line
 * between the two, which is not "is it gameplay" but "would terrain built
 * before the change disagree with terrain built after it".
 */
static const PauseRow world_rows[WORLD_ROW_COUNT] = {
  {ROW_VALUE, SHOW_TEXT, RULE_MONSTERS_COUNT, "MONSTERS",
    "HOW FULL A NIGHT IS ALLOWED TO GET"},
  {ROW_VALUE, SHOW_TEXT, 2, "SURVIVAL",
    "HUNGER AND HARM  OFF IS FOR BUILDING"},
  {ROW_VALUE, SHOW_ACTION, 0, "SAVE WORLD",
    "WRITE THIS WORLD TO THE CARTRIDGE"}
};

static u8 world_row = WORLD_ROW_MONSTERS;

/* Everything the setup card offered except the terrain shape, which has its
   own line above.  All of it is shown dimmed: the four that regenerate cannot
   move at all, and the rest were spent when the world was made. */
static const u16 world_made_rows[] = {
  MOD_CAVES, MOD_RUINS, MOD_FORESTS, MOD_CRITTERS,
  MOD_PEACEFUL, MOD_BONUS_KIT, MOD_64MON
};
#define WORLD_MADE_COUNT ((u8) (sizeof world_made_rows / sizeof (u16)))

static u32 worldMadeX(u8 index) {
  return index < 4 ? ROW_LABEL_X : WORLD_MADE_COLUMN_X;
}

static u32 worldMadeY(u8 index) {
  return WORLD_MADE_TOP + (index < 4 ? index : index - 4) * WORLD_MADE_PITCH;
}

static const char *worldMadeName(u16 bit) {
  u8 i;

  for (i = 0; i < WORLD_MOD_COUNT; i++) {
    if (world_mod_table[i].bit == bit) {
      return world_mod_table[i].name;
    }
  }
  return "";
}

static void drawWorldPage(void) {
  CheckMarkPlacement marks[WORLD_MADE_COUNT];
  u8 i;

  /* The identity block has no cursor in it, so it gets a quiet slab rather
     than rows: nothing here responds to a button. */
  setPauseFillColor(15, 18, 20);
  gDPFillRectangle(dlp++, PAGE_LEFT, WORLD_FACTS_TOP - 4, PAGE_RIGHT,
    WORLD_FACTS_TOP + 50);
  setPauseFillColor(58, 64, 64);
  gDPFillRectangle(dlp++, PAGE_LEFT, WORLD_FACTS_TOP - 4, PAGE_LEFT + 1,
    WORLD_FACTS_TOP + 50);

  for (i = 0; i < WORLD_ROW_COUNT; i++) {
    if (i == world_row) {
      drawRowHighlight(rowY(WORLD_RULES_TOP, i));
    }
  }

  for (i = 0; i < WORLD_MADE_COUNT; i++) {
    marks[i].kind = CHECK_MARK_BOX;
    marks[i].on = worldModOn(world_made_rows[i]);
    marks[i].dim = TRUE;
    marks[i].x = (u16) worldMadeX(i);
    marks[i].y = (u16) worldMadeY(i);
  }
  drawCheckMarks(marks, WORLD_MADE_COUNT);
}

/*
 * The save row says what the cart is doing, because nothing else will.
 *
 * A write started here is sliced across callbacks exactly like the D-pad
 * save, and the driver steps it whatever screen is up -- but drawMenu only
 * prints its "SAVING" line on the gameplay screen, so from inside the pause
 * the press would otherwise look like it did nothing for two seconds.
 */
static const char *worldValueText(u8 row) {
  switch (row) {
    case WORLD_ROW_MONSTERS:
      return monsterRuleName(world_rules.monsters);
    case WORLD_ROW_SURVIVAL:
      return world_rules.survival ? "ON" : "OFF";
    default:
      if (!saving_available) {
        return storageStatusText();
      }
      return worldJobActive() ? "SAVING" : "";
  }
}

static void drawWorldPageText(void) {
  u32 ticks = dayCycleWorldTicks();
  u8 i;

  setPauseTextColor(155, 164, 160);
  drawString("NAME", ROW_LABEL_X, WORLD_FACTS_TOP);
  drawString("SEED", ROW_LABEL_X, WORLD_FACTS_TOP + 12);
  drawString("SHAPE", ROW_LABEL_X, WORLD_FACTS_TOP + 24);
  drawString("TIME", ROW_LABEL_X, WORLD_FACTS_TOP + 36);

  setPauseTextColor(235, 237, 227);
  if (game_file_num >= 1 && game_file_num <= 3) {
    drawString(world_names[game_file_num - 1], ROW_LABEL_X + 60,
      WORLD_FACTS_TOP);
  }
  /* Hex, as the setup card shows it, so the number a player wrote down off
     one screen is the number they read back off the other. */
  for (i = 0; i < 8; i++) {
    drawChar("0123456789ABCDEF"[(world_seed >> ((7 - i) * 4)) & 15],
      ROW_LABEL_X + 60 + i * 7, WORLD_FACTS_TOP + 12);
  }
  drawString(worldTerrainName(), ROW_LABEL_X + 60, WORLD_FACTS_TOP + 24);
  /* Which half of which day, in that order: "DAY 3" reads as a time, where a
     3 followed by the word DAY reads as a count of them. */
  drawString(dayCycleIsNight() ? "NIGHT" : "DAY", ROW_LABEL_X + 60,
    WORLD_FACTS_TOP + 36);
  drawPauseUnsigned(ticks / DAY_CYCLE_TICKS + 1, ROW_LABEL_X + 104,
    WORLD_FACTS_TOP + 36);

  for (i = 0; i < WORLD_ROW_COUNT; i++) {
    u32 y = rowY(WORLD_RULES_TOP, i) + ROW_TEXT_DROP;
    u8 selected = i == world_row;
    u8 usable = i != WORLD_ROW_SAVE || saving_available;

    if (!usable) {
      setPauseTextColor(104, 108, 98);
    } else {
      setPauseTextColor(selected ? 255 : 222, selected ? 236 : 226,
        selected ? 170 : 214);
    }
    drawString(world_rows[i].label, ROW_LABEL_X, y);
    setPauseTextColor(usable ? 241 : 104, usable ? 195 : 108,
      usable ? 58 : 98);
    drawString(worldValueText(i), ROW_VALUE_X, y);
    if (i == WORLD_ROW_SAVE && usable && worldJobActive()) {
      drawPauseUnsigned(worldJobProgress(),
        ROW_VALUE_X + pauseStringWidth("SAVING") + 7, y);
    }
  }

  setPauseTextColor(181, 148, 96);
  drawString("MADE WITH", PAGE_LEFT + 4, WORLD_MADE_HEADER_Y);
  setPauseTextColor(126, 135, 132);
  for (i = 0; i < WORLD_MADE_COUNT; i++) {
    drawString(worldMadeName(world_made_rows[i]),
      worldMadeX(i) + checkMarkSize() + 5, worldMadeY(i) + 1);
  }

  setPauseTextColor(200, 204, 190);
  drawString(world_rows[world_row].blurb, PAGE_LEFT + 4, BLURB_Y);
}

/* ------------------------------------------------------------ controls -- */

/*
 * How to play, as two columns of controls rather than sentences.
 *
 * This card used to live on the title screen and explained the unknown in
 * terms of itself -- "use / place / eat: A button", on the one screen a
 * player reads precisely because they do not yet know which button is which.
 * It comes back here instead, where a player who is already stuck can reach
 * it without leaving the world, and where the shoulder buttons that got them
 * to it are drawn along the top.
 */
static const LegendEntry controls_move_legend[] = {
  { BUTTON_ICON_STICK, BUTTON_ICON_NONE, "WALK / TURN" },
  { BUTTON_ICON_Z, BUTTON_ICON_STICK, "LOOK AROUND" },
  { BUTTON_ICON_Z, BUTTON_ICON_C_UP, "STEP SIDEWAYS" },
  { BUTTON_ICON_L, BUTTON_ICON_NONE, "SPRINT" },
  { BUTTON_ICON_R, BUTTON_ICON_NONE, "JUMP" },
  { BUTTON_ICON_C_UP, BUTTON_ICON_NONE, "CAMERA" }
};

static const LegendEntry controls_act_legend[] = {
  { BUTTON_ICON_A, BUTTON_ICON_NONE, "USE / PLACE / EAT" },
  /* No brackets: the font atlas is capitals, digits and a hyphen, and a
     paren comes out as a hole in the middle of the word. */
  { BUTTON_ICON_B, BUTTON_ICON_NONE, "MINE - HOLD" },
  { BUTTON_ICON_C_LEFT, BUTTON_ICON_C_RIGHT, "ITEMS" },
  { BUTTON_ICON_START, BUTTON_ICON_C_DOWN, "PACK" },
  { BUTTON_ICON_DPAD, BUTTON_ICON_NONE, "SAVE GAME" },
  { BUTTON_ICON_START, BUTTON_ICON_NONE, "CO-OP P2-P4" }
};

#define CONTROLS_TOP 48
#define CONTROLS_ROW_PITCH 18
#define CONTROLS_COLUMN_GAP 22
#define CONTROLS_NOTE "SLEEP IN A BED TO PASS A SAFE NIGHT"
#define CONTROLS_NOTE_Y 168

static u32 controlsMoveX(void) {
  return (SCREEN_WD -
    (legendColumnWidth(controls_move_legend,
       LEGEND_COUNT(controls_move_legend)) + CONTROLS_COLUMN_GAP +
     legendColumnWidth(controls_act_legend,
       LEGEND_COUNT(controls_act_legend)))) / 2;
}

static u32 controlsActX(void) {
  return controlsMoveX() +
    legendColumnWidth(controls_move_legend,
      LEGEND_COUNT(controls_move_legend)) + CONTROLS_COLUMN_GAP;
}

static void drawControlsPage(void) {
  drawLegendColumnIcons(controls_move_legend,
    LEGEND_COUNT(controls_move_legend), controlsMoveX(), CONTROLS_TOP,
    CONTROLS_ROW_PITCH);
  drawLegendColumnIcons(controls_act_legend,
    LEGEND_COUNT(controls_act_legend), controlsActX(), CONTROLS_TOP,
    CONTROLS_ROW_PITCH);
}

static void drawControlsPageText(void) {
  setPauseTextColor(226, 231, 219);
  drawLegendColumnLabels(controls_move_legend,
    LEGEND_COUNT(controls_move_legend), controlsMoveX(), CONTROLS_TOP,
    CONTROLS_ROW_PITCH);
  drawLegendColumnLabels(controls_act_legend,
    LEGEND_COUNT(controls_act_legend), controlsActX(), CONTROLS_TOP,
    CONTROLS_ROW_PITCH);
  setPauseTextColor(200, 204, 190);
  drawString(CONTROLS_NOTE,
    (SCREEN_WD - pauseStringWidth(CONTROLS_NOTE)) / 2, CONTROLS_NOTE_Y);
}

/* -------------------------------------------------------------- footer -- */

static const LegendEntry pause_change_legend[] = {
  { BUTTON_ICON_STICK, BUTTON_ICON_NONE, "CHOOSE" },
  { BUTTON_ICON_A, BUTTON_ICON_NONE, "CHANGE" },
  { BUTTON_ICON_START, BUTTON_ICON_B, "CLOSE" }
};

static const LegendEntry pause_close_legend[] = {
  { BUTTON_ICON_L, BUTTON_ICON_R, "PAGE" },
  { BUTTON_ICON_START, BUTTON_ICON_B, "CLOSE" }
};

static const LegendEntry *pauseFooter(u8 *count) {
  if (pause_tab == PAUSE_TAB_CONTROLS) {
    *count = LEGEND_COUNT(pause_close_legend);
    return pause_close_legend;
  }
  *count = LEGEND_COUNT(pause_change_legend);
  return pause_change_legend;
}

static u32 pauseFooterX(const LegendEntry *entries, u8 count) {
  return (SCREEN_WD - legendWidth(entries, count)) / 2;
}

/* ---------------------------------------------------------------- draw -- */

void drawPauseBody(void) {
  const LegendEntry *footer;
  u8 count;

  gDPPipeSync(dlp++);
  gDPSetCycleType(dlp++, G_CYC_FILL);
  gDPSetRenderMode(dlp++, G_RM_NOOP, G_RM_NOOP2);
  if (pause_tab == PAUSE_TAB_OPTIONS) {
    drawOptionsPage();
  } else if (pause_tab == PAUSE_TAB_WORLD) {
    drawWorldPage();
  } else {
    drawControlsPage();
  }
  footer = pauseFooter(&count);
  drawLegendIcons(footer, count, pauseFooterX(footer, count),
    PAUSE_LEGEND_Y);
  gDPPipeSync(dlp++);
}

void drawPauseBodyText(void) {
  const LegendEntry *footer;
  u8 count;

  if (pause_tab == PAUSE_TAB_OPTIONS) {
    drawOptionsPageText();
  } else if (pause_tab == PAUSE_TAB_WORLD) {
    drawWorldPageText();
  } else {
    drawControlsPageText();
  }
  footer = pauseFooter(&count);
  setPauseTextColor(150, 155, 142);
  drawLegendLabels(footer, count, pauseFooterX(footer, count),
    PAUSE_LEGEND_Y);
  setPauseTextColor(255, 255, 255);
}

/* --------------------------------------------------------------- input -- */

void beginPause(void) {
  pause_tab = PAUSE_TAB_PACK;
  option_row = OPTION_ROW_REACHABLE(OPTION_MUSIC) ? OPTION_MUSIC : OPTION_FOG;
  world_row = WORLD_ROW_MONSTERS;
}

void pauseTabPrevious(void) {
  pause_tab = pause_tab == 0 ? PAUSE_TAB_COUNT - 1 : (u8) (pause_tab - 1);
}

void pauseTabNext(void) {
  pause_tab = (u8) ((pause_tab + 1) % PAUSE_TAB_COUNT);
}

/* Step past the headers, which are labels rather than choices. */
static void moveOptionRow(s8 direction) {
  s8 row = (s8) option_row;

  do {
    row = (s8) (row + direction);
    if (row < 0) {
      row = OPTION_ROW_COUNT - 1;
    } else if (row >= OPTION_ROW_COUNT) {
      row = 0;
    }
  } while (option_rows[row].kind == ROW_HEADER ||
    !OPTION_ROW_REACHABLE(row));
  option_row = (u8) row;
}

/*
 * Two ways to change a value, deliberately behaving differently.
 *
 * The stick points at an end of the scale and stops there, because these are
 * ordered: pushing right on a volume already at maximum has exactly one
 * sensible outcome, and dropping to silence is not it.  A wraps, because A on
 * a two-value row has to be able to come back, and cycling four steps is
 * quicker than walking back down the row.
 */
static s16 steppedValue(u8 value, u8 steps, s8 direction, u8 wrap) {
  s16 next = (s16) value + direction;

  if (next < 0) {
    return wrap ? (s16) (steps - 1) : 0;
  }
  if (next >= steps) {
    return wrap ? 0 : (s16) (steps - 1);
  }
  return next;
}

static void changeOption(s8 direction, u8 wrap) {
  const PauseRow *row = &option_rows[option_row];

  setOptionValue(option_row, (u8) steppedValue(optionValue(option_row),
    row->steps, direction, wrap));
}

static void optionsInput(NUContData *cont, u8 navigation) {
  if (navigation & NAV_UP) {
    moveOptionRow(-1);
    return;
  }
  if (navigation & NAV_DOWN) {
    moveOptionRow(1);
    return;
  }
  if (navigation & NAV_LEFT) {
    changeOption(-1, FALSE);
    return;
  }
  if (navigation & NAV_RIGHT) {
    changeOption(1, FALSE);
    return;
  }
  if (cont->trigger & A_BUTTON) {
    changeOption(1, TRUE);
  }
}

static void changeWorldRule(s8 direction, u8 wrap, u8 fire) {
  if (world_row == WORLD_ROW_MONSTERS) {
    /* Ordered like the volumes, and clamped like them: NONE and MANY are the
       ends of a scale, not neighbours on a ring. */
    world_rules.monsters = (u8) steppedValue(world_rules.monsters,
      RULE_MONSTERS_COUNT, direction, wrap);
    /* Monsters already out are not deleted; updateMobs spends the new budget
       next frame and sends whatever it cannot afford home the way dawn
       does. */
    return;
  }
  if (world_row == WORLD_ROW_SURVIVAL) {
    world_rules.survival = (u8) steppedValue(world_rules.survival ? 1 : 0, 2,
      direction, wrap) != 0;
    applySurvivalRule();
    return;
  }
  /* SAVE, which is a thing to do rather than a value to walk along -- so the
     stick pointing at it does nothing and only A fires it.  A write already
     in flight absorbs the press rather than opening a second file over the
     first, the same rule the D-pad save follows. */
  if (!fire || !saving_available || worldJobActive()) {
    return;
  }
  requestWorldSave();
}

static void worldInput(NUContData *cont, u8 navigation) {
  if (navigation & NAV_UP) {
    world_row = world_row == 0 ? WORLD_ROW_COUNT - 1 : (u8) (world_row - 1);
    return;
  }
  if (navigation & NAV_DOWN) {
    world_row = (u8) ((world_row + 1) % WORLD_ROW_COUNT);
    return;
  }
  if (navigation & NAV_LEFT) {
    changeWorldRule(-1, FALSE, FALSE);
    return;
  }
  if (navigation & NAV_RIGHT) {
    changeWorldRule(1, FALSE, FALSE);
    return;
  }
  if (cont->trigger & A_BUTTON) {
    changeWorldRule(1, TRUE, TRUE);
  }
}

u8 pauseTabInput(NUContData *cont, u8 navigation) {
  if (pause_tab == PAUSE_TAB_PACK) {
    return FALSE;
  }
  if (pause_tab == PAUSE_TAB_OPTIONS) {
    optionsInput(cont, navigation);
  } else if (pause_tab == PAUSE_TAB_WORLD) {
    worldInput(cont, navigation);
  }
  /* CONTROLS reads rather than does; the strip and B are its whole input. */
  return TRUE;
}
