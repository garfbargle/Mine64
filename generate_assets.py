#!/usr/bin/env python3
"""Generate Mine64's tiny, original N64-ready art set.

The project intentionally does not bundle Minecraft Classic artwork.  These
procedural 16-colour tiles and 5x7 UI glyphs are generated locally, so a clean
checkout can build without downloading proprietary game assets.
"""

from pathlib import Path

ASSET_DIR = Path("assets")


def rgba5551(rgb):
    r, g, b = rgb[:3]
    alpha = rgb[3] if len(rgb) == 4 else 255
    return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (1 if alpha else 0)


def pack_nibbles(values):
    result = []
    for offset in range(0, len(values), 8):
        word = 0
        for value in values[offset:offset + 8]:
            word = (word << 4) | value
        result.append(word)
    return result


def mottled(x, y, salt):
    """A 2x2-pixel, low-frequency pattern that stays readable on a CRT."""
    return ((x // 2) * 3 + (y // 2) * 5 + ((x // 4) * (y // 4)) + salt) & 3


# Hand-composed low-frequency tiles. These stay 16x16 CI4 textures, but avoid
# the obvious mathematical checkerboard a simple formula creates when repeated
# across a large greedy-meshed surface.
GRASS_TOP = (
    "2222233333222222",
    "2222333333322222",
    "2222333333322222",
    "2222233333332222",
    "2222223333333222",
    "2222222333333222",
    "2222222233333222",
    "2222222233333222",
    "2222222223333222",
    "2222222222333322",
    "2222222222333322",
    "2222222222233322",
    "2222222222223322",
    "2222222222223332",
    "2222222222222332",
    "2222222222222222",
)

SAND = (
    "2222222222222222",
    "2222222222222222",
    "2222222222222322",
    "2222222222233322",
    "2222222222333322",
    "2222222222333222",
    "2222222222332222",
    "2222222222222222",
    "2222222222222222",
    "2222222322222222",
    "2222223332222222",
    "2222233332222222",
    "2222233322222222",
    "2222223322222222",
    "2222222222222222",
    "2222222222222222",
)

def tile(kind, x, y):
    # These designs intentionally avoid 1-pixel noise. Nearest-sampled CI4
    # texture on the N64 makes that kind of detail shimmer and look corrupted.
    if kind == "dirt":
        return 1 + (mottled(x, y, 1) >> 1)
    if kind == "stone":
        return 1 + (mottled(x, y, 3) >> 1)
    if kind in ("coal_ore", "iron_ore"):
        # Reuse stone's broad clusters, then lay sparse 2x2 ore seams over
        # them.  Large marks survive composite video far better than speckles.
        seam = ((x // 2) * 7 + (y // 2) * 11 + (x // 4) * (y // 4)) % 17
        if seam < 2:
            return 4 + seam
        return 1 + (mottled(x, y, 3) >> 1)
    if kind == "bedrock":
        return 1 + ((mottled(x, y, 6) + (x // 4) + (y // 4)) % 3)
    if kind == "mossy_cobblestone":
        shifted_x = (x + ((y // 4) & 1) * 3) & 15
        if y % 4 == 0 or shifted_x % 8 == 0:
            return 1
        if ((x // 4) * 3 + y // 3) % 7 < 2:
            return 4
        return 2 + ((shifted_x // 4 + y // 4) & 1)
    if kind == "grass_top":
        return int(GRASS_TOP[y][x])
    if kind == "grass_side":
        if y < 4:
            return 1 + ((x // 4 + y // 2) & 1)
        return 4 + (mottled(x, y - 4, 2) >> 1)
    if kind == "cobblestone":
        # Offset 8x4 stones with a single dark mortar pixel between them.
        shifted_x = (x + ((y // 4) & 1) * 3) & 15
        if y % 4 == 0 or shifted_x % 8 == 0:
            return 1
        return 2 + ((shifted_x // 4 + y // 4) & 1)
    if kind == "sand":
        return int(SAND[y][x])
    if kind == "water":
        wave = ((x + y * 2) // 4) & 3
        return 1 + (wave if wave < 3 else 1)
    if kind == "wood_top":
        ring = max(abs(x - 7), abs(y - 7))
        return 1 + ((ring // 2 + (x // 6)) & 2)
    if kind == "wood_side":
        if y % 6 == 0:
            return 1
        return 2 + ((x // 4) & 1)
    if kind == "leaves":
        return 1 + ((mottled(x, y, 0) + x // 4) >> 2)
    if kind == "planks":
        if y % 5 == 0:
            return 1
        return 2 + ((x // 6 + y // 5) & 1)
    if kind == "bricks":
        shifted_x = (x + ((y // 4) & 1) * 3) & 15
        if y % 4 == 0 or shifted_x % 8 == 0:
            return 1
        return 2 + ((shifted_x // 4 + y // 4) & 1)
    if kind == "wool":
        # Broad, offset tufts read as fleece instead of TV-static on a CRT.
        if (x + (y // 4) * 3) % 8 == 0 or y % 5 == 0:
            return 1
        return 2 + ((x // 4 + y // 5) & 1)
    if kind == "sun":
        # A subtle cross-shaped corona reads clearly at N64 resolution without
        # shimmering like a noisy radial gradient.
        distance = abs(x - 7) + abs(y - 7)
        if distance < 4:
            return 3
        if distance < 7:
            return 2
        return 1
    if kind.startswith("moon_"):
        phase = int(kind.rsplit("_", 1)[1])
        dx = x - 7.5
        dy = y - 7.5
        if dx * dx + dy * dy > 45:
            return 0
        # The phases progress full, waning, new, then waxing.  Deliberately
        # blocky edges preserve the project’s original pixel-art language.
        widths = (8, 6, 4, 2, 0, 2, 4, 6)
        width = widths[phase]
        if phase == 0:
            return 2 + ((x + y) & 1)
        if phase == 4 or abs(dx) > width:
            return 0
        if phase < 4 and dx > 0:
            return 0
        if phase > 4 and dx < 0:
            return 0
        return 2 + ((x + y) & 1)
    raise ValueError(f"unknown tile: {kind}")


PALETTES = {
    "dirt": [(79, 47, 25), (105, 65, 35), (132, 86, 47), (0, 0, 0)],
    "stone": [(71, 76, 78), (94, 100, 102), (119, 124, 124), (0, 0, 0)],
    "coal_ore": [(71, 76, 78), (94, 100, 102), (119, 124, 124),
                 (0, 0, 0), (27, 29, 31), (47, 50, 51)],
    "iron_ore": [(71, 76, 78), (94, 100, 102), (119, 124, 124),
                 (0, 0, 0), (151, 104, 72), (190, 137, 92)],
    "bedrock": [(25, 27, 29), (43, 46, 48), (62, 66, 67), (0, 0, 0)],
    "mossy_cobblestone": [(45, 51, 48), (66, 73, 72), (91, 99, 97),
                          (113, 121, 117), (45, 91, 48)],
    "grass_top": [(30, 76, 34), (43, 101, 43), (58, 123, 50), (76, 143, 57)],
    "grass_side": [(30, 76, 34), (43, 101, 43), (58, 123, 50), (79, 47, 25), (105, 65, 35), (132, 86, 47)],
    "cobblestone": [(57, 62, 64), (85, 91, 93), (108, 114, 115), (132, 137, 137)],
    "sand": [(166, 143, 88), (190, 168, 108), (211, 190, 132), (0, 0, 0)],
    "wood_top": [(72, 43, 22), (103, 62, 30), (137, 87, 43), (174, 115, 62)],
    "wood_side": [(67, 39, 20), (95, 56, 27), (127, 78, 38), (160, 103, 54)],
    "leaves": [(24, 62, 30), (35, 84, 39), (48, 105, 47), (68, 126, 57)],
    "planks": [(91, 55, 27), (120, 75, 36), (152, 99, 50), (184, 127, 69)],
    "bricks": [(93, 43, 34), (126, 57, 45), (157, 73, 57), (188, 92, 72)],
    "wool": [(142, 144, 145), (188, 190, 188), (224, 224, 216), (248, 246, 232)],
    "water": [(22, 62, 112), (31, 88, 148), (47, 116, 178), (72, 145, 201)],
    "sun": [(255, 175, 56), (255, 207, 76), (255, 239, 139)],
    "moon_0": [(0, 0, 0, 0), (121, 137, 166), (186, 200, 218), (226, 231, 236)],
    "moon_1": [(0, 0, 0, 0), (121, 137, 166), (186, 200, 218), (226, 231, 236)],
    "moon_2": [(0, 0, 0, 0), (121, 137, 166), (186, 200, 218), (226, 231, 236)],
    "moon_3": [(0, 0, 0, 0), (121, 137, 166), (186, 200, 218), (226, 231, 236)],
    "moon_4": [(0, 0, 0, 0), (121, 137, 166), (186, 200, 218), (226, 231, 236)],
    "moon_5": [(0, 0, 0, 0), (121, 137, 166), (186, 200, 218), (226, 231, 236)],
    "moon_6": [(0, 0, 0, 0), (121, 137, 166), (186, 200, 218), (226, 231, 236)],
    "moon_7": [(0, 0, 0, 0), (121, 137, 166), (186, 200, 218), (226, 231, 236)],
}


def write_textures():
    chunks = ["// Generated original Mine64 art; see generate_assets.py.\n#include <nusys.h>\n\n",
              "typedef struct { u32 color_indices[32]; u16 pallet[16]; } __attribute__((aligned(8))) Texture;\n"]
    for name, palette in PALETTES.items():
        indices = [tile(name, x, y) for y in range(16) for x in range(16)]
        words = pack_nibbles(indices)
        palette_words = [rgba5551(colour) for colour in palette] + [0] * (16 - len(palette))
        index_text = ", ".join(f"0x{word:08X}" for word in words)
        palette_text = ", ".join(f"0x{word:04X}" for word in palette_words)
        chunks.append(f"\nTexture {name}_texture = {{ {{ {index_text} }}, {{ {palette_text} }} }};\n")
    (ASSET_DIR / "texture_data.h").write_text("".join(chunks))


GLYPHS = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "B": ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    "C": ("01111", "10000", "10000", "10000", "10000", "10000", "01111"),
    "D": ("11110", "10001", "10001", "10001", "10001", "10001", "11110"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "F": ("11111", "10000", "10000", "11110", "10000", "10000", "10000"),
    "G": ("01111", "10000", "10000", "10111", "10001", "10001", "01111"),
    "H": ("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "I": ("11111", "00100", "00100", "00100", "00100", "00100", "11111"),
    "J": ("00111", "00010", "00010", "00010", "10010", "10010", "01100"),
    "K": ("10001", "10010", "10100", "11000", "10100", "10010", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": ("10001", "11011", "10101", "10101", "10001", "10001", "10001"),
    "N": ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "Q": ("01110", "10001", "10001", "10001", "10101", "10010", "01101"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": ("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
    "W": ("10001", "10001", "10001", "10101", "10101", "10101", "01010"),
    "X": ("10001", "10001", "01010", "00100", "01010", "10001", "10001"),
    "Y": ("10001", "10001", "01010", "00100", "00100", "00100", "00100"),
    "Z": ("11111", "00001", "00010", "00100", "01000", "10000", "11111"),
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("01110", "10001", "00001", "00010", "00100", "01000", "11111"),
    "3": ("11110", "00001", "00001", "01110", "00001", "00001", "11110"),
    "4": ("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": ("11111", "10000", "10000", "11110", "00001", "00001", "11110"),
    "6": ("01110", "10000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00001", "01110"),
    ":": ("00000", "00100", "00100", "00000", "00100", "00100", "00000"),
    ".": ("00000", "00000", "00000", "00000", "00000", "00110", "00110"),
    ">": ("10000", "01000", "00100", "00010", "00100", "01000", "10000"),
    "<": ("00001", "00010", "00100", "01000", "00100", "00010", "00001"),
    "-": ("00000", "00000", "00000", "11111", "00000", "00000", "00000"),
}


def write_font():
    pixels = [[0 for _ in range(128)] for _ in range(64)]
    for codepoint in range(32, 128):
        glyph = GLYPHS.get(chr(codepoint).upper(), ())
        cell_x = (codepoint - 32) % 16
        cell_y = 16 + ((codepoint - 32) // 16) * 8
        for y, row in enumerate(glyph):
            for x, bit in enumerate(row):
                if bit == "1":
                    pixels[cell_y + y][cell_x * 8 + x + 1] = 15
    packed = pack_nibbles([pixel for row in pixels for pixel in row])
    body = ",\n  ".join(f"0x{word:08X}" for word in packed)
    (ASSET_DIR / "font.h").write_text("// Generated original Mine64 UI font; see generate_assets.py.\n#include <nusys.h>\n\nu32 font_texture[] __attribute__((aligned(8))) = {\n  " + body + "\n};\n")


if __name__ == "__main__":
    ASSET_DIR.mkdir(exist_ok=True)
    write_textures()
    write_font()
