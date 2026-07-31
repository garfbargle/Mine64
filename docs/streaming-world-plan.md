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

**Bisection run 1 verdict: the origin rebase is innocent.** The freeze
reproduced at (x 69, z 15) with the frozen screen reading `O 0` — the rebase
had never once run (a controller toggle in that build had also disabled it).
Other frozen-screen values: `F 485` (frozen — no frames being built),
`R 225 D 157 Q 5 A 42 C 0 T 0` — streaming completely healthy to the last
frame.  Notable: the walk was north-east, so the residency ring spanned
negative z chunks (pcz 1 → ring to cz −6), and the freeze came after only
~43 blocks of walking — the trigger is not raw distance from spawn.
`REBASE_DISTANCE` is restored to 256 (so any repro under ~250 blocks is
rebase-free by construction) and the toggle is removed — its Z+L+R combo
collided with sprint/look/jump and self-toggled during normal play.

**The current build adds the phase square**: a 16×16 square near the lower
left, painted directly into the displayed framebuffer by the CPU — no RSP,
no RDP — so it keeps reporting after the graphics pipeline dies.  While the
game runs it flickers as frames paint over it.  On a freeze, its color is
the finding:

| color | meaning |
|---|---|
| red | a graphics task sat unfinished for ~2s: the RSP/RDP hung executing it |
| green | CPU died inside `stepWorldStreaming` (generation/decoration) |
| yellow | CPU died inside the origin rebase |
| cyan | CPU died inside `draw()` — mesh compile, culling, or submission |
| magenta | CPU died inside `updatePlayers` — physics or input |
| blue | callbacks stopped arriving with the last one completing normally |

Red means chasing the *content* of a display list (a column mesh the RDP
cannot digest); green/cyan/magenta mean a CPU loop in that subsystem; blue
would implicate the scheduler.  Report the color together with the `X Z`
rows off the frozen diagnostics stack.

**Bisection run 2: the square VANISHED at the freeze** (at x 100, z 169;
in life it read solid blue idle, green/blue flicker walking, mostly cyan at
speed — draw() dominating, as expected).  The vanishing is itself evidence:
the phase was painted only into the displayed buffer, and the draw task
already submitted that callback completes on the RSP/RDP *regardless of the
CPU*, repaints the other buffer entirely and swaps to it.  So the CPU died
**after draw() submitted the frame** — in the update path (player physics /
input, trees, items, mobs) — and the swap erased the painted evidence.
`detectCollision` and `updateTargetBlock` were re-audited on this suspicion
and both provably terminate (`max_t <= 1`; `t` grows monotonically to
`ray_limit`), so it is not the obvious ray-marches.

**Run-3 build** makes the evidence survive anything short of a power cycle:

- The phase is *recorded* (`diag_current_phase`) and a dedicated **watchdog
  thread at priority 126** — woken by a hardware timer, immune to a spinning
  or dead graphics thread — repaints it into **both** framebuffers every
  second once the `diag_heartbeat` callback counter stalls for 2s.  The
  colour on a frozen screen is now trustworthy.
- The update path is split: **magenta** = player input/physics, **orange** =
  trees, **white** = items, **black** = mobs.  Green streaming, cyan draw,
  yellow rebase, blue clean-idle, red RSP/RDP-hang as before.
- New diagnostics rows `M` (peak Gfx commands one frame has used) and `V`
  (frame overflow count).  Everything after the terrain shed limit — HUD,
  entities, these diagnostics — writes into the 2048-command tail reserve
  *unguarded*; if `M` approaches 6656 or `V` ticks, frames have been writing
  past the buffer into adjacent BSS, which is exactly the kind of silent
  corruption that produces wandering, healthy-looking freezes.

Report on the next freeze: the (now persistent) square colour, plus `X Z M
V` from the stack.

**Bisection run 3: no freeze — and the milestone.**  The player reached
**x 572** (z 70) on real hardware: past the ±512 s15.16 limit, with `O 2`
rebases fired and no visible seam, jump, or garbling.  Origin rebasing is
hardware-verified; the world is walkably unbounded for the first time.
Streaming stayed textbook throughout (R 225, T 0, Q 0, A 59, C 0), and the
frame list peaked at `M 2433` of 6656 with `V 0` — the tail-overflow theory
is dead.

