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
| A / B | Place / break a block |
| C-left / C-right | Select a block |
| R | Jump |
| D-pad | Save, when cartridge storage is available |
| Controller 2 START | Join co-op during a running world |

## SummerCart64

The v0.2 ROM is intended to live alongside the original build, not replace it.
On the cartridge SD card, launch:

```
/roms/Mine64 v0.2.n64
```

The original `/roms/Mine64.n64` and existing `mine64/world_*.m64` save files
are left alone. Saves are stored on the cartridge SD card when libcart detects
a supported flash cartridge.

## Build

v0.2 builds with a Modern N64 SDK-compatible environment containing the
`mips-n64-*` toolchain, NuSystem, libultra, libcart, `spicy`, and `makemask`.
With that environment installed and its compatibility root at `/etc/n64`:

```
make
```

The ROM is written to `build/mine64_v0_2.n64`. `generate_assets.py` runs
automatically and creates the compact original tile set and UI font required by
the game; no Minecraft assets or Python packages are needed. The build also
uses `toolchain/spicy-ld.sh` to bridge the historical makerom flags and
compiler-runtime objects to modern GNU ld.

## Hardware notes

Mine64 renders the same world mesh for both cameras rather than duplicating the
world or adding player models. The split-screen viewport shares the original
framebuffer, and the explicit co-op visibility cap keeps the main per-frame
display list below its 1,024-command budget.

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
