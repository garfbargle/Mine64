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
/* The face: 24 vertices and six quads authored for a 32-unit head box at the
   head part's own origin, drawn with whatever matrices that head is already
   bound to.  Everybody in the world who is a person wears it -- see
   humanoid.c -- rather than each of them wearing an imitation of it. */
extern Vtx steve_face_verts[];
extern Gfx steve_face_display_list[];
/* The triangle order every box model in the game shares. */
extern Gfx box_display_list[];
/* Whatever is in a hand, drawn at the origin of that hand. */
void drawToolGeometry(u8 item);
/* The day/night level the entity passes load as their primitive colour, for
   a pass that has its own colour to fold into it. */
void graphicsEntityTintRGB(u8 *out_rgb);
/*
 * A part's whole modelview: rotate about the model's own origin, then move it
 * into the world, in one matrix and one gSPMatrix.  See graphics.c for why
 * the pair a model used to load never needed to be a pair.
 *
 * `modelMatrixFrom` takes a linear transform already built -- a scale, or a
 * rotation with a scale folded in -- and writes into it, so the caller's
 * scratch must be a local rather than a table it wants to keep.
 */
void modelMatrix(Mtx *out, float pitch, float yaw, float roll, float x,
  float y, float z);
void modelMatrixFrom(Mtx *out, float linear[4][4], float x, float y, float z);
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
  /* Not buttons, but every screen that names one names buttons alongside it. */
  BUTTON_ICON_STICK,
  BUTTON_ICON_DPAD,
  BUTTON_ICON_COUNT
} ButtonIconId;

/* For a legend entry whose control is a single button: drawing it is a no-op
   and it measures zero, so the second slot simply disappears. */
#define BUTTON_ICON_NONE BUTTON_ICON_COUNT

void drawButtonIcon(ButtonIconId id, u32 x, u32 y);
/* The round buttons are 13x13, the shoulders 19x11 and the D-pad 13x13; ask
   rather than assume, so a label can be laid out beside any of them. */
u32 buttonIconWidth(ButtonIconId id);
u32 buttonIconHeight(ButtonIconId id);

/*
 * A legend row: each control, then what pressing it does.  `icon2` is for the
 * controls that are a pair -- C left and C right moving one cursor -- and is
 * BUTTON_ICON_NONE otherwise.
 *
 * A row is drawn twice, once per phase: drawLegendIcons from the screen's
 * fills, drawLegendLabels from its text.  Give both the same entries, x and
 * y, and the words land on their icons; there is deliberately no single call
 * that does both, because there is no phase in which it would be legal.
 */
typedef struct {
  ButtonIconId icon;
  ButtonIconId icon2;
  const char *label;
} LegendEntry;

#define LEGEND_ROW_HEIGHT 13
#define LEGEND_ICON_GAP 3
#define LEGEND_PAIR_GAP 2
#define LEGEND_ENTRY_GAP 11
/* Centres an 8px glyph box against a 13px button. */
#define LEGEND_LABEL_DROP 2
/* Icons drawn in one grouped pass.  Sized for the longest legend in the game
   -- the how-to column, thirteen controls of which four are pairs -- with
   room over; anything longer is truncated rather than split. */
#define LEGEND_MAX_ICONS 24

#define LEGEND_COUNT(table) ((u8) (sizeof (table) / sizeof (LegendEntry)))

/*
 * The world setup card's switches, on the same fill-sprite machinery as the
 * buttons and for the same reasons: a bright border the near-black panel
 * cannot swallow, a mark two pixels thick, and one grouped pass so eleven
 * switches cost a handful of pipe syncs rather than thirty.
 *
 * `dim` is a row the current world cannot offer.  Fills phase only.
 */
#define CHECK_MARK_BOX 0    /* an independent toggle */
#define CHECK_MARK_RADIO 1  /* one of a mutually exclusive group */

typedef struct {
  u8 kind;
  u8 on;
  u8 dim;
  u16 x;
  u16 y;
} CheckMarkPlacement;

void drawCheckMarks(const CheckMarkPlacement *list, u8 count);
u32 checkMarkSize(void);

u32 legendEntryWidth(const LegendEntry *entry);
u32 legendWidth(const LegendEntry *entries, u8 count);
void drawLegendIcons(const LegendEntry *entries, u8 count, u32 x, u32 y);
void drawLegendLabels(const LegendEntry *entries, u8 count, u32 x, u32 y);

/*
 * The same entries stacked instead of strung out: one control per line, every
 * label starting at the same x.  A row wants the icons packed tight against
 * their words; a column wants the words in a straight edge, or a list of a
 * dozen controls reads as a dozen unrelated fragments.
 *
 * legendColumnIconWidth is that shared indent -- the widest icon cell in the
 * table -- so a caller can centre the whole block.
 */
u32 legendColumnIconWidth(const LegendEntry *entries, u8 count);
u32 legendColumnWidth(const LegendEntry *entries, u8 count);
void drawLegendColumnIcons(const LegendEntry *entries, u8 count, u32 x, u32 y,
  u32 pitch);
void drawLegendColumnLabels(const LegendEntry *entries, u8 count, u32 x, u32 y,
  u32 pitch);

void draw(int can_reclaim_mesh_arena);
/* Frames that exceeded the command budget and had to shed terrain or be
   dropped.  Non-zero means the frame list was overflowing. */
extern u32 frame_overflows;

#endif /* GRAPHICS_H */
