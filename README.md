# Mine64 for Nintendo 64

Mine64 is a compact block-building game for original Nintendo 64 hardware.
Version 0.2 adds a hardware-conscious two-player co-op mode, cartridge saves,
and an asset pipeline that does not require Minecraft files.

![Mine64 box art](mine64.png)

![Mine64 in-game screenshot](game.png)

## v0.2 highlights

* Two-player horizontal split-screen: Player 2 can join a running world with
  **START** on Controller 2.
* Both players have independent movement, camera control, targeting, block
  selection, placement, and breaking.
* Each player sees the other as a lightweight Steve-style character, with a
  walking swing and head/eye direction that follows their camera.
* Co-op saves both player positions and inventories; older single-player saves
  still load safely.
* Co-op deliberately uses a narrower view and no more than 24 visible columns
  per player. This keeps the RSP and display-list workload within the limits of
  an unmodified N64.
* Original, generated 16-colour block tiles and UI font replace the former
  external Minecraft-art build dependency.

## Controls

| Control | Action |
| --- | --- |
| Analog stick | Walk |
| Hold Z + analog stick | Look around |
| A / hold B | Place / punch a tree log |
| C-left / C-right | Cycle the selected hotbar block |
| START (Controller 1) | Open / close inventory |
| R | Jump |
| D-pad | Save, when cartridge storage is available |
| Controller 2 START | Join co-op during a running world |

The bottom hotbar contains all nine placeable block types. Its bright slot and
the enlarged block at the lower right show what each player is holding; that is
the block placed with **A**. Punching a log makes a wood pickup pop out. Walk
close to collect it into a stack of up to 64; placing a wood block consumes
one. Hold B on a log to punch it—releasing B or looking away resets the
breaking progress.

Press **START** to open the inventory. It currently provides a 3-row storage
grid, a selectable nine-slot hotbar, and a visible 2x2 crafting area. The
crafting cells are UI-only for now; recipes will be added next.

## SummerCart64

On the cartridge SD card, launch:

```
/games/Mine64.n64
```

The prior ROM is preserved as `/games/Mine64_original.64`; existing
`mine64/world_*.m64` save files are left alone. Saves are stored on the
cartridge SD card when libcart detects a supported flash cartridge.

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

Put an edited 4x3 PNG atlas at `art/custom-textures.png`. The first eleven
cells are used in this order: dirt, stone, grass top, grass side, cobblestone,
sand, log end, log side, leaves, planks, bricks; the final cell is ignored.
`make` automatically converts each cell into a 16x16, 16-colour CI4 tile.
The importer preserves crisp source pixels with nearest sampling and reduces
each tile to its own N64 palette.

For the complete art-to-cartridge walkthrough, see
[Custom texture workflow](docs/custom-textures.md).

## Hardware notes

Mine64 renders the same world mesh for both cameras rather than duplicating the
world. In co-op it also submits one small, untextured Steve-style model per
viewport. The split-screen viewport shares the original framebuffer, and the
explicit co-op visibility cap keeps the main per-frame display list below its
1,024-command budget.

## Technical Details

* The world consists of a 8x8 grid of "columns", each split into 4 vertical chunks of 8x8x8 blocks each.
* For each chunk a greedy scanning algorithm merges adjacent block faces with the same texture,
to reduce the number of quads that need to be rendered.
* `quads.h` contains the vertex data for all possible shapes and orientations of merged quad.
These quads are translated into place for rendering.
* Display lists for each column are recomputed every time a block changes in the column.
* Columns outside the camera view are culled by excluding their display lists from the main display list.

## Acknowledgements

Mine64 uses nowl's Perlin noise implementation in C: https://gist.github.com/nowl/828013

Mine64 uses devwizard's [libcart](https://github.com/devwizard64/libcart) library to access files on the cartridge.
The files in the `src/ff` and `include/ff` directories were copied from the libcart project.
