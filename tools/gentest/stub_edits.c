/*
 * The generation harness compares the blocks a world is *generated* from, so
 * the player-edit overlay is stubbed out there.  It lives in its own file
 * because the edit harness links the real src/edits.c against the same set of
 * stubs and must not collide with this one.
 */
#include <nusys.h>

void worldApplyEditsToColumn(int cx, int cz) {
  (void) cx; (void) cz;
}