That leaves the run-1/run-2 freezes **unexplained**: no game logic changed
between the freezing build and this one, only instrumentation (a watchdog
thread, its stack, counters), which shifted BSS layout and timing.  Treat
the fault as latent and masked, not fixed.  The forensics harness therefore
**stays in the build** until the freeze recurs (the watchdog-painted square
now survives swaps, loops and thread death, so it will name the subsystem)
or until long sessions justify calling it gone.  Still unwalked from the
hardware checklist: standing in negative coordinates, far-out block edits
and tree felling, the far-save refusal message, and re-walking evicted
terrain.

**Post-run-3 audit: two latent far-coordinate faults found and fixed, plus
the compaction feedback loop.**  A source audit after run 3 found concrete
faults consistent with "latent and masked", all now fixed and
gentest-verified, none yet on hardware:

- **`removeTreeBlocks` stack overflow (trees.c).**  Its changed-column array
  was 196 entries indexed by raw u8 record coordinates — up to index 465, a
  ~270-byte write past a stack array, for any felling with a wrapped
  coordinate past 111.  Worse, every tree function that touched blocks
  (`discardMissingParts`, `removeTreeBlocks`, `emitDebris`) read and wrote
  through the wrapped u8 directly; past the first 256 blocks those
  coordinates fail residency, so far fellings silently did nothing and far
  debris spawned up to 256 blocks away.  All three now recover absolute
  coordinates via `treeAbsoluteRoot()`: the u8 wrap is a whole multiple of
  the window span, so the record's own slot key holds the exact absolute
  chunk.  The changed-column array is gone outright — per-block
  `makeDisplayListsAt` marking replaced it.
- **Compaction fed itself.**  `column_meshed` was one array shared by both
  arenas; compaction start cleared it for all 256 slots, the streaming ring
  scan then re-marked the whole ring dirty every frame, and after the pass
  published those rebuilds re-orphaned the fresh arena straight back under
  its reserve — permanent compaction, CPU-bound, with every counter reading
  healthy.  `column_meshed` is now per-arena, `graphicsColumnNeedsMesh`
  reads only the active arena, a publish arms a 120-frame cooldown before
  the next compaction may start, and the per-frame budget charges rebuilds
  (2) and slot walking (64) separately instead of six of whatever comes.
- **Saves could carry poison tree records.**  With the ring wider than the
  world, a save at spawn can include live trees at chunk −1/14 whose wrapped
  coordinates are ≥112 — which `treesValid` rejects on the *next load*,
  reading the whole file as corrupt and falling back to the backup.
  `treesDropOutsideFixedExtent()` now retires them before the checksum.
- Dead blocking builds (`buildAllColumns`, `makeWorldDisplayLists`,
  `makeGameWorldDisplayLists`, `worldMeshBuildActive`) deleted;
  `drawFallingTrees` no longer double-scans 96 records with four `floor()`
  calls each when nothing is falling.

**Bisection run 4: MAGENTA at (x 120, z 31)** — first run of the baked-verts
+ LOD build.  The CPU died inside the *player* phase (`updatePlayers`), with
F frozen at 407, O 0, M 2449, V 0, R 213, D 153, Q 17, A 27, C 0, T 12.
Same family as the unexplained run-1/2 freezes: update-path death while
walking, streaming healthy.  Also reported: movement felt like quicksand —
consistent with a collapsed frame rate plus the MAX_FRAME_DELTA clamp
slowing the simulation, and with A 27 / Q 17 showing far more arena traffic
than the LOD split predicts.  Two suspicious loops in the player phase
(`updatePlayer`'s collision resolve, `detectCollision`'s boundary march)
terminate on clean math but have razor-thin float assumptions.

**Run-5 build sharpens the evidence rather than guessing:**

- The frozen square now has **three bands**: top = phase (red = RSP hang),
  middle = last player sub-step (green objectives, yellow input/steer,
  orange vault, red collision resolve, cyan targeting, magenta actions,
  blue post), bottom = **white if the CPU took an OS_EVENT_FAULT (crash)**,
  black if not (loop).  Crash vs loop are entirely different hunts.
- Both suspect loops now carry **runaway guards**: the collision resolve
  breaks after 8 axis hits, the boundary march gives up after 128 steps.
  If one of these was the freeze, the console now *survives* and the new
  `L` row counts the clamps instead.  A rising L with no freeze is the
  confession.
