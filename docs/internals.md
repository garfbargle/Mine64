# Internals

Why the world works the way it does, and what breaks if you change it. Current
radii, budgets and sizes are all `#define`s — read them out of the source
rather than from here.

## The world is a function, not a map

Terrain is a pure function of `(x, z, world_seed, world_mods)`. The console
holds a patch of that function evaluated around the player, not a map. Walking
back re-evaluates the same coordinates and gets the same blocks, which is what
makes discarding terrain behind the player free.

Consequences worth keeping in mind:

* Anything derivable — biome, tree species, ore veins, where hamlets and ruins
  go — is derived on the spot and never stored. Adding a stored table for one
  of these gives up the property that makes eviction free.
* What the player did is *not* derivable, and lives in the **home store**: a
  dense second copy of a fixed save extent, independent of residency. Edits are
  there whether or not the column is currently loaded. Outside that extent,
  changes stand until the column is recycled and are then gone.

## The block window

Live terrain is a wrapping window of columns, nibble-packed. A column is the
full world height, split into vertical chunks; height is fixed, so this is 2D
streaming with no vertical paging.

* A column's slot is the low bits of its chunk coordinates. The window does not
  scroll — walking addresses different slots, and out-of-range columns are
  overwritten in place. **Block data is never moved.**
* Residency is folded into a bit of the key each slot stores, so the test is
  one load and one compare. `blockGet` is in the mesher's inner loop; keep it
  that cheap.
* The window is deliberately wider than the outermost ring. That slack cushions
  the wrap so a column arriving on one side cannot land on a slot the other
  side is still using.

## Rings and stages

Residency is concentric square rings (Chebyshev), one per stage: terrain →
structures → decoration/meshing, widest first. Each stage's ring is wider than
the next because **decoration writes across column boundaries** — a tree canopy
reaches into its neighbours — so a column may only advance when everything it
can write into is already claimed. Narrowing that gap causes trees to be cut
off at column edges.

Rings rather than view cones: walking exposes one new row at a time, while
turning around would invalidate a whole cone at once.

Underground carving is deferred to a much smaller radius; far columns generate
surface-only. This is safe only because caves never breach their roof, so the
surface is identical either way and the frontier is invisible.
`tools/gentest` verifies the two paths produce identical blocks — run it after
touching generation.

Each frame advances the stages **stage-major**: every column in reach is
structured before any column grows a tree. Advancing one column as far as it
can go instead spends the whole budget retrying the nearest column, which
cannot finish until neighbours that never get a turn catch up.

Within a stage the scan is nearest-first, biased ahead of the player by
smoothed movement, so the ring builds toward where you are running rather than
finishing the ground you are about to step on last. The bias moves ranking
only, never ring membership.

## The frame budget

Streaming shares the frame with physics, so it runs against a wall-clock
deadline and stops between columns; whatever is left happens next callback.

* **Most of the work is free.** It runs from graphics callbacks that found an
  RSP task still in flight — pure CPU work against the block window that
  touches nothing the RSP reads. Overlapping there is why walking doesn't pay
  for generation and rendering in series.
* **A hole under the player is an emergency.** Unbuilt or unmeshed terrain very
  close by raises the budget and trades a hitch for closing the gap. A merely
  pending LOD upgrade is *not* urgent — treating it as urgent made every chunk
  crossing hitch.
* Exactly one stage per frame may take a single step past the deadline, and the
  turn rotates. Giving every stage that exemption every frame lets them stack,
  and the worst walking frame pays an overrun for *every* stage at once.

## Seams

Meshing reads one block across each horizontal boundary to decide whether a
face is hidden, so a column compiled before its neighbours exist culls against
"not resident" and leaves a hole at the seam.

A column that finishes decorating re-meshes its **already-decorated** neighbours
and only those — a neighbour not yet compiled will read this column correctly
whenever it does get compiled. Marking all neighbours unconditionally re-meshes
most columns several times over as the ring advances, and every rebuild orphans
its predecessor in the arena.

