# Streaming world plan (tasks 2–5)

Handoff for a future session. Goal: replace the fixed 112×112 world with a
residency window that streams around the player, and spend the freed budget on
view distance. Task 1 (window storage) and the per-frame work budget are
already in; what follows builds on them.

## Read first

`README.md` → *Hardware notes → Two hardware faults that emulators do not
reproduce*. Both cost a full debugging session. Do not reintroduce either.

Mine64 runs on real hardware over SummerCart64 (`./live-load`). It can be
compiled but not run or observed from a dev machine, so **hardware bugs are not
solvable by reading source** — build on-screen counters and controller-driven
switches and bisect on the device. That is what finally found the RDP hazard
after five wrong theories.

## What already landed

- **Window storage** — `window_blocks[256][1024]` + `window_keys[256]`, slot =
  low 4 bits of each chunk coord, residency folded into bit 31 of the key so a
  lookup is one load and one compare. `blockGet`/`blockSet` already take `int`,
  so widening coordinates will not re-touch every call site.
  `BLOCK_NOT_RESIDENT` (0xFF) distinguishes "not loaded" from "air", which is
  what lets meshing defer a column whose neighbours have not streamed in.
- **Sliced generation and meshing** — `beginWorldGeneration` /
  `stepWorldGeneration`, `beginWorldMeshBuild` / `stepWorldMeshBuild`, driven
  from `callbackGfx` with per-frame budgets (`WORLD_GEN_COLUMNS_PER_STEP`,
  `WORLD_MESH_COLUMNS_PER_STEP` in `src/main.c`). Mesh builds target the arena
  that is *not* on screen and publish on completion.
- **Surface-shell preview mesh** — the picker compiles roughly 20–35 quads per
  column instead of ~170 by dropping everything below the surface. Gameplay
  gets its own full-detail arena.
- **Frame budget guard** — `drawTextured` stops at `frame_dlp_limit`; `draw()`
  drops an over-length frame rather than submitting it. `-DNDEBUG` compiles
  `assert` out, so this cannot rely on asserts. Surfaces as
  `FRAME BUDGET EXCEEDED` on the picker.
- **T-junction cap** — `splitTjunctions` was O(n³) per plane and unbounded;
  capped at `MAX_TJUNCTION_REFINEMENTS`, and skipped entirely for the scenic
  preview mesh.
- **Task 3, coordinate-hashed decoration** — see below; generation is now a
  pure function of `(x, z, world_seed)`, checked by `tools/gentest/run.sh`.

## The cost model that drives everything

Per column (8×32×8 blocks):

| | cost | needed for |
|---|---|---|
| block data | **1 KiB** packed | collision, mobs, mining — including behind the player |
| mesh | **~5 KiB** | rendering only |

Mesh is 5× the cost with the *smaller* useful radius, so residency is two rings,
not one. Both must be **discs, not frustum cones** — walking exposes one new row
of columns, but spinning 180° invalidates the whole frustum at once, so a
cone-shaped mesh ring stalls every time the player turns around. Prefetch
biases by heading *within* the disc.

Because generation is a pure function of `(x, z, seed)`, an unmodified chunk
costs zero bytes: evict it and regenerate it identically later. Eviction is
**clean → drop, dirty → write diff**.

## Task 2 — Widen coordinates, rebase the origin — *in progress*

**Two decisions taken that differ from what is written below.**

*Coordinates stay absolute.* Rather than shifting entity positions when the
origin moves, block coordinates (`s32`) and entity positions (`float`) stay
absolute, and the render origin is subtracted only where a world position
becomes an `Mtx`. That is five sites — `cam_translate`, `steve_translate`,
`mob_translate`, `dropped_item_translate`, `falling_tree_translate` — plus
`c_models`, and it means no churn at all in `player.c`, `mobs.c` or `items.c`.
The cost is float precision: the mantissa gives about 1/128 of a block at
±100k blocks out, which is far past anywhere this game can be walked.