- New rows `W` and `B` (tenths of ms, worst over ~2s): W is the real frame
  time (166 = 60 Hz, 1000 = 10 fps); B is the CPU cost of the gated
  streaming/mesh/draw block.  B ≈ W blames callback CPU work; W high with
  B low blames the RSP/RDP.  This is the quicksand diagnosis.

Report on the next freeze: all three band colours plus X Z W B L Q A.  If
there is no freeze but quicksand returns, photograph the stack while it is
happening.

**Bisection run 5: yellow / blue / WHITE at (x 113, z 432)** — three findings
at once:

- **The player-phase freeze is caught.** `L 18`: the new loop guards fired
  eighteen times and the console survived every one.  Whatever drives the
  collision code degenerate is real and frequent, but it no longer kills the
  machine — it is clamped and counted.
- **The quicksand is diagnosed.** `W 1430 B 1419`: single callbacks were
  spending ~143 ms of pure CPU in the gated streaming/mesh block while
  physics kept running at retrace pace.  The RSP was never the problem.
- **A new death: a CPU FAULT (white band) inside the origin rebase (yellow
  phase), with O 1.**  At those coordinates a second rebase should not even
  have fired unless x was negative — or the position was corrupt.  A
  corrupt position both *triggers* a spurious rebase and *faults* inside
  it: guTranslate's float→s15.16 conversion of an insane value raises the
  VR4300's unimplemented-operation exception.  Eighteen collision clamps
  upstream are eighteen chances for physics to have produced exactly that.

**Run-6 build — evidence and armour:**

- **SD post-mortem.**  Once the heartbeat is 2 s stale the watchdog writes
  `mine64/freeze.txt`: faulting thread id, PC, CAUSE, BADVADDR, RA, SP,
  plus raw position bits, origin, and every counter.  With `mine64.out`'s
  symbol map, PC/RA turn the next freeze into a source line —
  `mips-n64-objdump` or `nm` on the .out resolves it.
- **Position sanity snap** at the top of `updatePlayer`: a non-finite or
  absurd position is replaced with the last known-good one (G row counts
  it).  The rebase separately refuses insane coordinates.  If the corrupt-
  position theory is right, the fault becomes a G tick and play continues.
- **Time-sliced streaming.**  `stream_work_deadline` caps the gated block's
  generation/decoration/meshing at ~10 ms per callback (first unit of work
  always runs, so nothing starves; the loading screen is uncapped).  W/B
  should now hover near 166/100 while walking, and the quicksand should be
  gone — terrain arrives over a few more frames instead.

**Run 6: the black box delivered.**  Two freezes on the run-6 build (one
photographed at x 517 z 639 with O 3, G 0, L 25; W 521 B 511 —
time-slicing killed the quicksand).  `mine64/freeze.txt` off the SD card
resolved the death exactly:

    PC 800504F8 = guTranslate+0x58  (trunc.w.s of the *z* argument)
    RA 8002CD24 = graphicsSetRenderOrigin+0x124
    CAUSE 1000003C = FPU exception (unimplemented operation), thread 4

`trunc.w.s` faults when the s15.16 conversion overflows: |z offset| ≥ 512
blocks.  The report's origin (56,56) and position (~56,59 blocks) are sane
and G 0 says the position guard never fired — so a **resident window key
decoded to a column hundreds of chunks away that no one ever walked to: a
stray store is corrupting `window_keys`**.  The x argument converted fine,
so the low halfword (the z field) was clobbered while the high half
survived — not a sequential overrun from `window_blocks`; more like a
16/32-bit store landing mid-key.  The writer is still unidentified.

**Run-7 build — catch the writer, survive the wound, fix the pop-in:**

- `windowAuditKeys` walks the keys every streaming step: a resident key
  decoding past ±2000 chunks is latched (first bad value + slot go to the
  freeze report as KVAL/KSLOT — the bit pattern should name the writer),
  counted on the new `K` row, and repaired by evicting the slot (the column
  regenerates from the seed).  `graphicsSetRenderOrigin` independently
  refuses any offset past ±500 blocks, so the fault site itself is armored.
