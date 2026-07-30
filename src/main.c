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
#include "day_cycle.h"

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
    /* A named world becomes a real slot immediately when the flashcart
       filesystem is available; RAM-only uploads still run, but cannot retain
       worlds after a reset. */
    if (saving_available && !saveGame()) {
      save_failed_message = 120;
    }

    beginLoadingPreview();
    current_screen = LOADING_PREVIEW;
  }

  if (current_screen == LOADING) {
    loadGame();
    initDroppedItems();
    initGeometry();
    makeWorldDisplayLists();
    
    beginLoadingPreview();
    current_screen = LOADING_PREVIEW;
  }

  if (current_screen == LOADING_PREVIEW && loadingPreviewFinished()) {
    current_screen = GAME;
  }

  /* The title menu is also a world picker.  Preparing the highlighted slot
     here keeps menu input responsive and gives the renderer a real world to
     orbit rather than a generic background image. */
  if (current_screen == MENU && menuPreviewRequested()) {
    game_file_num = menuSelectedWorld() + 1;
    if (files_present[menuSelectedWorld()]) {
      loadGame();
    } else {
      initWorld();
      initPlayers();
    }
    initDroppedItems();
    initGeometry();
    makeWorldDisplayLists();
    beginLoadingPreview();
    menuPreviewLoaded();
  }

  if (current_screen == GAME || current_screen == INVENTORY) {
    updateDayCycle();
  } else {
    pauseDayCycle();
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
  initDayCycle();
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
