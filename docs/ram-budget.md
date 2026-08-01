# RAM budget

Where Mine64's 4 MiB goes, in plain terms, and what can be got back.

Measured by parsing the linked ELFs (`mine64.out`, `mine64-audio.out`) directly
— section headers and symbol tables, no toolchain needed. Figures are from
2026-08-01, at `d953a71`, and the treemap in *Visualising it* comes out of the
same pass: `tools/ram_report.py` prints every number quoted below and rewrites
the treemap's data from the same two files. Re-measure after anything large
moves — one command, both builds:

```sh
python3 tools/ram_report.py
```

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
| Audio heap | 70 KiB | *Audio builds only.* Working space for the sound engine. The SDK's default is 320 KiB; `src/audio.c` sizes it, from a hardware reading. |

NuSystem pins these at fixed addresses at the **top** of RDRAM and does not
allocate them from the linked program. The program is laid out from the bottom
up. So the two grow toward each other, and the only question that matters is
whether they meet.

**Nothing at link time notices when they do.** The build succeeds; the game
boots; on real hardware the picture or the sound quietly corrupts, because two
things are writing the same bytes. `tools/check_ram.py` runs after every link
precisely because the linker will not tell you.

Current state, at `d953a71`:

- `make` — the program ends at `0x803720B0`, **117 KiB** (120,656 bytes) below
  the framebuffers. Comfortable: the guard warns under 64 KiB.
- `make audio` — the image ends at `0x80379950` and the heap begins at
  `0x8037E000`: **17 KiB free** (18,096 bytes). It passes, and still trips the
  warning, which is correct — see *Where this leaves each build*.

The audio figure is newer than it looks. Two commits ago, at `cc5dd2d`, that
build was 208 bytes *over*; sizing the heap from a hardware reading rather than
the SDK's formula (`5aee62a`) returned 18 KiB and is what put it back under.
The story is in *An audio heap sized for this game* and *What was spent since
the last measurement*.

One thing worth knowing about the failure mode, since the audio build has now
hit it twice: when the guard fails, `make audio` exits non-zero, but
`build/mine64-audio.n64` has already been written and masked by then, and the
Makefile has no `.DELETE_ON_ERROR`, so **the bad ROM is left sitting on disk**.
Do not deploy it because the file is there. It is the one the guard refused.

## What every big piece actually does

Static allocation (BSS) is 2.91 MiB — about three quarters of the machine, and
the nine symbols below hold 92.6% of it. In rough plain English, with the symbol
name so you can find it in the source:

### `mesh_arena` — 1,152 KiB · "the shapes to draw"

**The single biggest thing in the game**, at 38.6% of BSS. The world is stored as
block IDs, but
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

### `nuRDPOutputBuf` — 64 KiB · "the conveyor belt"

The N64 has two graphics processors that work in a chain: the RSP transforms
geometry, the RDP fills pixels. This is the belt between them — the RSP writes
commands in, the RDP consumes them. It was 128 KiB, Nintendo's default; Mine64
now supplies its own at half that (`src/rdp_fifo.c`).

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

### Thread stacks and debug — 92 KiB of stacks, 21.5 KiB of debug

Working memory for the handful of threads NuSystem and libultra run (graphics,
audio, controllers, cartridge) is 92 KiB across fifteen stacks — but 16 KiB of
that is `rmonIOStack`, which belongs to the next number. Thread stacks the game
or NuSystem actually schedule against come to 76 KiB.

The other 21.5 KiB is libultra's remote debug monitor and thread profiler, in a
**release** build — welded to the exception handler by this SDK's libultra and
not removable from here; see *What looks wasteful but isn't*.

### Everything else — ~122 KiB

Animation matrices for mobs, the player model, dropped items and trees; the
tree list; entity state; scratch buffers for the mesher. Nothing here is over
12.2 KiB and nothing is obviously wrong.

And outside BSS, **code and read-only data is 393 KiB** — 318 KiB of game code,
61 KiB of read-only data, 6 KiB of microcode and 7 KiB of alignment padding.
Only 8.4 KiB of the read-only data is textures — the atlas is not a memory
problem — and the microcode is the one still linked, down from 35 KiB across
six. The audio build adds 28 KiB on top: 23 KiB of code, 4 KiB of the audio
microcode, and 1.4 KiB of read-only data.

## What was reclaimed

