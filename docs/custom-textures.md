# Custom texture workflow

Mine64's editable atlas contains twelve 16×16 CI4 texture tiles. Each tile gets
its own palette of up to 16 colours, so the source art can be richer than a
single shared palette. The build also generates four companion terrain tiles
for coal ore, iron ore, bedrock, and mossy cobblestone.

## 1. Start with the exported atlas

Run this from the project root:

```sh
python3 tools/export_textures.py
```

Use `art/mine64-textures-preview.png` for a large pixel-accurate reference and
`art/mine64-textures.json` for the order and palette metadata.

## 2. Paint a 4×3 atlas

Create or edit a PNG with four columns and three rows. Every cell may be any
size, but must be square and all cells must be the same dimensions. The
importer samples each cell down to 16×16 with nearest-neighbour sampling, so
use crisp pixel art rather than blurred or anti-aliased edges.

All twelve cells are read left-to-right, top-to-bottom:

| Row | Tiles |
| --- | --- |
| 1 | dirt, stone, grass top, grass side |
| 2 | cobblestone, sand, log end, log side |
| 3 | leaves, planks, bricks, water |

Keep important details broad enough to survive at 16×16. Fine one-pixel noise
can shimmer on a CRT. The importer automatically reduces each tile to no more
than 16 opaque RGB colours.

The four companion tiles are derived from the built-in palettes after the
custom atlas is imported. This preserves the small 4×3 authoring surface while
ensuring every terrain ID still has valid N64 texture data.

## 3. Import and build

Save the atlas as:

```text
art/custom-textures.png
```

Then run:

```sh
make
```

`make` detects that file, generates N64-ready CI4 data in `assets/texture_data.h`,
and writes the ROM to `build/mine64.n64`. Remove or rename
`art/custom-textures.png` to return to the built-in procedural tiles.

## 4. Put it on the SummerCart64

With the console not running a ROM, upload `build/mine64.n64` to:

```text
/games/Mine64.n64
```

The preserved pre-v0.2 ROM is `/games/Mine64_original.64`.
