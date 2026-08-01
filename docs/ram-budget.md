# RAM budget

Where Mine64's 4 MiB goes, in plain terms, and what can be got back.

Measured from the linked ELFs (`mine64.out`, `mine64-audio.out`) at commit
`155204a`, re-checked against the 2026-08-01 09:21 rebuild. There is an
interactive treemap of the same data — see *Visualising it* at the bottom.

## The machine

A retail N64 has 4 MiB of RDRAM and no Expansion Pak, and Mine64 targets that.
Everything the game will ever have has to fit in those four megabytes at once:
there is no swapping, no virtual memory, and no allocator to fall back on. The
cartridge is a separate 1 MiB (2 MiB with audio) that the CPU cannot execute
from directly — ROM is not RAM, and a bigger ROM buys nothing here.

Three chunks are spoken for before a single line of game code loads:

| Region | Size | What it is |
|---|---|---|
| Z buffer | 150 KiB | Depth-per-pixel, so near things draw over far things. One screen's worth. |
| Framebuffers | 450 KiB | Three screens of pixels: one being shown, one being drawn, one spare. |
| Audio heap | 320 KiB | *Audio builds only.* Working space for the sound engine. |

NuSystem pins these at fixed addresses at the **top** of RDRAM and does not
allocate them from the linked program. The program is laid out from the bottom
up. So the two grow toward each other, and the only question that matters is
whether they meet.

**Nothing at link time notices when they do.** The build succeeds; the game
boots; on real hardware the picture or the sound quietly corrupts, because two
things are writing the same bytes. `tools/check_ram.py` runs after every link
precisely because the linker will not tell you.

Current state:

- `make` — the program ends **35.7 KiB** below the framebuffers. The check
  warns under 64 KiB, so this build already prints a warning.
- `make audio` — the program runs **310 KiB past** the audio heap. It fails the
  check and does not ship. This is not new; it predates the home store.

## What every big piece actually does

Static allocation (BSS) is 2.99 MiB — about three quarters of the machine. Nine
things are 97% of it. In rough plain English, with the symbol name so you can
find it in the source:

### `mesh_arena` — 1,152 KiB · "the shapes to draw"

**The single biggest thing in the game.** The world is stored as block IDs, but
the graphics chip cannot draw block IDs — it needs a list of triangles with
positions, colours and texture coordinates. Turning blocks into that list is
"meshing", it is expensive, and you absolutely do not want to redo it every
frame. So each column of the world gets meshed once and the result is parked
here until the column changes.

Think of it as a car park for pre-built scenery. Columns arrive and leave as you
walk, which leaves gaps, so there is a small defragmenter that slides one parked
block into the lowest free gap each frame — the arena heals over a few dozen
frames while you play, rather than stopping the world to tidy up.

This is the one number that **cannot** be sized by reading code. How full it
gets depends on what terrain you happen to be standing in. The `A` row on the
diagnostic HUD reports free percentage for exactly this reason. If `A` reads
near zero while terrain is missing at the edge of view, this is the number that
gave.

### `window_blocks` — 1,024 KiB · "the world you're standing in"

The actual block data: what is stone, what is air, what is a plank. Each block
is 4 bits (16 kinds), and a "column" is an 8×8 footprint the full 32 blocks
high, so one column is 1 KiB.

The world does not have edges, so this is not the world — it is a **sliding
window** of 32 × 32 columns around you. Walk far enough and a column you left
behind gets recycled to hold a column ahead of you. Coming back regenerates it
from your coordinates and the world seed, which is why an endless world is
affordable at all.

Why 32 × 32 and not something tighter: finding a column's slot has to be a bit
mask rather than a division, because that lookup sits in the innermost loop of
meshing. Bit masks need a power of two.

### `home_blocks` — 196 KiB · "everything you built"

A permanent copy of the saveable region (112 × 32 × 112 blocks). The window
above is a cache — walk away and your changes would be recycled along with the
column. Keeping a second, dense copy of the save area means every cell of it can
hold whatever you put there, with no ceiling on how much you can build and no
list of exceptions to maintain.

It is also what a save writes. Before this existed, saving needed the whole area
loaded at once, so walking away from spawn made saving refuse.

### `nuRDPOutputBuf` — 128 KiB · "the SDK's conveyor belt"

The N64 has two graphics processors that work in a chain: the RSP transforms
geometry, the RDP fills pixels. This is the belt between them — the RSP writes
commands in, the RDP consumes them. Its size is Nintendo's default, compiled
into the SDK library, not a number this game ever chose.