- **Pop-in fix**: the 10 ms deadline was letting terrain generation starve
  decoration (and meshing waits on decoration) — blocks existed, mobs
  walked on them, the mesh arrived seconds late.  Every streaming stage now
  gets one guaranteed step per callback before the deadline may refuse it,
  and `nearestColumnNeeding` ranks candidates around a point two chunks
  ahead of the player's smoothed heading, so the ground being walked toward
  builds first instead of last.

Watch: K (any tick = corruption caught and survived; then pull freeze.txt
for KVAL/KSLOT), and whether the edge-of-world pop-in while sprinting is
gone.

**Hardware checklist for the next session** (nothing else above has been on
the console yet; checklist items 4 and 5 specifically exercise the tree and
save fixes described above — the far felling in item 4 would previously have
smashed the stack):

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

## Task 4 — Drop per-quad matrices — **done, not yet hardware-verified**

Landed together with the in-window half of task 5, because neither pays for
itself alone:

- **Baked vertex buffers** (`makeColumnTextureDL`, graphics.c): a full-detail
  column's quads copy their four vertices out of the shared table,
  pre-translated into column-local space (y spans all four chunks, so one
  matrix serves the whole column), batched 8 quads per gSPVertex.  The two
  per-quad matrix operations are gone from the near terrain entirely.
- **Two-level LOD inside the window**: full detail within Chebyshev 3-4 of
  the nearest player (split-screen aware), the scenic surface shell —
  in the old compact matrix-pair format, whose memory cost is what makes it
  affordable — for the rest of the decorated ring.  Promote at 3, demote at
  5; the gap is hysteresis.  Freshly streamed columns always arrive as
  shells, and shells skip T-junction refinement, which was the dominant cost
  of compiling an arriving column — this is the fix for the walk-hitches.
- **Single-pass texture bucketing** (`resolveColumnQuads`): one walk of the
  greedy scratch into a flat per-texture-counted array replaced sixteen full
  rescans per column.

The old 16-bank rescan emitters (`makeColumnDL`, `makeQuadDL*`) are gone.
View distance is *unchanged* in this step: the mesh ring is still capped by
the decoration chain (terrain 7 → waystones 6 → trees 5) and the 16×16
window.  The view-distance payoff needs the window widened to 32×32, which
needs the ~1 MiB the two mesh arenas currently pin — see the revised task 5.

### Original notes

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

## Task 5 — Two-ring residency with LOD — **done**

**Deferred underground generation — done.**  The sprint "bonk" was terrain
generation losing the race: at radius 12 a chunk crossing bills ~25 fresh
columns, and the cave carve (two 3D noise samples per underground block) is
most of each one's cost.  Deep generation is now restructured as *surface
fill + carve pass* sharing one `carveTerrainColumn`, far columns outside
the fixed save extent generate surface-only (~3-4× cheaper), and a deepen
stage carves the underground within `STREAM_DEEPEN_RADIUS` 5 of the player
-- caves never breach their three-block roof, so the surface is identical
either way and the frontier is invisible.  The full-detail LOD waits for
depth (`worldColumnDeep`), the fixed extent always generates deep so saves
never see uncarved stone, and shared-carve construction makes
shallow+deepen ≡ deep byte-identical -- the far-lands harness now proves it
end to end.  Digging can only reach deepened columns (reach ≪ 40 blocks).

**Hardware retune after the first 32×32 run:** radius 12 / cap 160 was
measurably choppy and the arbitrary mesh order was visible ("terrain paints
in from the fog toward me").  Now: radii 12/11/10 (view ~80 blocks, twice
the original), solo cap 120, arena back to 1 MiB (headroom 304 KiB),
`takeDirtyColumn` picks nearest-player-first so meshing radiates from the
player outward, and the 25 ms urgency boost keys off *missing* mesh only --
treating routine LOD promotions as emergencies made every chunk crossing
hitch.  Deferred-underground generation (surface-only far columns, deepen
on approach) is logged as the next big streaming-CPU win.

