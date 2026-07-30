#include <nusys.h>
#include "main.h"
#include "menu.h"
#include "camera.h"
#include "audio.h"
#include "player.h"
#include "geometry.h"
#include "graphics.h"
#include "items.h"
#include "storage.h"

void draw(void);

void callbackGfx(int pendingGfx) {
  if(pendingGfx < 2) {
    draw();
  }

  if (current_screen == GENERATING) {
    initWorld();
    initPlayers();
    initDroppedItems();
    initGeometry();
    makeWorldDisplayLists();

    current_screen = GAME;
  }

  if (current_screen == LOADING) {
    loadGame();
    initDroppedItems();
    initGeometry();
    makeWorldDisplayLists();
    
    current_screen = GAME;
  }

  updatePlayers();
#ifdef ENABLE_AUDIO
  updateAudio(current_screen);
#endif
}

void callbackPreNMI() {
    nuGfxDisplayOff();
    osViSetYScale(1);
    osAfterPreNMI();
}

void initVideo() {
  osCreateViManager(OS_PRIORITY_VIMGR);

  if (osTvType == OS_TV_NTSC) {
    osViSetMode(&osViModeNtscLan1);
  } else if (osTvType == OS_TV_PAL) {
    osViSetMode(&osViModeFpalLan1);
    osViSetYScale(0.833);
  } else if (osTvType == OS_TV_MPAL) {
    osViSetMode(&osViModeMpalLan1);
  }

  osViSetSpecialFeatures(OS_VI_GAMMA_OFF);
  nuPreNMIFuncSet((NUScPreNMIFunc) callbackPreNMI);
}

void mainproc(void) {
  initVideo();
  initCamera();
  initGraphics();
  initStorage();
  nuContInit();
#ifdef ENABLE_AUDIO
  /*
   * Storage owns the flashcart/PI during startup. Start audio only after cart
   * probing and filesystem recovery have completely finished.
   */
  initAudio();
#endif
  nuGfxFuncSet((NUGfxFunc) callbackGfx);

  while(1)
    ;
}
