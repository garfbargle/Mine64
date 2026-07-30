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
void makeWorldDisplayLists();
void makeDisplayListsAt(u8 x, u8 z);
void drawWorld();
void drawWireframes();
void drawHUD();
void draw(int can_reclaim_mesh_arena);

#endif /* GRAPHICS_H */
