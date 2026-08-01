# Mine64 for Nintendo 64

Mine64 is a compact block-building game for original Nintendo 64 hardware.
The world is no longer a fixed map: terrain streams around the player in a
residency window, so it can be walked in any direction without end, on an
unmodified 4 MiB console.

![Mine64 box art](mine64.png)

![Mine64 in-game screenshot](game.png)

## The streaming world

* **Walkably unbounded.** Terrain is generated on demand from
  `(x, z, world_seed)` into a 32×32-column residency window, and released
  again behind the player. Because generation is a pure function, an
  unmodified column costs nothing to evict: it comes back identical.
* **View distance ~80 blocks**, twice the original fixed world. The near
  disc renders from baked vertex buffers with no per-quad matrices; the far
  ring is a surface shell of roughly 20–35 quads per column instead of 170.
* **One 1 MiB mesh arena**, managed as first-fit blocks with an incremental
  relocation defrag that slides a single block per frame. This replaced a
  double-buffered pair plus a stop-the-world compaction pass, and the
  megabyte it returned is what paid for the wider window.
* **Distance fog matched to the sky** hides the streaming frontier, so
  terrain arrives behind haze rather than in visible rows of bare columns.
* **Deferred underground.** Far columns generate surface-only; the cave and
  ore carve runs within five chunks of the player. Caves never breach their
  three-block roof, so the surface is identical either way and the frontier
  is invisible — verified byte-identical by `tools/gentest`.
* **Landmarks worth walking to.** Hamlets (cottages, a well, connecting
  paths) and ruins are placed on a coarse 64×64 macrocell grid, hashed from
  the coordinate like everything else, so they need no stored map.
* **Torches, stairs, doors, and windows** live in a small sparse record pool
  rather than in the packed terrain, which has exactly sixteen block IDs and
  no room for shape or state. Player edits outside the original footprint
  are likewise kept as a sparse journal, so an effectively infinite world
  does not imply effectively infinite RDRAM.
* **Survival pressure**: a hunger bar that movement drains and food refills,
  zombies and spiders alongside the original night slimes, and a mob pool
  that retreats at sunrise.

## v0.4 highlights

* Coal and iron form compact underground veins above an unbreakable bedrock
  floor. Mossy waystones provide sparse landmarks, placed by a per-column
  probability so they mean something in a world with no fixed size.
* Wooden, stone, and iron swords, pickaxes, and axes have distinct held,
  pickup, hotbar, and inventory silhouettes.
* Sprint toward a one-block obstacle while holding **L + R** to vault it.
  Long falls hurt, while slime gel can automatically cushion one landing.
* A bounded eight-slot mob pool supports sheep, pigs, night slimes, zombies,
  and spiders, budgeted at five passive and three hostile so adding species
  never becomes an unbounded AI or matrix cost. Animals flee attackers and
  drop species-specific resources; hostiles telegraph a strike before it
  lands, and leave at sunrise rather than lingering.
* Apples and raw meat restore health and food. The HUD includes half-hearts,
  a food bar, a compass, progressive opening objectives, and a compact
  C-button guide.
* Runtime terrain is nibble-packed, halving the live world-array cost — which
  is what makes a 32×32-column window affordable at 1 MiB.

* Up to four players: Controller 2 joins with horizontal split-screen; adding
  Player 3 or 4 switches to a 2x2 split-screen. Players join a running world
  in controller order with **START** on their controller.
* All players have independent movement, camera control, targeting, block
  selection, placement, breaking, inventory management, and crafting.
* Split-screen render commands and every camera/entity matrix are
  double-buffered. This prevents the RSP from reading a camera transform while
  the CPU prepares the following frame, an important difference on real N64
  hardware.
* Each player sees the other as a lightweight Steve-style character, with a
  walking swing and head/eye direction that follows their camera.
* Co-op saves both player positions and inventories; older single-player saves
  still load safely.
* Save v10 preserves exact hotbar, inventory, crafting, carried-item,
  objective, hunger, world clock, tree, and packed-terrain state.
  It validates player/world data with a checksum and uses temporary plus backup
  files so an interrupted cartridge write can recover the previous world.
  It still writes the original fixed footprint, so saving is refused away
  from spawn until per-chunk diff saves land, and torches, stairs, doors and
  windows are not yet carried — see *Known gaps* below.
* Gatherable terrain becomes a protected physical pickup. Rock requires a
  pickaxe to yield resources; coal accepts any pickaxe, while iron requires
  stone or better. Axes accelerate wood and preserve whole-tree felling.