*Streaming lands before rebasing.* Inside ±512 blocks the matrices are already
valid, so streaming can be built and debugged there first and any fault is
unambiguously a streaming fault. Given the console cannot be observed from a
dev machine, not stacking two new failure modes is worth more than doing these
in the order they are numbered.

**Increment 1 — per-column tables indexed by window slot — done.** A slot is
the only name a column has once the world loses its edges, and it is what stays
valid when the window rebinds. `c_models` (now `[WINDOW_SLOTS][CHUNKS_Y]`,
written by `makeColumnDisplayLists` rather than prebaked in `initGraphics`),
`column_starts`, `dirty_columns` and `visible_columns` all moved. Iteration
still walks the fixed extent, so this is storage relabelling with no visible
effect — deliberately, because it has to be verified on hardware.
`drawTextured` in particular still walks world order: slot order draws the same
columns in a different sequence, and sequence decides both alpha-blended water
compositing and which columns the frame budget sheds. Costs ~23 KiB
(`c_models` 50→64 KiB, `column_starts` 25→33 KiB); non-audio free RAM
392→369 KiB.

**Increment 2 — per-column generation — done.** Generation no longer assumes
it is walking the whole world in x order.

- The three rotating world-width height rows are gone, replaced by a 10×10
  `height_patch` per chunk. The rows got terrain sampling down to one
  multi-octave sample per block column, but only by sweeping x and keeping the
  adjacent rows alive. The patch costs ~1.6 samples per block column and buys
  the ability to build a chunk alone.
- A column advances `EMPTY → TERRAIN → WAYSTONED → DECORATED`, gated on its
  neighbours. Decoration reaches across boundaries — a tree writes canopy two
  blocks out, a waystone reads ground two blocks east and south — and
  waystones must all be placed before any tree decides, because a waystone
  takes ground a tree would have rooted in. The gates are what make the result
  independent of the order columns stream in.
- `stepWorldGeneration` now walks *chunk* columns in three whole-extent
  passes, which satisfies the same gates globally. `WORLD_GEN_COLUMNS_PER_STEP`
  is therefore 1, not 48.

**The invariant streaming must honour:** a neighbour that is not resident
counts as satisfied, not as behind — otherwise a fixed world's edge columns
would wait forever on columns outside the map. So **the ring that gets terrain
must be one column wider than the ring that gets decorated**, or a canopy
reaching into an unclaimed column is silently dropped.

Verified in `tools/gentest`: building every column singly, in a shuffled order,
with decoration driven to a fixpoint in a freshly shuffled order each round,
produces a world byte-identical to the whole-world passes — across eight seeds.
Multiple seeds matter because a tree canopy suppresses a tree that would have
rooted under it, so adjacent-column trees are an order-dependent pair a single
seed can easily not contain.

One accepted change: decoration order moved from block-major over the whole
world to chunk-column-major, so a *newly generated* world can differ from what
earlier code produced for the same seed, in the rare case of two features
within two blocks of each other. Saves carry blocks, so nothing existing moves.

**Increment 3 — coordinate safety — done.** `tree_at_root` was indexed
`x * MAX_Z + z` into 12544 entries with a `u8` x: at x=255 that is index 28815,
a silent 16 KiB out-of-bounds write. Rekeyed to a window-relative 128×128
table. `player->target_x/z` and `spawnDroppedItem` widened to `int`; the
targeting raycast no longer has the world edge in its loop condition and
refuses to target unstreamed terrain. Bounds checks in player/camera/mob/item
code replaced by residency — `BLOCK_IS_SOLID(BLOCK_NOT_RESIDENT)` is TRUE, so
unloaded terrain already walls the player off. Tree leaf offsets now computed
in wrapping `u8`, which is correct across a 256-block wrap where widening to
`int` first is not, and keeps `TreeRecordV5` at its asserted 20 bytes.
`treesEvictColumn` hooked to slot rebinding so the 96-record pool is not
exhausted by walking.

