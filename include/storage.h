#ifndef STORAGE_H
#define STORAGE_H

#include <nusys.h>

#define WORLD_NAME_LENGTH 12

extern u8 saving_available;
extern u8 files_present[3];
extern u32 game_file_num;
extern char world_names[3][WORLD_NAME_LENGTH + 1];

void initStorage();
u8 saveGame();
/* Write a plain-text post-mortem to mine64/freeze.txt on the cartridge SD.
   Called by the freeze watchdog after the console is already dead, so the
   usual single-threaded storage assumptions are moot; a failed write costs
   nothing that was not already lost. */
u8 storageWriteFreezeReport(const char *text, u32 length);
void loadGame();
void setWorldName(u8 slot, const char *name);

#endif /* STORAGE_H */
