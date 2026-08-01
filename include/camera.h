#ifndef CAMERA_H
#define CAMERA_H

#include <nusys.h>
#include "geometry.h"
#include "player.h"

/*
 * Per-slot visibility, and which side of the fog onset a visible column sits
 * on.  Zero is invisible.  NEAR columns lie entirely closer than the distance
 * where fog begins (a function of fog_start and the projection's far plane),
 * so the terrain pass may draw them in single-cycle mode with fog off -- the
 * blend result is identical and the RDP runs at twice the pixel rate.  FAR
 * columns actually reach into the fog band and take the two-cycle pass.
 * Any non-zero value still reads as "visible" to boolean users.
 */
#define COLUMN_VISIBLE_NEAR 1
#define COLUMN_VISIBLE_FAR 2
extern u8 visible_columns[MAX_PLAYERS][WINDOW_SLOTS];
extern u8 third_person_avatar_visible[MAX_PLAYERS];

void initCamera();
/* Advances a small first-person resting sway.  Kept in camera state so it
   remains visual-only and never changes a saved player record. */
void updateCameraIdleSway(u8 player_num, float delta, u8 standing_still);
Vector3 playerCameraPosition(u8 player_num);
void updateVisibleColumns(u8 player_num);
void updateCameraMatrices(u8 player_num);
void loadCameraProjection();
void loadCameraMatrices(u8 player_num);

/* A title-card camera used while a freshly prepared world is coming online. */
void beginLoadingPreview();
void updateLoadingCamera();
u8 loadingPreviewFinished();
u8 loadingPreviewProgress();

#endif /* CAMERA_H */
