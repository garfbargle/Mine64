#ifndef GRAPHICS_H
#define GRAPHICS_H

#define SCREEN_HT 240
#define SCREEN_WD 320

/* Mine64 supplies its own RDP command FIFO instead of NuSystem's 128 KiB
   default; see src/rdp_fifo.c.  Must be a multiple of 16. */
#define MINE64_RDP_FIFO_SIZE (64 * 1024)
void mine64SetRDPFifo(void);

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
void beginWorldMeshBuild(void);
u8 stepWorldMeshBuild(u16 columns);
u8 worldMeshBuildProgress();
u8 worldMeshBuildComplete();
/* Drop a build whose world is no longer wanted, returning its staged blocks
   to the arena.  The live pointer set -- the world on screen -- is untouched,
   which is what makes this safe to call from the same gated slot the build
   itself steps in. */
void cancelWorldMeshBuild(void);
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
/* Pad calibration, riding the G row: current raw stick deflection and the peak
   since boot.  Rest the stick to read its centre error, then roll it round the
   gate to read how far it actually travels -- the two numbers the stick
   shaping in player.c has to assume. */
extern u32 diag_stick_magnitude;
extern u32 diag_stick_peak;
/* Simulation step in hundredths of a 60 Hz frame, riding the T row.  100 is
   retrace rate.  Distinct from FPS, which counts drawn frames: the player
   updates on every callback whether one is drawn or not, so this sits well
   above the frame rate and is the unit the stick constants are written in. */
extern u32 diag_sim_delta;
/* Overlay visibility: Z + D-pad Up toggles; any integrity anomaly (fault,
   hang, key corruption, position snap) switches it on automatically.  The
   watchdog and SD post-mortem are never gated on it. */
extern u8 diagnostics_visible;
/* Sky-matched distance fog that hugs the streaming frontier: parked past
   the mesh ring (where it costs nothing) while the view is fully meshed,
   slid inward by updateAutoFog to cover the void whenever the player
   outruns the mesher.  fog_start is the live screen-depth value the
   controller lands on (the P row shows it; see the distance table in
   graphics.c -- 985 is ten blocks out, 999 past the ring).  With the
   overlay up, Z + D-pad Left/Right walk fog_auto_bias, which offsets
   where the band rests relative to the hole, and Z + D-pad Down toggles
   fog outright. */
extern u8 fog_enabled;
extern u16 fog_start;
extern s8 fog_auto_bias;
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
/*
 * The world job (see main.c).  Without these a stall anywhere in loading kept
 * whatever colour the previous callback signed off with -- DONE, blue -- so
 * the frozen square said only "the last frame ended normally" about the one
 * part of the program that does seconds of work at a time.  These three
 * cannot be confused with the gameplay phases above in practice: they are
 * only ever painted from the title and loading screens, where streaming,
 * rebasing and the entity passes never run.
 */
#define DIAG_PHASE_LOAD GPACK_RGBA5551(255, 0, 128, 1)       /* rose */
#define DIAG_PHASE_GENERATE GPACK_RGBA5551(128, 255, 0, 1)   /* lime */
#define DIAG_PHASE_MESH GPACK_RGBA5551(128, 128, 255, 1)     /* periwinkle */
/* Committing a named world: the one place left that stops the console for
   longer than a frame, so the frozen square must be able to say it was the
   cart rather than blaming whatever ran before it. */
#define DIAG_PHASE_SAVE GPACK_RGBA5551(255, 200, 0, 1)       /* amber */
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
/* The player's face: 24 vertices and six quads authored for a 32-unit head
   box at the head part's own origin, drawn with whatever matrices that head
   is already bound to.  Shared with 64MON's trainers, who are other players
   and so wear the same face rather than a copy of it. */
extern Vtx steve_face_verts[];
extern Gfx steve_face_display_list[];
void drawWorld();
void drawWireframes();
void drawHUD();

/*
 * Controller buttons as the controller wears them, for naming a control
 * without spelling it out.  A button is a fill-mode sprite, so the RDP must
 * already be in G_CYC_FILL and the call must sit in a screen's fills phase --
 * putting one between two runs of text swaps the RDP back and forth mid-card,
 * which is the hazard that locks the console (see menu_setup_display_list).
 */
typedef enum {
  BUTTON_ICON_A,
  BUTTON_ICON_B,
  BUTTON_ICON_START,
  BUTTON_ICON_C_UP,
  BUTTON_ICON_C_DOWN,
  BUTTON_ICON_C_LEFT,
  BUTTON_ICON_C_RIGHT,
  BUTTON_ICON_L,
  BUTTON_ICON_R,
  BUTTON_ICON_Z,
  BUTTON_ICON_COUNT
} ButtonIconId;

void drawButtonIcon(ButtonIconId id, u32 x, u32 y);
/* The round buttons are 13x13 and the shoulders 19x11; ask rather than
   assume, so a label can be laid out beside either. */
u32 buttonIconWidth(ButtonIconId id);
u32 buttonIconHeight(ButtonIconId id);
void draw(int can_reclaim_mesh_arena);
/* Frames that exceeded the command budget and had to shed terrain or be
   dropped.  Non-zero means the frame list was overflowing. */
extern u32 frame_overflows;

#endif /* GRAPHICS_H */