### `frame_display_lists` — 104 KiB · "this frame's instructions"

The actual list of commands sent to the graphics chip for one frame: the world,
the block you are pointing at, the HUD, the menus. Two of them, so one can be
built while the other is being drawn. The `M` row on the HUD reports the
high-water mark, so we know how much of it is really used.

### `column_starts` + `staged_starts` — 128 KiB · "the index"

For every column slot, and for every one of the 16 textures, a pointer saying
"the triangles using this texture start here". Drawing groups by texture because
switching textures is expensive, so this index is what lets a frame do one pass
per texture over the whole visible world.

There are two copies because a whole-world rebuild (new world, title screen) has
to emit a complete replacement while the old world keeps rendering.

### `c_models` — 64 KiB · "where each column sits"

One 64-byte matrix per column slot, holding nothing but "this column is at
X, Z". The meshed triangles are stored relative to their own column, so
something has to say where that column is in the world.

### `mesh_blocks` — 32 KiB · "the car park ledger"

Bookkeeping for `mesh_arena`: which column owns which stretch of it, how long,
and where its triangles end and its commands begin. Sized for every live column
plus every staged replacement.

### Thread stacks and debug — 87 KiB

Working memory for the handful of threads NuSystem and libultra run (graphics,
audio, controllers, cartridge). Also, unexpectedly, libultra's remote debug
monitor and thread profiler — in a **release** build.

### Everything else — ~123 KiB

Animation matrices for mobs, the player model, dropped items and trees; the
tree list; entity state; scratch buffers for the mesher. Nothing here is over
12 KiB and nothing is obviously wrong.

And outside BSS, **code and read-only data is 395 KiB**, of which 35 KiB is
graphics microcode (see below) and only 8 KiB is textures — the texture atlas is
not a memory problem.

## What we should absolutely do

Both of these are things the game is paying for and getting nothing back.

### 1. Stop linking five microcodes we never run — 29 KiB, both builds

"Microcode" is the program the RSP runs. The SDK ships several variants for
different tradeoffs, and the spec files link **six** of them. `src/graphics.c`
holds the only `nuGfxTaskStart` call in the entire tree, and it always asks for
F3DEX2. The other five sit in RAM for the whole run and never execute.

The catch: they cannot simply be dropped from `spec`, because NuSystem's
`nuGfxInit_ex2` holds a static table naming all six. Removing them means adding
ten one-word stub symbols in a game source file so the table still links. The
entries for unused slots are never dereferenced.

This is the cheapest 29 KiB in the project, it applies to both builds, and it
fails loudly at link time if it is wrong. On its own it takes `make` from 35.7
KiB of headroom to about 65 KiB, which clears the warning.

### 2. Stop reserving 320 KiB of audio heap we don't use — ~224 KiB, audio build

This is the whole reason `make audio` is broken.

The 320 KiB audio heap is **not** a NuSystem reservation we have to live with —
`src/audio.c` passes the address and the size to `nuAuMgrInit` itself. 320 KiB
(`NU_AU_HEAP_SIZE`) is just the SDK's default, sized for a game running a full
MIDI sequence player.

Mine64 asks for 4 voices, 64 parameter updates, 32 × 1 KiB streaming buffers and
a 2048-entry command list. Running the SDK's own `NU_AU_HEAP_MIN_SIZE` formula
over those numbers gives roughly **73 KiB**. A 96 KiB heap would leave a third
of it spare.

Do it in this order:

1. Print `nuAuHeapGetUsed()` on the diagnostic HUD, next to the `A` row, and
   read it on hardware. That is the real number, not an estimate.
2. Set the heap size from that, with headroom.
3. Move the heap address up to match — it has to stay directly beneath the
   framebuffers.
4. Update `AUDIO_HEAP_SIZE` in `tools/check_ram.py` so the guard checks the new
   ceiling.

Items 1 and 2 together leave the audio build about 57 KiB over. It needs one
more thing from the list below to actually ship.

## What we could consider

None of these are free. They cost either performance or a chunk of work.

### Halve the RDP conveyor belt — 64 KiB, both builds

`nuRDPOutputBuf` is 128 KiB because the SDK says so. The size is compiled into
`nusys.o`, so calling `nuGfxSetUcodeFifo` with a smaller span reclaims nothing —
the array is still linked. Getting it back means rebuilding libnusys inside the
project's Docker image with a smaller `NU_GFX_RDP_OUTPUTBUFF_SIZE`.