Three things the game was paying for at the SDK's default size rather than its
own. All three are in, and each was built and run before being written down
here — see *Verified how* at the end of the section for what that test did and
did not cover.

### 1. Five microcodes that never ran — 29 KiB, both builds · `5f8979d`

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

### 2. Our own RDP FIFO — 64 KiB, both builds · `d972dcb`

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

### 3. An audio heap sized for this game — 250 KiB, audio build · `d972dcb`, `823fdfe`, `5aee62a`

This is the whole reason `make audio` was broken, and — twice now — the thing
that unbroke it.

The 320 KiB audio heap is **not** a NuSystem reservation we have to live with —
`src/audio.c` passes the address and the size to `nuAuMgrInit` itself. 320 KiB
(`NU_AU_HEAP_SIZE`) is just the SDK's default, sized for a game running a full
MIDI sequence player.

Mine64 asks for 4 voices, 64 parameter updates, 32 × 1 KiB streaming buffers and
a 2048-entry command list. Running the SDK's own `NU_AU_HEAP_MIN_SIZE` formula
over those numbers gives roughly **73 KiB**. The reserve went in at 96 KiB,
was tightened to **88 KiB** in `823fdfe`, and is now **70 KiB** — and only the
last of those three is worth anything, because only the last one is a
measurement.

96 and 88 were both the formula plus a margin, and a margin on an estimate is
just a larger estimate. The procedure the earlier revisions of this document
kept prescribing was:

1. Print `nuAuHeapGetUsed()` on the diagnostic HUD, next to the `A` row, and
   read it on hardware. That is the real number, not an estimate.
2. Set the heap size from that, with headroom.
3. Move the heap address up to match — it has to stay directly beneath the
   framebuffers.
4. Update `AUDIO_HEAP_SIZE` in `tools/check_ram.py` so the guard checks the new
   ceiling.

**That reading has now been taken. The `U` row peaks at 64 KiB in game.**
`MINE64_AU_HEAP_SIZE` is 70 KiB (`0x11800`, base `0x8037E000`), so the reserve
is 6 KiB over a measurement instead of ~15 KiB over a guess — a smaller number
and a better founded one. It returned 18 KiB, which is what took `make audio`
from 208 bytes over to 17 KiB free.

**Why one reading settles it.** `alHeap` never frees, so `nuAuHeapGetUsed()` is
monotonic and any reading is a high-water mark. More than that, every
allocation of consequence happens in `initAudio`: `nuAuMgrInit` takes the
voices, the 32 × 1 KiB DMA buffers and the command list, and
`nuAuSndPlayerInit` takes `maxSounds` worth of sound state. `alSndpAllocate` at
play time hands back one of those already-allocated slots rather than growing
the heap. The peak is therefore reached before the first note plays, which is
why it does not matter which sounds happened to fire during the reading.

**What would invalidate it.** `maxVVoices`, `maxPVoices`, `maxUpdates`,
`nuAuDmaBufNum`, `nuAuDmaBufSize`, `nuAuAcmdLen`, `maxSounds`. Those are the
inputs to the number; touching any of them means re-reading `U` (audio builds
only, **Z + D-pad Up**) rather than trusting the 70. An undersized heap still
fails only on hardware and only as silence or corruption — no emulator
reproduces it — so this is not a thing to adjust by reasoning.

**Measured:** 96 KiB was the first heap that let `make audio` link at all, with
2 KiB free. 88 KiB bought 8 KiB more. 70 KiB, from the reading, bought 18 KiB
more and is where the build's first real margin came from.

### 4. One matrix per part instead of two — 24 KiB, both builds

Every model in the game loaded a translation and then multiplied a rotation
onto it: two `Mtx` of RDRAM per part, doubled again for the RSP's double
buffer, and two `gSPMatrix` for the RSP to walk.

It never needed to be two. Vertices go through row-vector, so the pair composes
to `rotation * translation` -- and a rotation's bottom row is (0, 0, 0, 1), so
the product is *the rotation with its bottom row replaced by the translation*.
There is no multiply to do. `modelMatrix` in `graphics.c` builds the rotation in
float, writes three numbers into the bottom row and converts once, which is less
CPU work than the two `gu` calls it replaces, not more.

It holds for any linear part whose bottom row is (0, 0, 0, 1) -- a rotation, a
scale, or the two already combined -- which is every matrix this game pairs with
a translation. The camera is untouched: it lives on the projection stack, and
the modelview is always loaded fresh, which is what makes collapsing the pair
safe.