**Increment 4 — the residency loop — done.** `stepWorldStreaming` keeps two
discs around the player: terrain at radius 7, decoration and meshing at 6. The
terrain disc is wider on purpose — that is the `neighboursReached` invariant.
Radius 7 spans 15 columns, the most a 16-slot window holds without two live
columns aliasing one slot. Nearest-first, so ground under the player fills
before the fringe.

Everything that walked a fixed extent had to follow: frustum culling is now
centred on the player (`CULL_SPAN`, `point_sides` indexed relative to the span
base, not absolute chunk), `drawTextured` walks slots, and compaction walks
slots rebuilding only resident `COLUMN_DECORATED` columns. `makeDisplayListsAt`
takes `int` and marks the west/north seam with a mask rather than a modulo,
since a negative coordinate's remainder is negative.

Two traps closed on the way: `windowClaimColumn` calls
`graphicsInvalidateColumnSlot` on rebind, or the outgoing column's mesh keeps
drawing at the outgoing column's position; and `worldMarkFixedExtentBuilt`
declares a loaded world complete, or streaming reads the save's columns as
`COLUMN_EMPTY` and regenerates fresh terrain over it.

**Saving is guarded while streaming.** `saveGame` still writes the whole
`0..MAX_X` extent, and columns the player has walked away from are not
resident, so `blockGet` would return `BLOCK_NOT_RESIDENT` and pack 0xF garbage
over the save. Both the D-pad handler and `saveGame` itself now refuse via
`worldFixedExtentResident()`, and the player sees "Too far from spawn to
save" rather than a corrupted world. Because `releaseColumnsOutsideRing`
evicts eagerly, the whole extent is only resident with the player within a
chunk or so of world centre — saving away from spawn stays impossible until
task 6 lands diff saves. `rebuildTreeLookup` also still rejects any record
with `tree->x >= MAX_X`. Task 6.

**Increment 5 — everything else that chained the world to the fixed extent —
done, not yet hardware-verified.** Streaming could claim columns anywhere,
but almost every consumer of those columns still assumed `0..111` (or
`0..255`, or non-negative):

- *The greedy mesher computed world coordinates in `u8`* — `r = cr *
  CHUNK_SIZE + br` wrapped at 256 and went wrong immediately for negative
  chunks — and clamped every x/z plane at `max_r = MAX_X - 1`, so columns
  past the old edge compiled empty or not at all. `blockAt` and the plane
  walk are `int` now and the edge clamp is gone; a plane against unloaded
  terrain stays empty because `BLOCK_NOT_RESIDENT` occludes the resident
  side's face and is barred (in `creationStep`) from growing a face of its
  own, so fixed-world output is unchanged.
- *`boxObstructed` treated `x >= MAX_X` as solid* — a literal invisible wall
  at the old world edge that made all of the above unreachable. Residency is
  the wall now (`BLOCK_IS_SOLID(BLOCK_NOT_RESIDENT)` is TRUE); only `y < 0`
  keeps a hard floor.
- *`noise2d`/`noise3d` truncated instead of flooring*, so fractional negative
  inputs extrapolated the smoothstep outside 0..1 — terrain west/north of the
  sample offsets came out as banded garbage. The 3D cave fields already
  sampled negative coordinates through this bug inside the fixed world, so
  flooring changes cave layout for a given seed; saves carry blocks, nothing
  existing moves. `oreInCell` similarly floor-divides its cells now.
- *Decoration clamped to the fixed extent*: `spawnTree`'s canopy loops
  clamped to `0..MAX`, `exposedGrassY` rejected coordinates outside it (so no
  waystones outside), `tryPlantTree` refused. All residency-driven now —
  outside the fixed world, trees were generating as bare trunks with no
  leaves and no waystones at all.
- *Block edits truncated coordinates*: `placeBlock`/`breakBlock` took `u8`
  x/z, and `breaking_x/z` in `Player` were `u8`, so past block 255 (or west
  of 0) mining reset every frame and edits went to the wrong column (in
  practice, silently nowhere, because the wrapped key never matched).
  `placeBlock` checks residency explicitly so a refused placement no longer
  consumes the item.