**The 32×32 window — done, not yet hardware-verified.**  `WINDOW_SHIFT` 5,
radii terrain 14 / waystones 13 / decorated+mesh 12: **view distance ~96
blocks, 2.2× the old ring**, with the whole far field in shell LOD behind
the fog.  What paid for it: the reclaimed arena megabyte (block window
1 MiB), c_models dropped to one matrix per column with b_models extended to
full column height (the baked format made the chunk dimension pointless),
`tree_at_root` at the full 256-block wrap, the live tree pool at 160 with
the on-disk payload frozen at 96 records (`treesDropOutsideFixedExtent`
compacts below that line, so v10 saves stay byte-compatible), culling spans
`CULL_RADIUS = STREAM_TREE_RADIUS` with the cap-trim rebuilt from a compact
candidate list, solo visible cap 160.  Mesh arena 1.125 MiB.  Link headroom
175 KiB — **audio is deferred**: it needs ~384 KiB, which arrives either
with a pooled (indirected) block window or post-diff-save trimming.
Saving also got roomier for free: the radius-14 terrain ring covers the
whole fixed extent from anywhere inside it, so "too far to save" now only
triggers genuinely far from the original footprint.

Hardware checks: the horizon at ~96 blocks (retune the fog start — the P
row wants to sit much farther out now), initial world entry filling ~500
columns behind the fog, W/B with the 160-column cap in dense terrain, A row
pressure (alloc-failure cooldowns would show as missing far columns that
heal), and long-walk stability as before.

**Single-arena rewrite — done, hardware-verified.**  The
double-buffered pair and the whole compaction machinery are gone.  One
1 MiB arena, first-fit blocks (one per column: baked vertex region first,
then per-texture command segments), and a relocation defrag that slides one
block per callback into the lowest gap -- safe because every mutation runs
at `pendingGfx == 0` and finishes before that callback's `draw()`.  The
patcher fixes the moved block's own gSPVertex addresses (range-checked, so
a shell's static-table references are untouched) and the per-texture start
pointers.  World builds emit a new block *generation* behind staged
pointers while the outgoing world renders on, then publish by pointer-copy
and free the old generation -- a shell preview plus a full game mesh share
the arena comfortably.  Allocation failure keeps the old mesh, keeps the
dirty mark, and arms a 60-frame backoff.  Link headroom went 336 KiB →
**1351 KiB**: the megabyte the 32×32 window needs.

Also landed: the "walk to the edge and wait ~3 s" fix -- the stage pipeline
gets a 25 ms deadline instead of 10 ms whenever a column within two chunks
of a player is unbuilt or unmeshed, and streaming ranking is biased two
chunks toward the player's smoothed heading.

Hardware checks for this build: terrain integrity after long walks (defrag
relocation is the new risk -- garbled or vanishing columns would implicate
the patcher), menu↔game and preview↔preview transitions (generation
staging), the A row on the single arena, C now showing allocated block
count, and whether the edge wait is gone.

The order the RAM math forced, for the record: single arena first (reclaim
the megabyte), then the window widening that spends it, then retuning the
budgets, culling extent and visible caps on hardware.  All three are above.
The original sketch aimed at radius 13 and ~104 blocks; hardware said radius
12 was choppy, and the shipped ring is 10.

## What landed after task 5

The streaming arc is finished, and the work since has been gameplay built on
top of it plus two performance/correctness faults that the new surface area
exposed.  Recorded here because each one is a trap the streaming design
makes easy to fall into again.

**Structures, details, and the edit journal.**  Three subsystems that all
exist because a streamed world cannot store what it has not generated:

- *Structures* (`world.c`) — hamlets and ruins on a coarse 64×64 macrocell
  grid, hashed from the cell coordinate exactly like trees and ores, with a
  small plan cache.  A cell has at most one structure and its plan is a pure
  function of the cell, so no structure map is stored and no oversized
  resident margin is needed.  They advance the column state machine's middle
  stage, which is why `COLUMN_WAYSTONED` is now `COLUMN_STRUCTURED`.
- *Details* (`details.c`) — torches, stairs, doors and windows.  The packed
  world has exactly sixteen block IDs and no room for orientation or open/
  closed state, so these keep sparse records and occupy their cell with
  `CRAFTING_TABLE` as a proxy.  An old ID-10 cell with no record is still an
  ordinary crafting table, which is what keeps existing saves readable.
- *Edits* (`edits.c`) — a 2048-entry journal of player deviations, re-applied
  when a column regenerates.  Terrain stays procedural; only deviations are
  retained, so an unbounded world does not imply unbounded RDRAM.

