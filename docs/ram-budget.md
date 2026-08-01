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
audio, controllers, cartridge). Also ~22 KiB of libultra's remote debug monitor
and thread profiler, in a **release** build — welded to the exception handler by
this SDK's libultra and not removable from here; see *What looks wasteful but
isn't*.

### Everything else — ~123 KiB

Animation matrices for mobs, the player model, dropped items and trees; the
tree list; entity state; scratch buffers for the mesher. Nothing here is over
12 KiB and nothing is obviously wrong.

And outside BSS, **code and read-only data is 395 KiB**, of which 35 KiB is
graphics microcode (see below) and only 8 KiB is textures — the texture atlas is
not a memory problem.

## What we should absolutely do

All three were built and run before being written down here. See *Verified how*
at the end of this section for what that test did and did not cover.

### 1. Stop linking five microcodes we never run — 29 KiB, both builds

"Microcode" is the program the RSP runs. The SDK ships several variants for
different tradeoffs, and the spec files link **six** of them.

This is a leftover from a change that was worth making. The world, the targeting
wireframe and the HUD used to be three separate RSP tasks on three microcodes —
F3DEX for the world, L3DEX for lines, S2DEX for sprites — and commit `7f48b6c`
collapsed them into one F3DEX2 task. That was the right call and it stays. What
it left behind is five microcodes with no caller: `src/graphics.c` now holds the
only `nuGfxTaskStart` in the tree and always passes `NU_GFX_UCODE_F3DEX`, index
0. `nuGfxInitEX2`'s own one-off RDP-state task uses the same index.

They cannot simply be dropped from `spec`, because `nuGfxInitEX2` builds a
static table naming all six and hands it to `nuGfxSetUcode`. But that table is
only ever *indexed* — an entry is dereferenced by a task that asks for it, and
nothing asks for 1–5. So ten one-word stubs (`src/ucode_stubs.c`) keep the table
resolving while the real bodies leave the ROM.

**Measured:** `make` went from 35 KiB free to 64 KiB, and the headroom warning
stopped. The five stubs are 8 bytes each; `gspF3DEX2_fifoTextStart` is still its
full 5,008 bytes.

### 2. Supply our own RDP FIFO — 64 KiB, both builds

*This was originally filed under "could consider", on the assumption it needed
an SDK rebuild. It does not.*

`nuRDPOutputBuf` is the ring the RSP writes RDP commands into and the RDP
drains, and NuSystem sizes it at 128 KiB. It turns out to be the **only** symbol
in its archive member (`nurdpoutput.o` inside `libnusys.a`), so defining it in
game code resolves nusys's reference against ours and the member is never pulled
in. No SDK rebuild, no patched container.

One trap, and it is the dangerous kind: `nuGfxInit` calls
`nuGfxSetUcodeFifo(nuRDPOutputBuf, NU_GFX_RDP_OUTPUTBUFF_SIZE)` — it passes the
SDK's compile-time constant, not `sizeof`. A smaller array on its own would
leave the RSP believing it still had 128 KiB to write into, which is a buffer
overrun into whatever follows. `initGraphics` must re-register the real size
immediately after `nuGfxInit` (`mine64SetRDPFifo` in `src/rdp_fifo.c`).

With the fifo microcode a short FIFO costs throughput, not correctness: the RSP
stalls until the RDP catches up. That is the real question here — Mine64 has no
frame rate to spare, and **the emulator cannot answer it**, because HLE graphics
plugins do not model FIFO pressure. Watch `W` and `B` on hardware before and
after.

**Measured:** `nuRDPOutputBuf` is 65,536 bytes in the linked image, down from
131,072. `make` reached 128 KiB free.

### 3. Stop reserving 320 KiB of audio heap we don't use — ~224 KiB, audio build

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

**Measured:** with a 96 KiB heap, and with items 1 and 2 also in, `make audio`
links and passes the guard for the first time — but with only **2 KiB free**.
That is not shippable margin. Audio needs one more source of 30–60 KiB, or a
heap sized from a real `nuAuHeapGetUsed()` reading rather than from the SDK
formula, before it can be on by default.

### Verified how

All three changes were applied together in a throwaway worktree at the same
commit and built in `mine64-nusys-build:local`, then both ROMs were run through
`tools/emu/run.sh tools/emu/scripts/gui-tour.txt`. Title, world select, world
naming, HUD, inventory, crafting, walking and camera all render correctly on
both.

The baseline and patched screenshots differ, and that is expected: the world
seed is `(u32) osGetTime()` at the moment the menu opens (`src/menu.c`), so
changing the code layout changes the cycle count at that instant and therefore
the world. Re-running the *same* ROM is byte-identical, which is what makes that
inference safe.

**What the emulator did not test:** FIFO pressure (HLE plugins do not model it),
audio output, and RDP timing generally. Items 1 and 3 are structural and the
emulator run is good evidence. Item 2 is a performance change and only hardware
can sign it off — check `W`/`B` before and after.

## What we could consider

None of these are free. They cost either performance or a chunk of work.

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

**libultra's debug monitor cannot be dropped, and is not the debugger we
want anyway — ~22 KiB.** `rmonIOStack` (16 KiB), `__osThprofHeap` and `thprof`
are libultra's remote debug monitor and thread profiler. They are in a release
build linked against plain `-lultra`, which looks like a mistake.

It is not something the game can switch off. In this SDK's libultra,
`threadprofile.o` is pulled in by **`exceptasm.o`** (the exception handler) and
**`createthread.o`** (`osCreateThread`), and `rmonsio.o` — which drags in the
rest of the rmon cluster including that 16 KiB stack — is pulled in by
`exceptasm.o` as well. Both of those are on every link. Making it optional means
rebuilding libultra without the hooks, which is a lot of risk for 22 KiB.

It is also worth being clear about what it *is*: rmon is Nintendo's host-side
remote debugger, driven over the RDB/SI cable from a development host. It is not
reachable over SummerCart64 and it is not what captures Mine64's freezes — the
freeze-forensics rig does that, it is Mine64's own code, and it is already
toggleable (**Z + D-pad Up**, plus the watchdog phase square). There is no
capability here worth putting behind a flag; there is just 22 KiB the SDK will
not give back.

**The Z buffer gap is fully used.** The program starts at `0x80025C00`, which is
exactly where the Z buffer ends. Nothing is wasted between them.

**Textures are 8 KiB.** Not a factor.

## Where this leaves each build

Measured, not projected — every column is a real build in the container.

| | today | + microcodes | + microcodes, FIFO, heap |
|---|---|---|---|
| `make` | 35 KiB free ⚠ | 64 KiB free ✓ | **128 KiB free** ✓ |
| `make audio` | 310 KiB over ✗ | 281 KiB over ✗ | **2 KiB free** ⚠ |

The default build goes from tripping the headroom warning to having four times
the margin the guard asks for. The audio build stops failing, but 2 KiB is not
margin — treat it as proof the path exists, not as a shipping configuration.

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
