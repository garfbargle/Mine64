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

/*
 * Sliced load, for the same reason world generation is sliced: a save is
 * 200 KB of packed blocks pulled through a 512-byte window, which is seconds
 * of cart traffic and cannot happen inside one graphics callback without
 * stopping the picture, the controller, and the freeze watchdog's heartbeat
 * along with them.
 *
 * beginLoadGame parses the header page and claims the extent; stepLoadGame
 * streams a bounded number of x-slabs per call and verifies the file when it
 * runs out.  Both answer with one of the status codes below, so the caller
 * can keep drawing frames in between and knows when the save turned out to
 * be unusable.
 */
#define LOAD_BUSY 0     /* call stepLoadGame again next frame */
#define LOAD_DONE 1     /* the world is in memory and verified */
#define LOAD_GENERATE 2 /* nothing loadable here; generate a fresh world */

u8 beginLoadGame(void);
u8 stepLoadGame(u16 slabs);
/* Abandon a load in progress and close its file.  The blocks already written
   stay where they are; the next build claims the extent again before writing
   anything into it. */
void cancelLoadGame(void);
/* 0..100 across the payload; header work counts as zero and is one page. */
u8 loadGameProgress(void);
/* Blocking whole-file load, for callers that cannot yield.  Identical
   outcome to driving the sliced pair to completion, fresh world included. */
void loadGame();
void setWorldName(u8 slot, const char *name);

#endif /* STORAGE_H */