* New worlds combine oceans, lakes, winding river channels, lowlands, rolling
  grasslands, ridge-shaped mountain regions, beaches/deserts, variable soil
  depths, exposed stone cliffs, and underground cave networks.
* Every gatherable block becomes a physical pickup, which pops out, settles,
  and then pulls into the nearby player. Tools are non-stackable, pickups are
  protected when the entity pool is full, and partial inventory transfers can
  no longer duplicate items.
* Long frame hitches are clamped before physics simulation, preventing world
  generation or storage delays from pushing players through terrain.
* Co-op deliberately uses a narrower view and no more than 24 visible columns
  per player in two-player mode, or 8 in four-player mode. This keeps the RSP
  and display-list workload within the limits of an unmodified N64.
* Original, generated 16-colour block tiles and UI font replace the former
  external Minecraft-art build dependency.

## Controls

| Control | Action |
| --- | --- |
| Analog stick | Walk |
| Hold L + analog stick | Sprint |
| Hold Z + analog stick | Look around |
| C-up | Toggle first-person / third-person camera |
| C-down | Open the pack or the targeted crafting table |
| C-left / C-right | Cycle the selected hotbar block |
| A | Open a door, use a crafting table, place a block or torch, or eat held food |
| Hold B / tap B | Mine / punch a nearby mob; swords deal more damage |
| START | Open / close that player's inventory |
| R | Jump |
| Hold L + R while moving | Vault a one-block obstacle |
| D-pad (either player) | Save, when cartridge storage is available |
| Z + D-pad Up | Toggle the developer diagnostics overlay |
| Controller 2-4 START | Join co-op during a running world |

Sprinting costs food, so **L** only sprints while the food bar has something
left in it. A full bar slowly restores health; an empty one drains health
down to a last half-heart rather than killing outright.

The bottom hotbar starts empty. Its bright slot and the enlarged block at the
lower right show what each player is holding; that is the block placed with
**A**. Hold B to mine—releasing B or looking away resets the breaking progress.
Gatherable terrain pops out before flying into the player. Rock requires a
pickaxe to yield resources; coal accepts any pickaxe, while iron requires
stone or better. Axes speed up logs and planks, and bedrock does not break.
In co-op, face a nearby player and tap **B** to swing. A player who loses all
ten hearts respawns at full health with their inventory intact. Sheep and pigs
flee attacks; hostile slimes pursue players at night.

Either player can press **START** or **C-down** to open their pack. The analog
stick alone moves the cursor: the left pane contains three storage rows plus
the nine-slot hotbar, and moving right from the last column enters the recipe
browser. Press **A** to move a stack or craft one recipe and **B** to return to
the game. In the pack, **C-left** takes or places one item, **C-right**
quick-moves a stack between storage and the hotbar, **C-down** drops one, and
**C-up** drops the full selected stack. In the recipe browser, **C-up** crafts
the maximum amount that both ingredients and free space allow.

Pocket crafting offers planks, sticks, a crafting table, and torches. Place a
crafting table, look at it, then press **A**, **C-down**, or **START** to open
the full workbench recipe list. Planks make wooden tools, cobblestone makes
stone tools, and iron chunks make iron tools. The workbench also builds
stairs, doors, and glass windows. Ingredients are consumed directly from the
pack, so crafting never requires manually arranging a hidden 2×2 or 3×3 grid.

Torches, stairs, doors, and windows are *details*: their shape or state
cannot be expressed by the sixteen block IDs the packed world allows, so each
one keeps a small record beside the terrain and occupies its cell with a
crafting-table proxy. A torch brightens the light around it; a door is
opened and closed with **A**. Because an old ID-10 cell with no record is
still an ordinary crafting table, existing saves are unaffected.

## SummerCart64

For the normal SummerCart64 menu workflow, upload the built ROM to the
cartridge SD card, then select it in the menu. This is the persistent,
recommended deployment path. **Turn the N64 off before this upload**: while
the console is running, it owns the SummerCart64 SD card and the USB write is
rejected as locked by the N64 side.

```sh
../N64FlashcartMenu/tools/sc64/sc64deployer sd upload \
  build/mine64.n64 /Games/Mine64.n64
```