The good news: with the fifo microcode, a short belt costs throughput, not
correctness — the RSP waits when it fills up rather than corrupting anything. So
this is a frame-rate question, and Mine64 does not have frame rate to spare.
Worth it if the audio build needs the last 57 KiB and nothing else can find it.

### Find out why rmon is in a release build — ~22 KiB, both builds

`rmonIOStack` (16 KiB), `rmonRdbReadBuf`, `__osThprofHeap` and friends are
libultra's remote debug monitor and thread profiler. They are present in
`mine64.out`, which links plain `-lultra`, not `-lultra_d`. On the original SDK
these belong to the debug library only.

This is unverified — it may be that this SDK's libultra always pulls them in via
the boot path, in which case there is nothing to do without rebuilding it.
Cheap to check, and if it is a choice rather than a requirement, it is free.
Start at the libultra build in `docker/N64SDK.Dockerfile`.

### Narrow the index — up to 64 KiB, both builds

`column_starts` and `staged_starts` store a full 32-bit pointer per texture per
slot, and most of those point at the same shared "nothing here" list — a typical
column uses five or six of the sixteen textures. Because a column's mesh is one
contiguous block, a slot only really needs its block number plus a 16-bit offset
within that block, which halves both tables.

The cost is an extra indirection per texture per column in the draw loop, and
that loop is already the bulk of a standing frame. Measure before committing.

### Delete `c_models` — 64 KiB, both builds

Folding each column's position into its triangles at mesh time would remove the
array *and* one matrix operation per column per frame. But it welds every meshed
column to the current render origin, so an origin rebase would have to re-mesh
the world instead of just re-pointing it. Probably not worth 64 KiB. Listed for
completeness.

## What looks wasteful but isn't

**About 480 KiB of the window is unreachable.** The block window is 32 × 32
slots, but `STREAM_TERRAIN_RADIUS` is 12, which spans 25 columns. So at most 625
of the 1,024 slots can ever be live: 399 KiB of `window_blocks`, plus roughly 78
KiB spread across the per-slot tables, that nothing can occupy.

It cannot be reclaimed. 25 columns of terrain needs 26 in the window, because
meshing reads one column past what it is compiling, and the next power of two is
32 — and the power of two is required for the mask lookup.

What it *is* is already-purchased capacity. Raising the stream radius toward 15
would use it, and the price is frame time rather than memory. `world.h` records
that radius 12 was measurably choppy on hardware and 10 was the compromise, so
this headroom is bounded by the frame budget, not by RDRAM.

**The Z buffer gap is fully used.** The program starts at `0x80025C00`, which is
exactly where the Z buffer ends. Nothing is wasted between them.

**Textures are 8 KiB.** Not a factor.

## Where this leaves each build

| | now | after must-do | after must-do + FIFO |
|---|---|---|---|
| `make` | 35.7 KiB free ⚠ | ~65 KiB free | ~129 KiB free |
| `make audio` | 310 KiB over ✗ | ~57 KiB over ✗ | ~7 KiB free |

## How to measure it yourself

- **After every link** — `tools/check_ram.py` runs from the Makefile. It fails
  the build on overrun and warns under 64 KiB of headroom. Pass `--audio` for
  audio builds. It reads the ELF's section headers, so it needs no toolchain.
- **On hardware, mesh arena** — the `A` row of the diagnostic HUD is
  `mesh_arena` free percentage.
- **On hardware, frame lists** — the `M` row is the peak command count against
  the 6,656 limit, and `V` counts frames that had to shed terrain or were
  dropped.
- **On hardware, audio heap** — `nuAuHeapGetUsed()` exists but is not wired to
  the HUD yet. Wiring it is step one of the audio fix.
- **Per-symbol breakdown** — `mips-n64-nm -S --size-sort mine64.out | tail -40`
  inside the build container, or parse the ELF symbol table directly if you have
  no toolchain to hand.

## Visualising it

There is an interactive treemap of this data — one box for the 4 MiB, divided by
real byte counts, clickable down to individual symbols, with a toggle between
the two builds:

[ram-report.html](ram-report.html) — a single self-contained page, no build step
and no server. Open it straight from the working tree:

```sh
open docs/ram-report.html
```

The same page is published at
<https://claude.ai/code/artifact/4f72a0af-89df-4341-9874-ec7c622f267f>.
