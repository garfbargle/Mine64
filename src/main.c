#include <nusys.h>
#include "main.h"
#include "menu.h"
#include "camera.h"
#include "audio.h"
#include "player.h"
#include "geometry.h"
#include "graphics.h"
#include "items.h"
#include "mobs.h"
#include "storage.h"
#include "day_cycle.h"

/*
 * Replacing the world rewrites every column display list in the mesh arena
 * the RSP renders terrain from.  That may only happen when NuSystem reports
 * no graphics task in flight, and it must finish before the next task is
 * submitted -- otherwise the RSP walks commands that are being overwritten
 * underneath it.
 *
 * The build also runs far longer than one retrace, and nothing else on this
 * thread submits frames, so the picture freezes for its duration.  Announce
 * it for one frame first: that frame is on screen while the build runs.
 */
#define WORLD_BUILD_IDLE 0
#define WORLD_BUILD_ANNOUNCE 1
#define WORLD_BUILD_RUN 2

static u8 world_build_stage = WORLD_BUILD_IDLE;

static u8 worldBuildRequested() {
  return current_screen == GENERATING || current_screen == LOADING ||
    (current_screen == MENU && menuPreviewRequested());
}

static void runWorldBuild() {
  if (current_screen == GENERATING) {
    initWorld();
    initPlayers();
    initDroppedItems();
    initMobs();
    initGeometry();
    world_incomplete_message = !makeWorldDisplayLists();
    /* A named world becomes a real slot immediately when the flashcart
       filesystem is available; RAM-only uploads still run, but cannot retain
       worlds after a reset. */
    if (saving_available && !saveGame()) {
      save_failed_message = 120;
    }

    beginLoadingPreview();
    current_screen = LOADING_PREVIEW;
  } else if (current_screen == LOADING) {
    loadGame();
    initDroppedItems();
    initMobs();
    initGeometry();
    world_incomplete_message = !makeWorldDisplayLists();

    beginLoadingPreview();
    current_screen = LOADING_PREVIEW;
  } else if (current_screen == MENU && menuPreviewRequested()) {
    /* The title menu is also a world picker.  Preparing the highlighted slot
       here gives the renderer a real world to orbit rather than a generic
       background image. */
    game_file_num = menuSelectedWorld() + 1;
    if (files_present[menuSelectedWorld()]) {
      loadGame();
    } else {
      initWorld();
      initPlayers();
    }
    initDroppedItems();
    initMobs();
    initGeometry();
    world_incomplete_message = !makeWorldDisplayLists();
    beginLoadingPreview();
    menuPreviewLoaded();
  }
}

void callbackGfx(int pendingGfx) {
  /*
   * One cinematic task can outlive a video retrace.  Queuing a second in that
   * case lets NuSystem rotate framebuffers while the RDP is still writing the
   * older one, which presents as torn terrain followed by corrupt UI.  Submit
   * only after the previous task has completely drained, letting the display
   * pace itself to the actual RSP/RDP cost.
   */
  if (pendingGfx == 0) {
    if (world_build_stage == WORLD_BUILD_RUN) {
      world_build_stage = WORLD_BUILD_IDLE;
      runWorldBuild();
    }
    /* A mesh arena can only be recycled when no submitted task can still
       reference its display lists. */
    draw(TRUE);
    /* Promote only once a frame has actually been submitted: that frame is
       what remains on screen while the next callback's build runs. */
    if (world_build_stage == WORLD_BUILD_ANNOUNCE) {
      world_build_stage = WORLD_BUILD_RUN;
    }
  }

  if (current_screen == LOADING_PREVIEW && loadingPreviewFinished()) {
    current_screen = GAME;
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

  /* Arm after input has settled.  A request raised by this frame's input gets
     one drawn frame to put its card or its outgoing preview on screen before
     the build stalls the thread. */
  if (world_build_stage == WORLD_BUILD_IDLE && worldBuildRequested()) {
    world_build_stage = WORLD_BUILD_ANNOUNCE;
  }
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
