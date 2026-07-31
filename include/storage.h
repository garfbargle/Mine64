#ifndef STORAGE_H
#define STORAGE_H

#include <nusys.h>

#define WORLD_NAME_LENGTH 12

extern u8 saving_available;
extern u8 files_present[3];
extern u32 game_file_num;
extern char world_names[3][WORLD_NAME_LENGTH + 1];

/*
 * Why storage came up unavailable.  initStorage has four distinct ways to
 * fail and every one of them used to return silently behind a single blanket
 * "NO CART SAVE DEVICE", which cannot tell a cart that was never detected
 * from an SD card that is simply formatted exFAT.  On hardware -- the only
 * place any of this can be observed -- that distinction is the whole
 * diagnosis, so the failing step is recorded and shown.
 */
#define STORAGE_OK 0
#define STORAGE_NO_CART 1       /* cart_init found no supported flashcart */
#define STORAGE_CARD_NOT_READY 2 /* flashcart found, SD card did not init */
#define STORAGE_BAD_FILESYSTEM 3 /* SD readable but not FAT16/FAT32 */
#define STORAGE_MOUNT_FAILED 4  /* f_mount failed some other way */
#define STORAGE_NO_DIRECTORY 5  /* mounted, but mine64/ is unusable */

extern u8 storage_status;
/* Short, uppercase, and sized for the title screen's centred line. */
const char *storageStatusText(void);

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
