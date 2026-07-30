#ifndef GRAPHICS_H
#define GRAPHICS_H

#define SCREEN_HT 240
#define SCREEN_WD 320

#define DISPLAY_LIST_SIZE 131072
#define NUM_DISPLAY_LISTS 2
#define WORLD_DISPLAY_LIST_SIZE 3072
#define LINE_DISPLAY_LIST_SIZE 512
#define HUD_DISPLAY_LIST_SIZE 2048
/* World, targeting, and HUD now share one RSP task.  Leave deliberate headroom
 * over the former three independent command buffers. */
#define FRAME_DISPLAY_LIST_SIZE 6656

#define BLOCK_SIZE 64

extern Gfx* dlp;
extern Gfx frame_display_lists[NUM_DISPLAY_LISTS][FRAME_DISPLAY_LIST_SIZE];
extern u32 dl_no;

void initGraphics();
/* Returns FALSE when the mesh arena filled before every column was compiled;
   the world then renders with missing columns. */
u8 makeWorldDisplayLists();
/* Full-detail gameplay mesh, compiled into the other arena when a world is
   entered.  makeWorldDisplayLists builds only the scenic surface shell. */
u8 makeGameWorldDisplayLists();

/* Sliced mesh compilation.  The build targets whichever arena is not on
   screen and publishes it only once complete, so the outgoing world keeps
   rendering for the whole rebuild. */
void beginWorldMeshBuild(u8 surface_only);
u8 stepWorldMeshBuild(u16 columns);
u8 worldMeshBuildActive();
u8 worldMeshBuildProgress();
u8 worldMeshBuildComplete();
void makeDisplayListsAt(u8 x, u8 z);
void drawWorld();
void drawWireframes();
void drawHUD();
void draw(int can_reclaim_mesh_arena);
/* Frames that exceeded the command budget and had to shed terrain or be
   dropped.  Non-zero means the frame list was overflowing. */
extern u32 frame_overflows;
extern u32 diag_frame_commands;
extern u32 diag_peak_frame_commands;
extern u32 diag_terrain_branches;
extern u32 diag_visible_columns;
extern u32 diag_arena_used;
extern u32 diag_mesh_failures;
extern u32 diag_frames;
extern u32 diag_arena_id;
extern u8 diag_draw_terrain;
extern u8 diag_refine_seams;
extern u8 diag_column_cap;
extern u32 diag_build_active;

#endif /* GRAPHICS_H */