- *The target wireframe indexed `b_models` with `%`*, whose negative
  remainder walks out of the array west/north of origin; shift-and-mask now.

**Increment 6 — origin rebasing — done, not yet hardware-verified.**
As decided above, coordinates stay absolute; `render_origin_x/z` (blocks,
chunk-aligned, with premultiplied float mirrors) is subtracted only where a
world position becomes an `Mtx`: `cam_translate`, `steve_translate`,
`mob_translate`, `dropped_item_translate`, `falling_tree_translate`, and
`c_models`. `graphicsSetRenderOrigin` rewrites every resident slot's chunk
matrices in one pass; `callbackGfx` re-centres on the player when they wander
`REBASE_DISTANCE` (256) blocks from the origin, inside the no-task-in-flight
branch and before `draw()` — which is sufficient, because `draw()` rebuilds
the camera and entity matrices every frame, so no frame can mix origins.
`beginWorldGeneration` and `loadGame` reset the origin to zero. Falling
trees needed one extra step: tree records store wrapping `u8` coordinates,
so `unwrapTreeCoord` recovers the absolute position relative to the viewer
(always valid — a live tree is inside the window, within 128 blocks).

`tools/gentest` gained a far-lands section that drives `stepWorldStreaming`
itself: it walks the ring from spawn to chunk (−52, 63) — negative x beyond
every noise offset, z past the fixed extent — and asserts the walked-to ring
fully decorates, that the same terrain arrives with no walk history, that
eviction and return regenerate it byte-identically, and that far trees carry
canopies (the exact thing the old clamps broke).

**First hardware run: froze.** Walking one direction long enough hard-froze
the console, repeatably, even walking slowly.  Diagnostics immediately before
were healthy (R≈219 D≈153 Q0 A73 C0 T≈0), so the streaming loop was keeping
up and the fault is in something the host harness cannot run — meshing,
rendering, or the origin rebase.  The generation/streaming path also passed
ASan+UBSan on the host, including the 100-chunk walk test.

**The bisection build** (current tree) instruments for exactly this, per the
debug-on-device doctrine:

- The diagnostics are now a vertical stack on the left edge (clear of the
  compass): `X Z` player block position (magnitude only), `F` frame
  heartbeat mod 1000, `O` rebase count, `B` rebasing enabled, then the
  original `R D Q A C T`.
- `REBASE_DISTANCE` is **temporarily 64** (from 256), so the first rebase
  fires a few blocks east of spawn.  If rebasing is the fault, the freeze
  now reproduces seconds from spawn, exactly as `O` flips 0→1.
- Hold **Z+L+R and press C-right** to toggle rebasing off (`B` shows 0; the
  hotbar also cycles, harmless).  With it off, translations degrade past
  ±512 blocks (world geometry garbles progressively) instead of rebasing —
  ugly but expected, and it cleanly separates "rebase freezes" from
  "distance freezes".

Protocol: walk east from spawn watching `O` and `X`.  (1) Freeze right as
`O` first ticks → rebase implicated; repeat with `B 0` to confirm the freeze
moves far out.  (2) No freeze near `O` ticks but the far freeze still
happens → note `X`, `F`, and whether `F` was still ticking at the moment of
freeze; a frozen `F` means no frames were being built (RDP/RSP hang or a
graphics-thread loop), a ticking `F` over frozen terrain means the streaming
logic stalled.  Restore `REBASE_DISTANCE 256` and remove the toggle once the
cause is found.

**Hardware checklist for the next session** (nothing else above has been on
the console yet):

1. Walk east past block 112 — terrain must appear ahead, mesh and all, and
   the old edge must not stop you.
2. Walk west past block 0 — negative chunks are the highest-risk path
   (mesher, wireframe indexing, noise all changed there).
3. Keep walking one axis past ~768 blocks so at least one rebase fires —
   watch for a one-frame world jump (would mean a mixed-origin frame) and
   for any Mtx overflow garbling as ±512 approaches without a rebase.
