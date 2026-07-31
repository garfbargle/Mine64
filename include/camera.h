#ifndef CAMERA_H
#define CAMERA_H

#include <nusys.h>
#include "geometry.h"
#include "player.h"

extern u8 visible_columns[MAX_PLAYERS][CHUNKS_X * CHUNKS_Z];
extern u8 third_person_avatar_visible[MAX_PLAYERS];

void initCamera();
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