The menu path is `/Games/Mine64.n64`. The prior ROM is preserved as
`/Games/Mine64_original.64`. The former 64x64-world saves
(`mine64/world_*.m64`) are left alone. This 112x112-world build uses
`mine64/w112_*.m64`, so saves cannot be misread with a different packed-world
length. Saves are stored on the cartridge SD card when libcart detects a
supported flash cartridge. Once the upload completes, turn the N64 on and
select Mine64 from the SummerCart64 SD menu.

Every save path must be a legal 8.3 short name. `ffconf.h` sets `FF_USE_LFN`
to 0, so FatFs rejects a basename over eight characters or an extension over
three with `FR_INVALID_NAME` before it ever touches the card. The
`world_large_*`, `world_128_*` and `world_112_*` schemes all overran that
limit, which is why saving silently stopped working when the world outgrew
the original 64x64 `world_1.m64` and stayed broken through every later
extent. The SD card must also be FAT16 or FAT32: `FF_FS_EXFAT` is 0, and
cards over 32 GB are formatted exFAT by default. When storage does not come
up, the title screen names the failing step rather than reporting a missing
cartridge for all of them.

For the usual persistent build-and-save workflow, run the project script from
the repository root:

```sh
./perma-load
```

It verifies that the SummerCart64 SD card is initialized and that the N64 is
powered off, which releases the SD card from the N64 side for USB writing. It
then builds the ROM in `mine64-nusys-build:local`, saves it to
`/Games/Mine64.n64`, and verifies the stored file. Turn the N64 on afterward
and select Mine64 from the SummerCart64 SD menu. Set `SC64_DEPLOYER` or
`SC64_SD_PATH` to override the deployer location or menu path.

For development-only USB streaming, with the SummerCart64 connected, use:

```sh
../N64FlashcartMenu/tools/sc64/sc64deployer upload build/mine64.n64
```

This sends the ROM to the cartridge's RAM and configures **Bootloader → ROM**;
it does not create or replace a file on the SD card. To bypass the bootloader
and start the uploaded ROM directly, add `--direct`.

For the usual edit-build-test loop, run the project script from the repository
root instead:

```sh
./live-load
```

It builds the default ROM in `mine64-nusys-build:local`, uploads it to the
connected SummerCart64's temporary ROM RAM, and confirms the boot mode. Set
`SC64_DEPLOYER` if your deployer is not at the adjacent
`../N64FlashcartMenu/tools/sc64/sc64deployer` path.

## Build

Mine64 uses a modern compatibility environment rather than the original
proprietary Nintendo SDK. It contains the `mips-n64-*` cross compiler,
NuSystem, libultra, libcart, `spicy`, and `makemask`.

### SDK setup used for this build

