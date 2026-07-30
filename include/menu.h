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
  GAME,
  INVENTORY
};

extern enum Screen current_screen;
extern u32 save_message_cooldown;
extern u8 save_failed_message;
extern u8 player_joined_message;
extern u8 player_joined_number;

void drawMenu();
void beginText();
void drawChar(char chr, u32 x, u32 y);
void drawString(const char *text, u32 x, u32 y);
u32 charWidth(char chr);
void menuDown();
void menuUp();
void menuAct();
u8 menuPreviewRequested();
void menuPreviewLoaded();
u8 menuSelectedWorld();

#endif /* MENU_H */