| | before | after |
|---|---|---|
| mobs | 13,312 | 7,168 |
| people | 12,288 | 6,144 |
| details | 6,144 | 3,072 |
| creatures | 5,632 | 2,816 |
| dropped items | 4,096 | 2,048 |
| falling trees | 7,168 | 3,584 |
| first person | 1,536 | 1,024 |

An animal's orientations are shared between its parts and its anchors are not,
so those two are held in CPU scratch and folded at the moment a part is drawn.
That scratch is not RSP-visible, so it needs neither the double buffer nor a
slot per animal -- which is why the mob saving is larger than a halving.

The RSP also walks one matrix command per part instead of two. That is a
frame-time win on the pass that owns the 20 fps ceiling, and like every other
frame-time claim here it wants hardware to confirm.

**Measured:** `make` went from 114 KiB free to 138 KiB, and `make audio` from
12 KiB over to 12 KiB free.

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

## What was spent, and what paid for it

### One body for every person — ~11 KiB, both builds

Players, 64MON's trainers and the villagers used to be two model
representations and one draw path each; they are now one body out of one matrix
pool, and there are villagers where there were none. About half of that is the
pool and half is the code that did not exist before.

The geometry itself became free rather than costing more. The boxes carry
shading only and the garment colour arrives as the primitive colour, so eight
people on screen share one set of vertices -- where the creature path writes
eight vertices per box per slot per frame. `HUMANOID_NPC_SLOTS` is the dial for
a busier world; each one is a kilobyte.

That spending is what turned up the entry below, which more than paid for it.

## What was spent since the last measurement

The figures above were taken at `abd4300`. Rebuilding that commit reproduces
them exactly — 138 KiB free and 12 KiB free — so the drift to `cc5dd2d` is
entirely the five commits in between, and none of them is a memory change:

| | `abd4300` | `cc5dd2d` | delta |
|---|---|---|---|
| code + read-only data | 382,960 | 402,432 | **+19,472 B** |
| BSS | 3,054,320 | 3,055,664 | +1,344 B |
| `make` headroom | 138 KiB | 117 KiB | −21 KiB |
| `make audio` headroom | 12 KiB free | 208 B **over** | −20 KiB, +8 back from the heap |

(`d953a71`, the moon-phase monster counts, landed after that and cost 128 bytes
of code and no BSS at all — too small to move either headroom figure. The
current numbers at the top of this document are from that commit.)

**Nineteen KiB of it is code.** Only seven BSS symbols changed at all, and
`special_flash_verts` (1,152 B, the sword specials) is the only one over 100
bytes. The growth is in functions:

| symbol | before | after | delta |
|---|---|---|---|
| `updateMobs` | 7,596 | 9,864 | +2,268 |
| `drawWorld` | 14,692 | 16,832 | +2,140 |
| `detectCollision` | — | 1,328 | +1,328 |
| `drawItemIcon` | 3,252 | 4,532 | +1,280 |
| `drawHealth` | — | 904 | +904 |
| `drawDetailsForPlayer` | 2,076 | 2,972 | +896 |
| `drawCheckMarks` | — | 668 | +668 |
| `quickMoveInventoryStack` | — | 556 | +556 |
| `drawDeathScreen` | — | 464 | +464 |

That is 120 functions moving for a net +19,044 B, and it reads exactly like the
commit log: a death screen, fences and taming and beds, party sleep, compass
and biome-gated spawns, sword specials. Ordinary feature work, no single
offender.

The lesson the audio build keeps teaching is that **it is the build to check,
and 12 KiB of headroom is not headroom.** Five ordinary gameplay commits came
to ~20 KiB of code. Anything under about 32 KiB free on `make audio` should be
read as "one feature away from broken".

The way back was item 3 above, and it is worth noting *why* that was the right
lever rather than the two 64 KiB structural savings further down. Those cost
frame time, on a game that has none to spare. Sizing the heap cost a hardware
session and no frame time at all, because the 18 KiB was never doing anything —
it was the difference between a formula's guess and what the allocator actually
takes. The cheapest headroom is usually a number nobody has measured yet, not a
structure nobody has rewritten yet.

That lever is now spent. The heap is 6 KiB over a reading; there is nothing
left in it. The next 17 KiB of features puts `make audio` back where it was,
and the options at that point are the frame-time ones.