The environment was assembled from the public
[ModernN64SDKArchives/n64sdkmod](https://github.com/ModernN64SDKArchives/n64sdkmod)
package archive. The source-controlled
[`docker/N64SDK.Dockerfile`](docker/N64SDK.Dockerfile) downloads the compiler
and newlib packages, sparse-checks out the NuSystem/libultra/libcart packages,
and installs them under the historical compatibility root `/etc/n64`.

On macOS, Linux, or Windows with Docker Desktop, build the Linux/amd64 image:

```sh
docker build --platform linux/amd64 \
  -t mine64-nusys-build:local \
  -f docker/N64SDK.Dockerfile .
```

`linux/amd64` is required because the archived host tools (`spicy` and the
cross compiler) are amd64 binaries. Docker Desktop handles this on Apple
Silicon.

### Build the ROM

Build inside that container:

```sh
docker run --rm --platform linux/amd64 \
  -v "$PWD:/work" -w /work \
  mine64-nusys-build:local make -j2
```

If you already installed a compatible SDK yourself, set `ROOT=/etc/n64` (or
the equivalent compatibility root) and run:

```
make
```

The ROM is written to `build/mine64.n64`. `generate_assets.py` runs
automatically and creates the compact original tile set and UI font required by
the game; no Minecraft assets are needed. The build uses
`toolchain/spicy-ld.sh` to bridge the historical makerom flags and
compiler-runtime objects to modern GNU ld.

The default is an optimized release ROM using the non-debug NuSystem and
libultra libraries. For an unoptimized SDK debug build, run `make DEBUG=1`.
Release and debug objects are cached separately, so switching variants is safe.

The stable ROM deliberately keeps runtime audio disabled. To build the isolated
real-hardware audio variant without replacing it, run:

```sh
make AUDIO=1
```

That writes `build/mine64-audio.n64`. The audio ROM uses full-size NuSystem
command buffers, explicitly aligned VADPCM books, and starts the audio manager
only after flashcart storage initialization. Music waits for 60 rendered
frames before its first ROM DMA, making a scheduler failure distinguishable
from a boot or storage failure.

### Texture art exports

The game tiles are generated in `generate_assets.py` as 16x16 CI4 textures.
Export the current art for a viewer or paint workflow with:

```
python3 tools/export_textures.py
```

This writes `art/mine64-textures.png` (a native 64x48 atlas),
`art/mine64-textures-preview.png` (16x nearest-neighbour), and
`art/mine64-textures.json` (tile order and palette metadata).

### Custom texture import

Put an edited 4x3 PNG atlas at `art/custom-textures.png`. All twelve cells are
used in this order: dirt, stone, grass top, grass side, cobblestone, sand, log
end, log side, leaves, planks, bricks, water.
`make` automatically converts each cell into a 16x16, 16-colour CI4 tile.
The importer preserves crisp source pixels with nearest sampling and reduces
each tile to its own N64 palette. Coal ore, iron ore, bedrock, and mossy
cobblestone are generated as companion tiles, keeping the editable atlas at
its compact 4×3 size.

### Music asset pipeline

The audio ROM consumes versioned, N64-ready assets in `assets/audio/`; it does
not require private WAV masters or any user-specific path. The tracked payloads
are VADPCM music plus big-endian PCM effects, with the matching decoder
metadata headers in `assets/`.

To deliberately replace the two music tracks, keep the private WAV masters in
any local directory and re-import them inside the same Docker environment used
for ROM builds:

```sh
docker run --rm --platform linux/amd64 \
  -v "$PWD:/work" -v /Users/codi/Downloads:/music:ro -w /work \
  mine64-nusys-build:local make music MUSIC_SOURCE_DIR=/music
```

The pipeline downmixes each track to mono, resamples to 12,000 Hz, removes
inaudible sub-bass/ultrasonic content, normalizes peaks to -3 dBFS, and
encodes the result as N64 VADPCM. It replaces the checked-in payloads and
metadata headers; intermediate WAV and AIFF-C files remain under
`build/audio-import/`.

VADPCM uses roughly 28% of the space of 16-bit PCM. The default 12 kHz mono
asset, filtered at 5.2 kHz, is the chosen quality/ROM-size balance for
background music. To prefer clearer assets, set both `MUSIC_RATE` and a
rate-appropriate `MUSIC_LOWPASS`, for example
`make music MUSIC_RATE=16000 MUSIC_LOWPASS=7000`. To use a different private
source folder, set `MUSIC_SOURCE_DIR=/path/to/music`. To keep the input's
level and EQ unchanged, set `MUSIC_EFFECTS=`. Review and commit the resulting
encoded assets whenever the masters or import settings change.

Music playback is intentionally not linked into the default ROM. Use the
separate `AUDIO=1` ROM for hardware validation; a known-good silent build
always remains available at `build/mine64.n64`.

### Gameplay sound effects

The audio ROM also includes four short original PCM effects: pickup, punch,
block break, and block place. Generate ordinary WAV previews and the N64
big-endian PCM inputs with:

```sh
make sfx
```

The encoded inputs are written to `assets/audio/sfx/`, while local WAV previews
there are ignored. The effects are generated deterministically by
`tools/generate_sfx.py`; review and commit the PCM inputs and metadata header
after regenerating them.

For the complete art-to-cartridge walkthrough, see
[Custom texture workflow](docs/custom-textures.md).

## Hardware notes

Mine64 renders the same world mesh for every camera rather than duplicating the
world. In co-op it also submits one small, untextured Steve-style model per
viewport. The split-screen viewports share the original framebuffer, and the
explicit co-op visibility cap keeps the main per-frame display list inside its
fixed hardware budget, including third-person avatars and pickups. All
per-frame display lists and referenced matrices are double-buffered so their
memory stays immutable until the RSP finishes.

The linked release program leaves roughly 300 KiB free below NuSystem's fixed
framebuffer reservation, including the 1 MiB block window, the 1 MiB mesh
arena, NuSystem task buffers, and doubled render state. It remains within the
stock console's 4 MiB RDRAM; an Expansion Pak is not required.
`tools/check_ram.py` runs at the end of every build and fails it on an
overrun, because nothing at link time notices when BSS grows into addresses
NuSystem pins at runtime. The audio variant does not currently fit and is
deferred; see *Known gaps*.

### Freeze forensics

Mine64 cannot be run under a debugger: it runs on real hardware, and the
console's only output is the screen.  The game therefore ships (in
development builds) with a self-diagnosing freeze rig that has turned
multi-session bisection hunts into single-command answers.  Keep it until
the streaming work is long stable; its cost is a few counters and one
watchdog thread.

