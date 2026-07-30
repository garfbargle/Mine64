# Mine64 for Nintendo 64

Mine64 is a compact block-building game for original Nintendo 64 hardware.
Version 0.4 adds a complete wood-to-iron progression, expanded mobs and
combat, traversal and survival mechanics, and a more informative HUD while
retaining the stable 112×32×112 renderer and loading-screen presentation.

![Mine64 box art](mine64.png)

![Mine64 in-game screenshot](game.png)

## v0.4 highlights

* Coal and iron form compact underground veins above an unbreakable bedrock
  floor. Mossy waystones provide sparse landmarks without increasing the
  permanent world dimensions.
* Wooden, stone, and iron swords, pickaxes, and axes have distinct held,
  pickup, hotbar, and inventory silhouettes.
* Sprint toward a one-block obstacle while holding **L + R** to vault it.
  Long falls hurt, while slime gel can automatically cushion one landing.
* A bounded eight-slot mob pool supports sheep, pigs, and hostile night
  slimes. Animals flee attackers and drop species-specific resources.
* Apples and raw meat restore health. The HUD now includes half-hearts, a
  compass, progressive opening objectives, and a compact C-button guide.
* Runtime terrain is nibble-packed, halving the live world-array cost while
  keeping the proven 112×32×112 rendering and visibility budgets.

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
  objective, world clock, tree, and packed-terrain state.
  It validates player/world data with a checksum and uses temporary plus backup
  files so an interrupted cartridge write can recover the previous world.
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
| C-down | Open inventory or the targeted crafting table |
| C-left / C-right | Cycle the selected hotbar block |
| A | Use a crafting table, place a block, or eat held food |
| Hold B / tap B | Mine / punch a nearby mob; swords deal more damage |
| START | Open / close that player's inventory |
| R | Jump |
| Hold L + R while moving | Vault a one-block obstacle |
| D-pad (either player) | Save, when cartridge storage is available |
| Controller 2-4 START | Join co-op during a running world |

The bottom hotbar starts empty. Its bright slot and the enlarged block at the
lower right show what each player is holding; that is the block placed with
**A**. Hold B to mine—releasing B or looking away resets the breaking progress.
Gatherable terrain pops out before flying into the player. Rock requires a
pickaxe to yield resources; coal accepts any pickaxe, while iron requires
stone or better. Axes speed up logs and planks, and bedrock does not break.
In co-op, face a nearby player and tap **B** to swing. A player who loses all
ten hearts respawns at full health with their inventory intact. Sheep and pigs
flee attacks; hostile slimes pursue players at night.

Either player can press **START** to open their inventory. It has a 3-row
storage grid, a selectable nine-slot hotbar, and a working 2x2 crafting area.
Navigate with the analog stick, D-pad, or C-buttons. For crafting, select a
material stack, move left into the craft grid, and press **A** to place one
directly from the blue-outlined source slot; **B** returns one to that slot.
Press **A** on the output to craft directly into storage—there is no need to
pick up and re-place the result. The labelled Hand slot remains available for
moving whole stacks around the inventory: press **A** on an item slot to pick
up or place a stack, or **B** to split one item. One log makes four planks; two
vertical planks make four sticks; and four planks make a crafting table.
Place a crafting table, look at it, then press **A**, **C-down**, or **START**
to open its 3×3 grid. Planks make wooden tools, cobblestone makes stone tools,
and iron chunks make iron tools.

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
(`mine64/world_*.m64`) are left alone. The
96x96-world saves (`mine64/world_large_*.m64`) and 128x128-world saves
(`mine64/world_128_*.m64`) are also preserved. This 112x112-world build uses
`mine64/world_112_*.m64`, so saves cannot be
misread with a different packed-world length. Saves are stored on the
cartridge SD card when libcart detects a supported flash cartridge. Once the
upload completes, turn the N64 on and select Mine64 from the SummerCart64 SD
menu.

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

The linked release program currently occupies about 3.1 MiB including its world,
geometry cache, NuSystem task buffers, and doubled render state. It remains
within the stock console's 4 MiB RDRAM; an Expansion Pak is not required.

## Technical Details

* The world consists of a 14x14 grid of "columns", each split into 4 vertical chunks of 8x8x8 blocks each (a 112x32x112-block world).
* Runtime blocks remain nibble-packed; hot accessors decode the requested
half-byte without allocating an expanded terrain mirror.
* For each chunk a greedy scanning algorithm merges adjacent block faces with the same texture,
to reduce the number of quads that need to be rendered.
* Greedy geometry is held only for the four chunks in the column currently
being compiled. The resulting display lists persist, while the scratch mesh is
reused for the next column and for edited columns.
* `quads.h` contains the vertex data for all possible shapes and orientations of merged quad.
These quads are translated into place for rendering.
* Display lists for each column are recomputed every time a block changes in the column.
* Columns outside the camera view are culled by excluding their display lists from the main display list.

## Acknowledgements

Mine64 uses nowl's Perlin noise implementation in C: https://gist.github.com/nowl/828013

Mine64 uses devwizard's [libcart](https://github.com/devwizard64/libcart) library to access files on the cartridge.
The files in the `src/ff` and `include/ff` directories were copied from the libcart project.
