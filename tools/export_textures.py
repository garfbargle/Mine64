#!/usr/bin/env python3
"""Export Mine64's generated CI4 tiles for viewers and art iteration.

Outputs:
  art/mine64-textures.png          Native 64x48 atlas (four 16x16 tiles/row)
  art/mine64-textures-preview.png  16x nearest-neighbour inspection atlas
  art/mine64-textures.json         Tile order, palettes, and atlas metadata
"""

import json
import sys
from pathlib import Path
from struct import pack
from zlib import compress

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from generate_assets import PALETTES, tile
from import_textures import decode_png, median_cut

TILE_SIZE = 16
COLUMNS = 4
SCALE = 16
OUTPUT_DIR = ROOT / "art"
CUSTOM_SOURCE = OUTPUT_DIR / "custom-textures.png"


def write_png(path, width, height, pixels):
    """Write an RGB PNG with only Python's standard library."""
    raw_rows = bytearray()
    for y in range(height):
        raw_rows.append(0)  # PNG's no-filter byte
        for r, g, b in pixels[y * width:(y + 1) * width]:
            raw_rows.extend((r, g, b))

    def chunk(kind, data):
        return pack(">I", len(data)) + kind + data + pack(">I", __import__("zlib").crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", compress(bytes(raw_rows), 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def main():
    names = list(PALETTES)
    rows = (len(names) + COLUMNS - 1) // COLUMNS
    width = COLUMNS * TILE_SIZE
    height = rows * TILE_SIZE
    pixels = [(24, 28, 28)] * (width * height)
    manifest_tiles = []

    imported = None
    if CUSTOM_SOURCE.exists():
        source_width, source_height, source_image = decode_png(CUSTOM_SOURCE)
        if source_width % COLUMNS or source_height % 3:
            raise ValueError("custom texture atlas must divide evenly into a 4x3 grid")
        imported = (source_width // COLUMNS, source_height // 3, source_image)

    for number, name in enumerate(names):
        atlas_x = (number % COLUMNS) * TILE_SIZE
        atlas_y = (number // COLUMNS) * TILE_SIZE
        if imported:
            cell_width, cell_height, source_image = imported
            origin_x = (number % COLUMNS) * cell_width
            origin_y = (number // COLUMNS) * cell_height
            tile_pixels = [
                source_image[origin_y + min(cell_height - 1, (y * cell_height + cell_height // 2) // TILE_SIZE)]
                            [origin_x + min(cell_width - 1, (x * cell_width + cell_width // 2) // TILE_SIZE)]
                for y in range(TILE_SIZE) for x in range(TILE_SIZE)
            ]
            palette = median_cut(tile_pixels, 16)
        else:
            palette = PALETTES[name]
            tile_pixels = [palette[tile(name, x, y) - 1] for y in range(TILE_SIZE) for x in range(TILE_SIZE)]
        for y in range(TILE_SIZE):
            for x in range(TILE_SIZE):
                pixels[(atlas_y + y) * width + atlas_x + x] = tile_pixels[y * TILE_SIZE + x]
        manifest_tiles.append({
            "name": name,
            "atlas_x": atlas_x,
            "atlas_y": atlas_y,
            "palette_rgb": palette,
        })

    OUTPUT_DIR.mkdir(exist_ok=True)
    write_png(OUTPUT_DIR / "mine64-textures.png", width, height, pixels)

    preview = []
    for y in range(height):
        expanded_row = []
        for pixel in pixels[y * width:(y + 1) * width]:
            expanded_row.extend([pixel] * SCALE)
        for _ in range(SCALE):
            preview.extend(expanded_row)
    write_png(OUTPUT_DIR / "mine64-textures-preview.png", width * SCALE, height * SCALE, preview)

    (OUTPUT_DIR / "mine64-textures.json").write_text(json.dumps({
        "format": "RGB preview of Mine64 CI4 tiles",
        "source": "custom-textures.png" if imported else "generate_assets.py",
        "tile_width": TILE_SIZE,
        "tile_height": TILE_SIZE,
        "atlas_columns": COLUMNS,
        "tile_order": names,
        "tiles": manifest_tiles,
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