**On-screen diagnostics** (left edge, single player; off by default —
**Z + D-pad Up** toggles it, and it switches itself on whenever an
integrity counter ticks, so an absorbed anomaly is never silent.  With the
overlay up, **Z + D-pad Left/Right** walk the fog start — the `P` row —
**Z + D-pad Down** toggles fog for an A/B against the bare streaming
edge, and **Z + C-left** cycles the LOD/visibility presets — the `E` row,
promote radius × 1000 + solo visible-column cap, 3120 being the shipped
default; columns re-LOD over a few seconds, so read FPS after `W`/`B`
settle): `X Z` player block position, `F` frame heartbeat (frozen F = no frames being built), `O`
origin rebases, `M`/`V` frame-list peak and overflows, `R D Q A C T`
streaming state (resident / decorated / queued / arena-free% / allocated mesh
blocks / terrain-pending), `W`/`B` worst frame gap and worst gated-CPU cost in
tenths of a millisecond over ~2 s (W 166 is clean 60 Hz; B tracking W
blames callback CPU work, W high with B low blames the RSP/RDP), `L`
runaway-loop guard trips, `G` position-sanity snaps, `K` corrupted window
keys caught and repaired, `P` the fog start.

Two rows are borrowed while a specific fault is being reproduced: once the
collision boundary marcher's guard has fired, `C` and `K` are replaced by `S`
(the swept-frame speed that triggered it) and `N` (its final candidate
boundary time), because during that hunt those two numbers matter more than
the arena census.

**The phase square** (lower left) is painted into the framebuffer by the
CPU — no RSP, no display list — and a priority-126 watchdog thread repaints
it into *both* framebuffers once the graphics-callback heartbeat stalls for
2 s, so it survives buffer swaps, infinite loops, and the death of the
graphics thread.  On a frozen screen it reads in three bands: **top** =
subsystem (green streaming, yellow rebase, cyan draw, magenta player,
orange trees, white items, black mobs, blue clean-idle, red = RSP/RDP task
hang); **middle** = last player sub-step; **bottom** = white if the CPU
took an exception (crash), black if not (spin).

**The SD post-mortem.**  Two seconds into a freeze the watchdog writes
`mine64/freeze.txt` to the cartridge SD card: faulting thread, PC, CAUSE,
BADVADDR, RA, SP, raw player-position bits, render origin, and every
counter.  Deploy scripts archive the matching symbols
(`build/mine64-deployed.out`) on every upload, and one command turns the
report into named functions:

```sh
./tools/resolve_freeze.sh
```

(Console off first — it owns the SD card while running.)  This is how a
long-standing "walks far, then dies" freeze was resolved to a single
instruction: an FPU unimplemented-operation fault in `guTranslate`, fed by
a corrupted window key.

**Loop guards over hangs.**  Any loop whose termination rests on float
edge-cases carries a bounded iteration guard that breaks out and increments
an on-screen counter instead of hanging a thread that nothing preempts.  A
rising counter with no freeze is a confession — the bug becomes observable
and survivable while the root cause is hunted.

### Two hardware faults that emulators do not reproduce

Both of these cost a long debugging session. Neither shows up under emulation,
and both present as "the picture is fine and then the console stops dead".

**Drain the RDP pipe before reconfiguring it.** Changing cycle type, render
mode, combine mode, or the loaded texture tile while a primitive is still in
flight is an RDP hazard. Hardware locks up hard — no restart, power cycle
required. Whether it bites depends on how busy the pipe still is, so it tracks
scene complexity: dense terrain locks, flat terrain survives, and the same ROM
looks intermittent. Symptoms before the lock include torn frames and garbled
text glyphs, because attributes changed underneath a primitive.

`beginText()` therefore opens with `gsDPPipeSync()`, and every branch of
`drawHUD()` drains the pipe before `drawMenu()` reconfigures the RDP for text.
Issuing a texture rectangle while the RDP is still in `G_CYC_FILL` — for
example straight after `clearBuffers()` — locks the console immediately and
reproducibly. If new drawing code mixes fill rectangles and textured
primitives, sync between them.

