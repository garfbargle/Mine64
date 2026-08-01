#ifndef GRAPHICS_H
#define GRAPHICS_H

#define SCREEN_HT 240
#define SCREEN_WD 320

#define NUM_DISPLAY_LISTS 2
/* World, targeting, and HUD share one RSP task.  Sized with deliberate
 * headroom over the three independent command buffers this replaced. */
#define FRAME_DISPLAY_LIST_SIZE 6656

#define BLOCK_SIZE 64

extern Gfx* dlp;
extern Gfx frame_display_lists[NUM_DISPLAY_LISTS][FRAME_DISPLAY_LIST_SIZE];
extern u32 dl_no;

void initGraphics();

/* Sliced mesh compilation.  The build targets whichever arena is not on
   screen and publishes it only once complete, so the outgoing world keeps
   rendering for the whole rebuild. */
void beginWorldMeshBuild(u8 surface_only);
u8 stepWorldMeshBuild(u16 columns);
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
/* Freeze-bisection instrumentation; temporary, until the far-walk freeze is
   found.  The diagnostics HUD shows how many times the origin has moved, and
   the phase square (see graphics.c) reports where the console died. */
extern u8 stream_rebase_enabled;
extern u32 render_rebase_count;
/* CPU-painted phase square, alive after the graphics pipeline dies.  The
   phase is recorded in diag_current_phase; a high-priority watchdog thread
   in main.c repaints it into both framebuffers once diag_heartbeat stops
   advancing, so the fatal phase survives task completion, buffer swaps, and
   the death of the graphics thread itself. */
void diagPaintPhase(u16 color);
void diagPaintStalePhase(void);
void diagWatchdogTick(int pendingGfx);
extern volatile u16 diag_current_phase;
extern volatile u32 diag_heartbeat;
/* Run-4 forensics: which player sub-step ran last (middle band of the frozen
   square), whether the CPU took a fault (bottom band, white = crashed rather
   than looping), and how often a runaway-loop guard fired (the L row). */
extern volatile u8 diag_player_step;
extern volatile u8 diag_cpu_faulted;
extern u32 diag_loop_clamps;
/* Split the L total by the two collision loops.  The HUD temporarily shows
   the ray guard's speed (S) and boundary time (N) in place of the
   low-priority mesh/key rows once it fires. */
extern u32 diag_ray_clamps;
extern u32 diag_resolve_clamps;
extern u32 diag_ray_guard_speed;
extern u32 diag_ray_guard_time;
extern u32 diag_position_glitches;
/* Overlay visibility: Z + D-pad Up toggles; any integrity anomaly (fault,
   hang, key corruption, position snap) switches it on automatically.  The
   watchdog and SD post-mortem are never gated on it. */
extern u8 diagnostics_visible;
/* Sky-matched distance fog over the gameplay terrain pass.  The start is a
   screen-depth value tuned on hardware: with the overlay up, Z + D-pad
   Left/Right moves it (P row shows it) and Z + D-pad Down toggles fog.
   The range covers the distances that exist: see the table in graphics.c,
   985 is ten blocks out and 999 is past the mesh ring, while the old floor
   of 900 was a block and a half in front of the player's face. */
extern u8 fog_enabled;
extern u16 fog_start;
#define FOG_START_MIN 985
#define FOG_START_MAX 999
/* Full-detail LOD radii, runtime for the Z + C-left preset chord: the near
   disc's RSP transform is the measured standing-still frame cost.  Keep
   demote = promote + 2 so the hysteresis gap survives every preset. */
extern u8 mesh_lod_promote_radius;
extern u8 mesh_lod_demote_radius;
#define DIAG_STEP_OBJECTIVES 1
#define DIAG_STEP_INPUT 2
#define DIAG_STEP_VAULT 3
#define DIAG_STEP_COLLIDE 4
#define DIAG_STEP_TARGET 5
#define DIAG_STEP_ACTIONS 6
#define DIAG_STEP_POST 7
/* Frame pacing evidence for the "quicksand" symptom: worst wall-clock gap
   between graphics callbacks and worst CPU time spent inside the gated
   streaming/draw block, both over a rolling ~2s window, shown as W and B. */
void diagNoteFrameInterval(u32 usec);
void diagNoteGatedWork(u32 usec);
/* Called once per submitted graphics task, from draw().  Frames displayed in
   the last whole second show beside the W row.  Counting submissions rather
   than callbacks is what makes it the rate the player sees: a callback that
   finds a task still in flight returns without building a frame. */
void diagNoteFrameSubmitted(void);
#define DIAG_PHASE_STREAMING GPACK_RGBA5551(0, 255, 0, 1)    /* green */
#define DIAG_PHASE_REBASE GPACK_RGBA5551(255, 255, 0, 1)     /* yellow */
#define DIAG_PHASE_DRAW GPACK_RGBA5551(0, 255, 255, 1)       /* cyan */
#define DIAG_PHASE_PLAYERS GPACK_RGBA5551(255, 0, 255, 1)    /* magenta */
#define DIAG_PHASE_TREES GPACK_RGBA5551(255, 140, 0, 1)      /* orange */
#define DIAG_PHASE_ITEMS GPACK_RGBA5551(255, 255, 255, 1)    /* white */
#define DIAG_PHASE_MOBS GPACK_RGBA5551(0, 0, 0, 1)           /* black */
#define DIAG_PHASE_DONE GPACK_RGBA5551(0, 0, 255, 1)         /* blue */
/* Stop drawing a slot and drop any pending rebuild for it.  Called when the
   residency window rebinds the slot to a different column. */
void graphicsInvalidateColumnSlot(u32 slot);
/* Queue a resident column for mesh compilation. */
void graphicsMarkColumnDirty(int cx, int cz);
/* TRUE for a resident column whose mesh is missing or LOD-stale. */
u8 graphicsColumnNeedsMesh(int cx, int cz);
/* TRUE only when the mesh is missing outright -- the urgency signal. */
u8 graphicsColumnMissingMesh(int cx, int cz);
void drawWorld();
void drawWireframes();
void drawHUD();
void draw(int can_reclaim_mesh_arena);
/* Frames that exceeded the command budget and had to shed terrain or be
   dropped.  Non-zero means the frame list was overflowing. */
extern u32 frame_overflows;

#endif /* GRAPHICS_H */