## What is still on the table

Neither of these is free — both trade frame time for memory, on a game that has
none to spare. The `U` reading was the cheap option and it has been taken, so
these are what is left: they are no longer "listed because the analysis found
them" but genuinely next, whenever `make audio` next runs out.

There is also a correctness debt in the same neighbourhood as the matrix work,
which is not a saving but will cost one. `detail_*`, `creature_*` and
`humanoid_*` are indexed by *per-viewport render slot*, and every viewport
writes into one display list that the RSP walks after the whole frame is built
— so in split-screen the second viewport overwrites the matrices the first
viewport's commands still point at. `mob_matrix` is indexed by the animal
itself and does not have this problem. Fixing the other three means widening
them to per-viewport ranges, which is about 13 KiB of the 24 the collapse just
returned.

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

| | before | + microcodes | + fifo & heap | + matrix | `cc5dd2d` | now (`d953a71`) |
|---|---|---|---|---|---|---|
| `make` | 35 KiB free ⚠ | 64 KiB free ✓ | 128 KiB free ✓ | 138 KiB free ✓ | 117 KiB free ✓ | **117 KiB free** ✓ |
| `make audio` | 310 KiB over ✗ | 285 KiB over ✗ | 2 KiB free ⚠ | 12 KiB free ⚠ | 208 B over ✗ | **17 KiB free** ⚠ |

The default build went from tripping the headroom warning to nearly twice the
margin the guard asks for, and it has absorbed 21 KiB of ordinary feature
growth on top of that without complaint. It is not the build to worry about.

The audio build is. It reached 2 KiB free, drifted to zero on ordinary feature
growth, went 12 KiB over when the people work landed, came back to 12 KiB free
on the matrix collapse that work turned up, gained 8 KiB when the reserve went
to 88 KiB, went 208 bytes over on five gameplay commits, and is at 17 KiB free
now that the heap is sized from a hardware reading instead of a formula.

Seventeen KiB is the most real margin it has ever had — the earlier figures
were headroom against a heap reserve that was itself padded guesswork, and this
one is not. It is still under the guard's warning threshold, and the warning
should stay. Five gameplay commits cost 19 KiB. This buys about one more round
of features, and the levers left after that all cost frame time.

## How to measure it yourself

- **After every link** — `tools/check_ram.py` runs from the Makefile. It fails
  the build on overrun and warns under 64 KiB of headroom. Pass `--audio` for
  audio builds. It reads the ELF's section headers, so it needs no toolchain.
- **The whole breakdown** — `tools/ram_report.py`, with both ELFs built. It
  prints the per-group table this document quotes and rewrites the treemap's
  data in place. `--print` reports without touching the file. It parses the ELF
  by hand too, so it also needs no toolchain — but it does need both
  `mine64.out` and `mine64-audio.out`. `make audio` fails the guard *after*
  linking, so the ELF is there to measure even when the build is red.
- **On hardware, mesh arena** — the `A` row of the diagnostic HUD is
  `mesh_arena` free percentage.
- **On hardware, frame lists** — the `M` row is the peak command count against
  the 6,656 limit, and `V` counts frames that had to shed terrain or were
  dropped.
- **On hardware, audio heap** — the `U` row is `nuAuHeapGetUsed()` in KiB,
  against the 70 KiB `MINE64_AU_HEAP_SIZE` reserves. Audio builds only. It
  reads 64, and the reserve is sized from that, so `U` above about 66 means
  something new is allocating and the reserve has to move. Re-read it after
  changing any voice, buffer or command-list count.
- **Per-symbol breakdown** — `mips-n64-nm -S --size-sort mine64.out | tail -40`
  inside the build container, or parse the ELF symbol table directly if you have
  no toolchain to hand.

## Visualising it

[ram-report.html](ram-report.html) is an interactive treemap of this data — one
box for the 4 MiB, divided by real byte counts, clickable down to individual
symbols, with a toggle between the two builds. It is a single self-contained
page: no build step, no server.

```sh
open docs/ram-report.html
```

This file is the canonical copy, and `tools/ram_report.py` regenerates its data
from the linked ELFs — the prose in it is hand-written, the numbers are not.
A snapshot was published to `claude.ai/code/artifact/4f72a0af` earlier in the
work; it has since drifted badly and still shows the pre-`5f8979d` figures, so
treat the repo copy as the truth.