4. Mine, place, and fell a tree far out (coordinates past 256 and negative).
5. D-pad save far out must show "Too far from spawn to save" and keep
   working (not "Save failed"); walk home, save, reload, verify the world.
6. Watch `T` (unstreamed columns in the ring) while sprinting: it should
   burst on chunk crossings and drain; a stuck non-zero `T` with the player
   against invisible walls means streaming is not keeping up.

Remaining: the LOD/throughput work in tasks 4 and 5, and task 6.

### Original notes

N64 `Mtx` is **s15.16 fixed point**. The integer part is 16 bits, so with
`BLOCK_SIZE 64` translations lose sub-block precision past ±32767/64 ≈ **±512
blocks**. Origin rebasing is forced by the hardware, not a preference.

- Widen x/z to `s32` world block coords across `world.h`, `graphics.h`,
  `trees.h`, `player.c`, `mobs.c`, `items.c`. `y` stays `u8` (0–31), so this is
  2D streaming — no 3D chunk graph, no vertical paging.
- Re-centre the render origin when the player crosses a boundary, shifting
  entity positions and window indices together.
- `trees.c` still has `tree_at_root[MAX_X * MAX_Z]`, a fixed-world table that
  has to become window-relative.
- `c_models[NUM_CHUNKS]` prebakes absolute chunk translations in
  `initGraphics`; these become window-slot relative and must be rewritten when
  a slot is rebound.

## Task 3 — Coordinate-hashed decoration — **done**

The problem was worse than "decoration consumes the RNG in world order". `seed`
was doing two jobs: it is the xorshift state `random()` advances on every call,
*and* it was what `noise2`/`noise3` salted the Perlin field with. So the noise
function itself changed shape partway through generating a world — the tree
stage sampled its density field with a different seed on every column, because
`generateWaystones()` had just advanced `seed` a few hundred times and each
`random(1000)` advanced it again.

What landed:

- **`world_seed` split out from `seed`** (`math.c`). `world_seed` is written
  once per world and never again; `seed` remains the gameplay RNG for mob AI,
  drop velocities and tree-fall direction, which legitimately want a stream.
  `noise.c` samples `world_seed`. Nothing in `world.c` calls `random()` now.
- **`coordinateHash` gained a seed and a salt.** It did *not* already take the
  seed — ore veins were identical in every world. The salt separates
  independent questions about one coordinate (tree height vs. canopy shape),
  which is cheaper and better behaved than slicing bit fields out of one hash.
- **Trees** — scatter, trunk height and all eight canopy corners are hashed.
- **Waystones** — were ten landmarks drawn from the RNG with up to 32 retries
  each, a question no single column can answer about itself. Now a per-column
  probability (`WAYSTONE_COLUMN_ODDS`), which is also the only formulation that
  means anything once the world has no fixed size. Measured mean is 10.5 per
  112×112 world against the old fixed 10, with the wider spread a density per
  unit area necessarily has.

**`tools/gentest/run.sh`** compiles `world.c`/`noise.c`/`math.c` for the host
against small shims and asserts that slice size, one-shot vs. sliced
generation, and arbitrary gameplay-RNG traffic during generation all leave the
world byte-identical. Generation is pure arithmetic, so unlike the rest of
Mine64 it *can* be checked from a dev machine — do that before trusting a
change here.

Two things this deliberately did **not** solve, both owned by later tasks:

- **`world_seed` is still not saved.** Harmless today because saves carry the
  whole block array and nothing regenerates, but a loaded world currently has
  no seed of its own. Task 6 must persist it; see the note there.
- **Features write across column boundaries.** A tree writes ±2 blocks in x/z
  and a waystone places outriggers at +2. Decisions are now pure, but
  regenerating one column in isolation still has to evaluate decoration for
  every column within that radius, or canopies and outriggers reaching in from
  neighbours go missing. Task 5 needs a generate-with-margin pass.