## Eviction

A column leaving the terrain ring is released in one pass: home-store snapshot
flushed, tree and detail records returned to their pools, mesh block returned
to the arena, window key cleared. The record pools are small and fixed — a walk
that never gave trees back would exhaust the pool and quietly stop growing new
ones.

## Coordinates

Block coordinates are absolute `s32`, but the N64 `Mtx` is fixed point and
loses sub-block precision once you walk far enough out. A **render origin** is
subtracted at the handful of places a world position becomes a matrix, and
re-centred on the player periodically. The rebase rewrites every resident
column's matrices, so it runs only with no RSP task in flight.

An impossible player position is treated as corruption and skipped rather than
converted. A far-walk freeze was an FPU unimplemented-operation fault from
pushing exactly such a value through `guTranslate`; window keys are audited
every frame for the same reason.

## Outrunning the mesher

You can sprint faster than the mesher on a bad frame. Two things cover it:

* The fog band normally parks past the mesh ring where it costs nothing, and
  slides in the moment culling sees a frustum cell with no geometry behind it,
  so terrain appears out of haze rather than out of nothing.
* Movement into a column that is not yet decorated and meshed is cancelled
  **per axis**, so the player slides along the frontier instead of sticking to
  it. The stick keeps being read, so a held run resumes on its own. Movement
  *out* of an unready column is never blocked — otherwise a slot repaired under
  someone's feet would trap them.

## Home store ordering

The snapshot is copied back over a column **after** decoration, not before:
canopies write into neighbours, so an earlier restore would be overwritten by a
neighbour's leaves.

Anything that writes past its own column must bracket the pass with a flush of
dirty neighbours and a resync afterwards. Restoring without flushing first
pushes a stale snapshot over a column the player has been building in. This is
the fragile part of the system; treat changes to pass ordering with suspicion.

## Rendering

* **View distance outranks near detail.** Full-detail terrain is a small bubble;
  beyond it columns are a cheap surface shell with no T-junction refinement.
  Hysteresis between the promote and demote radii keeps a boundary column from
  re-meshing every step.
* **Columns compile to baked vertex buffers** — quads pre-translated into
  column-local space and batched under a single matrix, with no per-quad
  matrices. Per-quad matrix ops across the visible far columns measured, on
  hardware, as the bulk of the RSP's frame; that is why the compact format was
  abandoned.
* Faces are merged by a greedy scan per chunk. Greedy geometry is scratch, held
  only for the column being compiled. A merged quad's vertices are computed at
  bake time from its face and spans (`writeBakedQuadVertex` in `graphics.c`);
  the generated table that used to hold them was 24 KiB of image.
* **One mesh arena**, first-fit, one block per column. An incremental defrag
  slides a single block per frame into the lowest gap and patches that block's
  vertex addresses and per-texture start pointers. This is safe *only* because
  every arena mutation runs from the graphics callback with no task in flight
  and completes before the next frame is submitted.
* Columns are recompiled when a block changes, when they cross an LOD boundary,
  or when a newly arrived neighbour means the seam was compiled against terrain
  that had not streamed in yet.
* Off-camera columns are culled by omission from the main display list, under a
  visible-column cap and a hard command budget that sheds the most distant
  terrain rather than overrunning the buffer.
* Fog hugs the streaming frontier rather than sitting at a fixed distance —
  fixed placement was measurably expensive, because every column pays the
  two-cycle rate instead of only the ones that reach the band.
* **Torches, stairs, doors and windows** live in a sparse record pool, not in
  the packed terrain, which has no room for shape or state. Each occupies its
  cell with a crafting-table proxy, so a cell with no record is an ordinary
  crafting table and old saves are unaffected.
* Runtime terrain is nibble-packed, which is what makes the window affordable.
* The mob pool is bounded and covers every species, split between passive and
  hostile, so adding a species never becomes an unbounded AI or matrix cost.
  64MON's roamers come out of the same passive budget, and a battle is a pause
  inside the game rather than a screen of its own.
