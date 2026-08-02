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
/*
 * The same visibility as a compact slot list, gathered by the culling scan.
 * At most ~120 of the window's 1,024 slots are ever visible, and the terrain
 * pass walks the set once per texture per viewer -- 16,384 slot tests a
 * frame, doubled when fog splits the pass, against ~120 real entries.  The
 * list is what drawTextured iterates; the per-slot array above stays, both
 * as the class lookup and for the point-visibility readers that index it
 * directly.  Sized for every cell the cull span can visit.
 */
#define VISIBLE_SLOT_LIST_MAX \
  ((2 * STREAM_TREE_RADIUS + 1) * (2 * STREAM_TREE_RADIUS + 1))
extern u16 visible_slot_list[MAX_PLAYERS][VISIBLE_SLOT_LIST_MAX];
extern u16 visible_slot_count[MAX_PLAYERS];
extern u8 third_person_avatar_visible[MAX_PLAYERS];
/* Solo view's visible-column cap; runtime for the Z + C-left preset chord. */
extern u16 solo_max_visible_columns;
/* Visible columns that reach the fog band, per viewer; zero lets the
   terrain pass skip the two-cycle fogged half entirely. */
extern u16 visible_far_count[MAX_PLAYERS];
/* Squared block distance to the nearest frustum cell with no geometry
   behind it, or VISIBLE_HOLE_NONE when the view is fully meshed.  The
   auto fog slides its band in to end where this says the void begins. */
#define VISIBLE_HOLE_NONE 1.0e9f
extern float visible_hole_sq[MAX_PLAYERS];

void initCamera();
/* Advances a small first-person resting sway.  Kept in camera state so it
   remains visual-only and never changes a saved player record. */
void updateCameraIdleSway(u8 player_num, float delta, u8 standing_still);
Vector3 playerCameraPosition(u8 player_num);
void updateVisibleColumns(u8 player_num);
void updateCameraMatrices(u8 player_num);
void loadCameraProjection();
void loadCameraMatrices(u8 player_num);

/*
 * The front end's camera: a slow carousel around the world standing behind
 * the cards.  resetPreviewOrbit puts it back at its starting angle, which the
 * job driver does whenever a build publishes a new world, so each one is
 * revealed from the same bearing as the last.
 */
void resetPreviewOrbit();
void updateLoadingCamera();

#endif /* CAMERA_H */
