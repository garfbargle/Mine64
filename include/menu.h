#ifndef MENU_H
#define MENU_H

#include <nusys.h>

enum Screen {
  MENU,
  INFO,
  GENERATING,
  LOADING,
  GAME,
  INVENTORY
};

extern enum Screen current_screen;
extern u32 save_message_cooldown;
extern u8 player_two_joined_message;

void drawMenu();
void drawChar(char chr, u32 x, u32 y);
void menuDown();
void menuUp();
void menuAct();

#endif /* MENU_H */
