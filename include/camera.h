#ifndef CAMERA_H
#define CAMERA_H

#include <nusys.h>
#include "geometry.h"
#include "player.h"

extern u8 visible_columns[MAX_PLAYERS][CHUNKS_X * CHUNKS_Z];

void initCamera();
void updateVisibleColumns(u8 player_num);
void updateCameraMatrices(u8 player_num);
void loadCameraMatrices(u8 player_num);

#endif /* CAMERA_H */
