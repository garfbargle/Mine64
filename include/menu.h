#ifndef MENU_H
#define MENU_H

#include <nusys.h>

enum Screen {
  MENU,
  INFO,
  GENERATING,
  LOADING,
  /* The world data is ready; keep it on screen briefly for the flyover. */
  LOADING_PREVIEW,
  /* Choosing what a new world is made of, with that world orbiting beside the
     card.  Everything on it is locked once the world is created. */
  WORLD_SETUP,
  WORLD_NAMING,
  GAME,
  INVENTORY
};

extern enum Screen current_screen;

/*
 * The screens that show the orbiting preview instead of a world being played.
 * The renderer and the camera each ask this in several places, and every one
 * of them used to be its own list of comparisons -- which is how a new card
 * gets drawn with a gameplay lens, no backdrop, or the inventory panel on top
 * of it.  A front-end screen is added here and nowhere else.
 */
static __inline__ __attribute__((unused)) u8 screenShowsPreview(
    enum Screen screen) {
  return screen == LOADING_PREVIEW || screen == MENU ||
    screen == WORLD_SETUP || screen == WORLD_NAMING;
}

/* Of those, the ones with a card over the world: they get a flat backdrop
   rather than a sky, and their mesh is whatever the last build published. */
static __inline__ __attribute__((unused)) u8 screenIsWorldPicker(
    enum Screen screen) {
  return screen == MENU || screen == WORLD_SETUP || screen == WORLD_NAMING;
}
extern u32 save_message_cooldown;
extern u8 save_failed_message;
extern u8 world_incomplete_message;
extern u8 player_joined_message;
extern u8 player_joined_number;
extern u8 stick_turns_message;
extern u8 stick_turns_enabled_message;

void drawMenu();
void beginText();
void drawChar(char chr, u32 x, u32 y);
void drawString(const char *text, u32 x, u32 y);
/* The same font at a whole-integer scale, for the rare line that has to carry
   a screen on its own -- the menu title, the word for having died.  A glyph is
   8x8 before scaling and charWidth(c) * scale wide after it. */
void drawLargeString(const char *text, u32 x, u32 y, u8 scale);
u32 charWidth(char chr);
void menuDown();
void menuUp();
void menuAct();
void menuBack();
u8 menuPreviewRequested();
u8 menuGameRequested();
void menuGameStarted();
void menuPreviewLoaded();
u8 menuSelectedWorld();

/*
 * Everything the world on screen was built from, and a token that changes
 * whenever any of it does.  The job driver compares the token it started with
 * against this one: a mismatch means the terrain being built is already the
 * wrong terrain, and the build is abandoned rather than run to completion for
 * a choice the player has moved on from.
 */
u8 menuPreviewToken();
u32 menuPendingSeed();

/* The setup card. */
void beginWorldSetup();
void worldSetupUp();
void worldSetupDown();
void worldSetupToggle();
void worldSetupReroll();
u8 worldSetupRow();
/* TRUE once the player has asked to continue but the world is not finished.
   The request is held rather than dropped, and spends itself the moment the
   preview lands. */
u8 menuActPending();

/*
 * Committing the named world.  The write stops the console for as long as the
 * cart takes, so it may not begin until a frame explaining the pause has been
 * drawn and drained -- menuCommitReady is that handshake, and the job driver
 * is what calls menuCommitWorld on the far side of it.
 */
u8 menuCommitReady();
/* Called by the job driver when a sliced save ends, either way. */
void menuSaveFinished(u8 ok);

void beginWorldNaming();
void worldNameKeyboardLeft();
void worldNameKeyboardRight();
void worldNameKeyboardUp();
void worldNameKeyboardDown();
void worldNameCursorLeft();
void worldNameCursorRight();
void worldNameInsertCharacter();
void worldNameErase();
void confirmWorldName();

#endif /* MENU_H */
