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
void makeDisplayListsAt(int x, int z);
/*
 * The render origin.  Block coordinates stay absolute everywhere; the origin
 * is subtracted only at the moment a world position becomes an Mtx, because
 * the N64 matrix format is s15.16 -- with BLOCK_SIZE 64 a translation loses
 * sub-block precision past about +-512 blocks of the origin.  Everything that
 * writes a world-space guTranslate must subtract these.
 */
extern int render_origin_x;   /* block coordinates, chunk aligned */
extern int render_origin_z;
extern float render_origin_units_x;  /* the same, premultiplied by BLOCK_SIZE */
extern float render_origin_units_z;
/* Move the origin and rewrite every resident column's chunk matrices to it.
   Only call with no graphics task in flight and before that callback's
   draw(): a frame must never mix origins between its camera and its terrain,
   and draw() rebuilds the camera and entity matrices itself each frame. */
void graphicsSetRenderOrigin(int block_x, int block_z);
/* Stop drawing a slot and drop any pending rebuild for it.  Called when the
   residency window rebinds the slot to a different column. */
void graphicsInvalidateColumnSlot(u32 slot);
/* Queue a resident column for mesh compilation. */
void graphicsMarkColumnDirty(int cx, int cz);
/* TRUE for a resident column with no compiled geometry behind it. */
u8 graphicsColumnNeedsMesh(int cx, int cz);
void drawWorld();
void drawWireframes();
void drawHUD();
void draw(int can_reclaim_mesh_arena);
/* Frames that exceeded the command budget and had to shed terrain or be
   dropped.  Non-zero means the frame list was overflowing. */
extern u32 frame_overflows;

#endif /* GRAPHICS_H */
