#ifndef PAUSE_H
#define PAUSE_H

#include <nusys.h>

/*
 * The pause screen, of which the pack is one page.
 *
 * START has always opened the inventory, and the inventory has always been a
 * value of `enum Screen`.  Tabs are a variable inside that screen rather than
 * four more screen values, because "paused" is one state as far as everything
 * outside this file is concerned: the music that keeps playing, the world
 * that stops simulating, the backdrop the renderer clears to, the save that
 * is still allowed to run.  Four screens would mean teaching every one of
 * those tests about all four, and the first one anybody forgot would be a tab
 * that silently switched to the title music.
 *
 * The stick-repeat navigation belongs to player.c, which is why the decoded
 * bitmask is passed in rather than reproduced here -- moving through a list
 * has to feel the same on every screen in the game.
 */
#define PAUSE_TAB_PACK 0
#define PAUSE_TAB_OPTIONS 1
#define PAUSE_TAB_WORLD 2
#define PAUSE_TAB_CONTROLS 3
#define PAUSE_TAB_COUNT 4

extern u8 pause_tab;

/* Back to the pack, with every cursor reset.  Called when START opens the
   pause, so it always comes up on the page the button has always opened. */
void beginPause(void);
void pauseTabPrevious(void);
void pauseTabNext(void);

/*
 * One frame of input for whichever tab is up.  The pack answers FALSE, which
 * is player.c's signal to run the inventory handling it already owns; every
 * other tab handles itself and answers TRUE.  L and R are consumed before
 * this is reached, by the caller, because they belong to the strip rather
 * than to any page on it.
 */
u8 pauseTabInput(NUContData *cont, u8 navigation);

/*
 * Both phases of both halves, kept apart for the reason every screen in this
 * game keeps them apart: fills and text want the RDP configured differently
 * and swapping between them mid-card is the hazard that locks the console.
 *
 * The frame -- panel, tab strip -- is shared by all four tabs.  The body is
 * the page itself, and the pack's lives in graphics.c beside the item icons
 * it needs.
 */
void drawPauseFrame(void);
void drawPauseFrameText(void);
void drawPauseBody(void);
void drawPauseBodyText(void);

#endif /* PAUSE_H */