Both pools are re-applied in `worldAdvanceColumnDecoration` before a column
reaches `COLUMN_DECORATED`, which is also the gate meshing waits on — that
ordering is load-bearing, and the pool-scan fix below depends on it.

**The pool scans that nearly killed the frame rate.**  `blockAt` gained a
detail lookup, and `detailIsCustomAt` walked all 384 records with no early
out — in every world, including the overwhelming majority that contain no
details at all.  `blockAt` runs 24,448 times per column the mesher compiles:
9.4 million pool iterations to build **one** column, roughly 0.4 s of pure
loop on the VR4300, paid at world build, at streaming, and again on every
place or break.  That is the "loading takes 10× longer" report.

Two facts make the common answer free, and both are worth remembering the
next time a pool is added: every live detail keeps its proxy ID in the
terrain, so one inlined nibble read settles any cell that cannot carry a
record; and records are handed out from the low end, so a high-water mark
(`detail_scan_limit`, `world_edit_scan_limit`) bounds the walk instead of the
ceiling.  Measured over 64 columns of an empty-detail world, 600,834,048
iterations became 0.  The same treatment was applied to
`worldApplyEditsToColumn`, `detailsEvictGeneratedColumn` (which runs on every
window claim *and* release) and `drawDetailsForPlayer`.

**Saving had been impossible since the world outgrew 64×64.**  `ffconf.h`
sets `FF_USE_LFN` to 0, so FatFs has no long-name support: `create_name()`
rejects any basename past eight characters or extension past three with
`FR_INVALID_NAME`, before touching the card.  `world_112_1.m64` overran both
limits, as `world_large_*` and `world_128_*` had before it — so `f_open`
failed on every save, and `f_stat` failing the same way made every slot read
as empty on the title screen.  Paths are now `mine64/w112_1.m64` and
friends.  `initStorage` also records *which* of its four failure modes it
hit (`storage_status`) and the title screen names it, because "no cart save
device" cannot distinguish a missing flashcart from an exFAT card — and on
hardware that distinction is the whole diagnosis.

**Also landed:** zombies and spiders with telegraphed strikes and a sunrise
retreat, budgeted five passive / three hostile inside the same fixed
eight-mob pool; hunger; a first-person idle sway; and a collision fix — cells
were computed with truncation toward zero, so negative-coordinate movement
could produce a negative collision time and grow the remaining sweep horizon
past one frame.  Floored grid coordinates, and collision progress can no
longer go backward.

**Loose end:** `useMobWeaponSpecial` and `mobWeaponSpecialCooldown` are
implemented and their effect timers are simulated, but nothing calls them —
the sword tier specials are unreachable until an input is bound.

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

**The detail and edit pools must land here too, and this is now urgent.**
Placing a torch, a staircase, a door or a window is a shipped feature, and
none of it survives a reload: `storage.c` references neither pool, so a
detail cell comes back as the plain crafting table that proxies it in the
terrain.  The edit journal is likewise in-memory only, which is tolerable
while saves still write the whole fixed footprint (edits inside it are
carried as blocks) and stops being tolerable the moment diff saves replace
that.  Both pools are flat arrays of fixed-size POD records with an `active`
flag — a version bump that appends them is mechanical; the ordering
constraint is simply that they load *before* the first column decorates.

## Constraints to respect

- **The audio build does not currently fit** — it overruns NuSystem's audio
  heap by ~20 KiB, so it needs roughly 384 KiB more than the non-audio build
  has to spare (`tools/check_ram.py` fails the build on overrun). NuSystem
  pins framebuffers and the audio heap at fixed absolute addresses; the link
  must stay below them. A pooled (indirected) block window or post-diff-save
  trimming is what buys it back. Until then, do not ship audio.
- Never busy-wait on the graphics thread; gate on `pendingGfx` instead.
- Always `gDPPipeSync` before reconfiguring the RDP.
- **Do not add an unbounded pool walk to anything `blockAt` or `blockGet` can
  reach.** See the pool-scan fault above: the mesher's inner loop turns a
  384-iteration scan into hundreds of millions per world build.

## Known loose end

`diag_arena_used` reported **0 while terrain was visibly rendering**, twice.
The published arena and the arena the column pointers refer to appeared to
diverge. The diagnostics have since been removed and the console is stable, so
this may have been an instrumentation artefact — but it was never explained. If
arena handoff misbehaves during task 5, start here.