One deliberate behaviour change: player-planted saplings (`tryPlantTree`) also
go through the hashed path, so replanting at the same spot regrows the same
tree. Planting is an edit and will be captured as a diff either way, so this is
harmless — but it is a choice, and reverting it just means giving `spawnTree` a
flag to draw shape from `random()` instead.

## Task 4 — Drop per-quad matrices

Every quad currently costs two matrix ops:

```c
gSPMatrix(c_models + chunk, MODELVIEW|LOAD|NOPUSH);   /* chunk translate */
gSPMatrix(b_models + b,     MODELVIEW|MUL|NOPUSH);    /* block translate — 4x4 MULTIPLY */
gSPVertex(QUAD_ADDR(face, width, height), 4, 0);
gSP1Quadrangle(3, 2, 1, 0, 0);
```

Quads live at the origin in the shared `quads.h` table and are positioned by
matrices — cheap in RAM, very expensive on the RSP. Replace with per-column
baked vertex buffers in chunk-local space: one `gSPMatrix` per chunk,
`gSPVertex` batched up to 32 verts (8 quads) per command, `gSP1Quadrangle` per
quad.

Trade: ~73 bytes/quad vs 32 today (**~2.3× mesh memory**) for roughly **2× RSP
throughput**. This does not pay for itself alone — the current fixed world
cannot absorb the memory. It only works together with task 5's LOD, which is
why they land as a pair.

## Task 5 — Two-ring residency with LOD

- Block ring: generous disc, cheap, covers physics and turning.
- Mesh disc: sized to view distance; far columns use the **surface-shell path
  that already exists** (`building_surface_mesh` in `graphics.c`), cutting
  ~170 quads/column to ~20.
- Mesh arena becomes an LRU cache with eviction rather than a whole-world
  snapshot. The main design fork is **fixed-size column slots** (O(1)
  eviction, no fragmentation, wastes space on sparse columns) versus keeping
  the bump arena and leaning on the existing incremental compaction. Decide
  this before writing code.

Rough target: near ring at full detail plus a far ring at surface LOD lands
near **1.1 MiB for ~300 visible columns**, against 2.2 MiB for 196 today —
roughly 2.4× the view distance at half the memory, and unbounded world size.

## Task 6 — Per-chunk diff saves

Saves currently write the whole packed block array in the original x-major byte
order (deliberately, so pre-window saves still load). Streaming needs seed +
player state + diffs for modified chunks only. `SAVE_VERSION` is 10 and loading
is already version-gated, so a bump is idiomatic. Cap or LRU the stored diffs —
a player who modifies everything is otherwise unbounded.

**`world_seed` must land here.** Task 3 made generation reproducible from
`(x, z, world_seed)`, but nothing writes that seed to disk, so a loaded world
cannot regenerate anything — the moment task 5 evicts a clean chunk from a
loaded save, it comes back as a different chunk. This was left for task 6 on
purpose: the version gate in `loadGame()` is one dense boolean covering V1–V10,
and adding an interim V11 that task 6 immediately replaces is throwaway work
against the one file where a mistake destroys player worlds. Do it once, here.

## Constraints to respect

- **Audio build headroom is ~43 KiB** and shrinking (`tools/check_ram.py` warns
  under 64 KiB and fails on overrun). NuSystem pins framebuffers and the audio
  heap at fixed absolute addresses; the link must stay below them. Task 4/5
  reclaiming mesh memory is what fixes this — until then, do not ship audio.
- The 16×16 window currently wastes ~60 KiB on slots outside the 14×14 world.
  That disappears once streaming uses the full window.
- Never busy-wait on the graphics thread; gate on `pendingGfx` instead.
- Always `gDPPipeSync` before reconfiguring the RDP.

## Known loose end

`diag_arena_used` reported **0 while terrain was visibly rendering**, twice.
The published arena and the arena the column pointers refer to appeared to
diverge. The diagnostics have since been removed and the console is stable, so
this may have been an instrumentation artefact — but it was never explained. If
arena handoff misbehaves during task 5, start here.
