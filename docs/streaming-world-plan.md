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

## Task 2 — Widen coordinates, rebase the origin

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