**Never busy-wait on the graphics thread.** `nuGfxTaskAllEndWait()` spins on
`nuGfxTaskSpool`, but `callbackGfx` runs on NuSystem's graphics thread at
priority 50, while the completion that clears that counter is posted by the
scheduler's own graphics thread at priority **17** (set in `nusched.c`, not in
any header). The N64 scheduler is strictly priority-based with no time
slicing, so a spin at 50 starves 17 forever and the wait never ends. To
serialise against the RSP, gate on the `pendingGfx` argument NuSystem already
passes to `callbackGfx` and do arena-rewriting work *before* the next `draw()`
submits a task — never wait.

## Technical Details

* The world is built from "columns": 8×8 blocks across, the full 32 blocks
  tall, split into four vertical 8×8×8 chunks. Height is fixed, so this is 2D
  streaming — there is no vertical paging.
* Live terrain occupies a **32×32-column wrapping window** (1 MiB, nibble
  packed). A column's slot is the low five bits of each chunk coordinate, and
  residency is folded into bit 31 of the slot's key, so `blockGet` is one load
  and one compare. Walking scrolls the window by recycling slots; block data
  is never moved, only overwritten.
* Residency is maintained as three concentric **square** rings — terrain 12,
  structures 11, decoration and meshing 10 (Chebyshev, in chunks). Each stage
  is one ring wider than the next because decoration writes across column
  boundaries, and a column may only advance when everything it can write into
  is already claimed. They are rings rather than view cones because walking
  exposes one new row, but turning around would invalidate a whole cone at
  once.
* Block coordinates are absolute `s32`. The N64 `Mtx` is s15.16 fixed point,
  which loses sub-block precision past about ±512 blocks, so a **render
  origin** is subtracted at the handful of places a world position becomes a
  matrix, and re-centred on the player every 256 blocks.
* For each chunk a greedy scanning algorithm merges adjacent block faces with
  the same texture, to reduce the number of quads that need to be rendered.
  Greedy geometry is held only for the column currently being compiled; the
  scratch is reused for the next one.
* Columns near the player compile to **baked vertex buffers** — quads copied
  out of the shared table and pre-translated into column-local space, batched
  eight per `gSPVertex` under a single matrix. Distant columns compile to a
  **surface shell** in the compact matrix-pair format: ~20–35 quads instead of
  ~170, and no T-junction refinement. The hysteresis between the promote and
  demote radii keeps a boundary column from re-meshing on every step.
* `quads.h` contains the vertex data for all possible shapes and orientations
  of merged quad.
* Meshes live in a single 1 MiB arena as first-fit blocks, one per column,
  vertex data first and command segments after. A defrag slides one block per
  frame into the lowest gap, patching that block's own vertex addresses and
  the per-texture start pointers. This is only safe because every arena
  mutation runs from the graphics callback with no task in flight and
  completes before the next frame is submitted.
* Display lists for a column are recomputed when a block in it changes, when
  it crosses an LOD boundary, or when a newly arrived neighbour means its seam
  was compiled against terrain that had not streamed in yet.
* Columns outside the camera view are culled by excluding their display lists
  from the main display list, with a per-frame cap on visible columns and a
  hard command budget that sheds the most distant terrain rather than
  overrunning the frame buffer.

## Known gaps

* **Saves still write the original fixed footprint**, so saving away from
  spawn is refused with "Too far from spawn to save". Per-chunk diff saves
  plus `world_seed` persistence are the fix; until then a loaded world cannot
  regenerate anything it evicts.
* **Details and edits are not persisted.** Torches, stairs, doors and windows
  live in a record pool that no save format carries, so they reload as the
  crafting tables that proxy them in the terrain; the sparse edit journal is
  likewise in-memory only.
* **Audio does not fit.** The audio variant overruns NuSystem's audio heap;
  it needs roughly another 384 KiB, which arrives with a pooled block window
  or with post-diff-save trimming.
* **Sword tier specials are implemented but unreachable.** `useMobWeaponSpecial`
  and its effect rendering exist and are simulated, but nothing is bound to
  trigger them yet.

## Acknowledgements

Mine64 uses nowl's Perlin noise implementation in C: https://gist.github.com/nowl/828013

Mine64 uses devwizard's [libcart](https://github.com/devwizard64/libcart) library to access files on the cartridge.
The files in the `src/ff` and `include/ff` directories were copied from the libcart project.