* Long frame hitches are clamped before physics, so generation or storage
  delays can't push players through terrain.

## Save format

Preserves hotbar, inventory, crafting, carried item, objectives, hunger, world
clock, trees, and the packed terrain of the save extent — plus the world seed
and mod mask.

Those last two matter more than they look: without them, everything past the
saved extent regenerated from whatever seed the session happened to be
carrying, so walking out of the saved region gave a different world every load.
Biomes made it obvious — a desert became a forest between sessions.

* New fields are appended into space the fixed-size header page already
  zero-fills, so older offsets stay put and one struct serves every version. An
  older file reads new fields as zero, which must resolve to the behaviour that
  version actually had.
* Player and world data are checksummed; writes go through a temporary file
  plus a backup so an interrupted cartridge write recovers the previous world.
* **A load must reset every pool it only partly refills.** The file carries the
  frozen 96 tree records, not the whole live pool, so `beginLoadGame` calls
  `initTrees` and `initHome` the way `beginWorldGeneration` does. Without it the
  records past the saved 96 were whatever RAM held — on a cold boot, bss, where
  `base_y` is 0 rather than `TREE_INACTIVE_Y`, so each read as a live tree at
  (0, 0) and `treesValid` failed on the duplicate root. The menu previews the
  highlighted slot as it comes up, so the first boot after the first save was
  enough to condemn it. Saving was never the broken half.
* A file that fails validation is renamed to `.bad`, not deleted. Freeing the
  final name is what the fallback world's first transactional rename needs;
  destroying the bytes was never part of it, and it is what turned the bug
  above into lost worlds rather than a bad load. One `.bad` is kept per slot.
* **Save paths must be legal 8.3 short names.** `ffconf.h` sets `FF_USE_LFN` to
  0, so FatFs rejects a longer basename or extension with `FR_INVALID_NAME`
  before touching the card. Several past naming schemes overran this and saving
  silently stopped working. The filename also encodes the world extent, so a
  save can't be misread with a different packed-world length.
* The card must be FAT16 or FAT32 — `FF_FS_EXFAT` is 0, and large cards are
  exFAT by default. When storage fails to come up the title screen names the
  failing step rather than blaming a missing cartridge.

## Known gaps

* **Only the fixed extent is persisted.** Saving works anywhere, but building
  outside the extent is transient by design — those columns regenerate from the
  seed and mod mask on the next visit. Per-chunk diff saves are the fix.
* **Details are not persisted.** Torches, stairs, doors and windows reload as
  the crafting tables that proxy them in the terrain.
* **Audio fits, with 115 KiB to spare.** It had gone 208 bytes over NuSystem's
  audio heap at `cc5dd2d`; sizing the reserve from a hardware reading of the
  `U` row (peak 64 KiB, reserve 70 KiB) brought it to 17 KiB, and the
  structure pass — extent-indexed staged tables, computed quad vertices, and
  pools resized to the bounds residency already enforces — took it to 115,
  clear of the headroom warning for the first time. Feature growth measured
  ~19 KiB per five gameplay commits, so this is several rounds of headroom,
  not a solved problem. See [RAM budget](ram-budget.md).
* **64MON teams are not saved.** `mon64SaveBlob` and `mon64LoadBlob` exist and
  nothing calls them, deliberately: the per-chunk diff format will move every
  offset in the file, and the party belongs in that version bump. See *Not
  wired yet* in [64MON](64mon.md).
* ~~**Sword tier specials are unreachable.**~~ Bound to **L + B** in the swing
  handler, with a charge bar between the health and food meters and a
  translucent plate for each move (`buildSpecialFlash`). The plate's dimensions
  are `SPECIAL_*` defines in `graphics.c` so `tools/preview/special.py` can
  frame them against the game's real FOV and eye height — which is how the
  first attempt's grey-floor cleave and out-of-frustum shockwave were caught.
