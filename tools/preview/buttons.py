#!/usr/bin/env python3
"""Draw the controller-button sprites on this machine, without a console.

Buttons are fill-rectangle sprites like the health and food meters, so the
same trick applies: read the `HudSpan` tables and `ButtonStyle` initialisers
out of src/graphics.c and replay the rectangles onto a bitmap.  The sprite in
the picture is the sprite in the ROM.

    tools/preview/buttons.py                # every button, zoomed
    tools/preview/buttons.py --crt          # through fake composite
    tools/preview/buttons.py --guides       # the two in-game guide panels

`--guides` reassembles drawCButtonGuide and drawActionGuide from the same
constants they use, which is the picture worth checking: a button is only
good if it still reads at 1x with a label beside it.
"""

import argparse
import os
import re

import numpy as np
from PIL import Image

import hud
from hud import Canvas, crt, draw_spans, zoom, _source, _spans

ROOT = hud.ROOT

# Order and labels as the guides use them.
ORDER = ["button_a", "button_b", "button_start", "button_c_up", "button_c_down",
         "button_c_left", "button_c_right", "button_l", "button_r", "button_z"]


def _shell_color(src):
    m = re.search(r"button_shell_color\[3\]\s*=\s*\{([^}]*)\}", src)
    return tuple(int(v) for v in re.findall(r"\d+", m.group(1)))


def _macro_shape(src, kind):
    """The ROUND_BUTTON / WIDE_BUTTON macro bodies carry the span counts and
    the sprite size, so read them rather than restating them here."""
    m = re.search(r"#define %s\(([^)]*)\)((?:.*\\\n)*.*)" % kind, src)
    body = m.group(2).replace("\\\n", " ")
    names = re.findall(r"button_\w+_spans", body)
    nums = [int(v) for v in re.findall(r"(?<![\w/])\d+(?![\w])", body)]
    # shell_spans, face_spans, then (glyph_x, glyph_y,) width, height
    return names, nums


def load_styles():
    src = _source()
    styles = {}
    round_names, round_nums = _macro_shape(src, "ROUND_BUTTON")
    wide_names, wide_nums = _macro_shape(src, "WIDE_BUTTON")
    shell = _shell_color(src)

    for name in ORDER:
        m = re.search(
            r"static const ButtonStyle\s+%s\s*=\s*\{(.*?)\};" % name, src, re.S)
        body = m.group(1)
        macro = re.search(r"(ROUND|WIDE)_BUTTON\(([^)]*)\)", body)
        kind = macro.group(1)
        args = [a.strip() for a in macro.group(2).split(",")]
        glyph_name = args[0]
        if kind == "ROUND":
            shell_name, face_name = round_names[0], round_names[1]
            shell_n, face_n = round_nums[0], round_nums[1]
            gx, gy = int(args[1]), int(args[2])
            w, h = round_nums[2], round_nums[3]
        else:
            shell_name, face_name = wide_names[0], wide_names[1]
            shell_n, face_n = wide_nums[0], wide_nums[1]
            gx, gy = wide_nums[2], wide_nums[3]
            w, h = wide_nums[4], wide_nums[5]

        colors = _resolve_colors(src, body[macro.end():])
        styles[name] = {
            "shell": _spans(src, shell_name)[:shell_n],
            "face": _spans(src, face_name)[:face_n],
            "glyph": _spans(src, glyph_name),
            "glyph_x": gx, "glyph_y": gy,
            "width": w, "height": h,
            "shell_color": shell,
            "face_color": colors[0],
            "glyph_color": colors[1],
        }
    return styles


def _resolve_colors(src, tail):
    """The two colours after the macro are either a literal brace triple or a
    BUTTON_* #define that expands to one."""
    out = []
    for token in tail.split(","):
        token = token.strip().strip("{}").strip()
        if not token:
            continue
        if re.match(r"^BUTTON_[A-Z_]+$", token):
            m = re.search(r"#define %s\s*\{([^}]*)\}" % token, src)
            out.append(tuple(int(v) for v in re.findall(r"\d+", m.group(1))))
        else:
            out.append(token)
    # Literal triples arrive as three separate numeric tokens.
    flat, nums = [], []
    for item in out:
        if isinstance(item, tuple):
            if nums:
                flat.append(tuple(nums))
                nums = []
            flat.append(item)
        else:
            nums.append(int(item))
            if len(nums) == 3:
                flat.append(tuple(nums))
                nums = []
    return flat


def draw_button(canvas, style, x, y):
    draw_spans(canvas, style["shell"], len(style["shell"]), x, y,
               255, style["shell_color"])
    draw_spans(canvas, style["face"], len(style["face"]), x, y,
               255, style["face_color"])
    draw_spans(canvas, style["glyph"], len(style["glyph"]),
               x + style["glyph_x"], y + style["glyph_y"], 255,
               style["glyph_color"])


def sheet(styles):
    pad = 5
    width = pad + sum(styles[n]["width"] + pad for n in ORDER)
    height = 11 + pad * 2
    canvas = Canvas(width, height, background=(24, 29, 34))
    x = pad
    for name in ORDER:
        style = styles[name]
        draw_button(canvas, style, x, pad + (11 - style["height"]) // 2)
        x += style["width"] + pad
    return canvas


def _guide_defines(src):
    """The GUIDE_*/CGUIDE_*/AGUIDE_* placement constants, read rather than
    restated -- moving a row in graphics.c must move it here too."""
    out = {}
    for m in re.finditer(r"^#define ((?:C|A)?GUIDE_\w+)\s+(.+)$", src, re.M):
        expr = m.group(2).strip()
        out[m.group(1)] = eval(expr, {"__builtins__": {}}, dict(out))
    return out


def guides(styles):
    """drawCButtonGuide and drawActionGuide, as drawHUD lays them out."""
    src = _source()
    d = _guide_defines(src)
    top = min(d["CGUIDE_PANEL_TOP"], d["AGUIDE_PANEL_TOP"]) - 4
    canvas = Canvas(320, 202 - top, background=(96, 132, 186))

    def panel(left, t, right, bottom):
        t -= top
        bottom -= top
        canvas.fill_rect(left, t, right, bottom, (8, 10, 13))
        canvas.fill_rect(left + 2, t + 2, right - 2, bottom - 2,
                         (131, 137, 139))
        canvas.fill_rect(left + 4, t + 4, right - 4, bottom - 4, (24, 29, 34))

    pitch = d["GUIDE_ROW_PITCH"]

    panel(5, d["CGUIDE_PANEL_TOP"], d["CGUIDE_PANEL_RIGHT"], 198)
    cx, cy = d["CGUIDE_ICON_X"], d["CGUIDE_ROW_Y"] - top
    draw_button(canvas, styles["button_c_up"], cx, cy)
    draw_button(canvas, styles["button_c_left"], cx, cy + pitch)
    draw_button(canvas, styles["button_c_right"], cx + 15, cy + pitch)
    draw_button(canvas, styles["button_c_down"], cx, cy + pitch * 2)

    panel(d["AGUIDE_PANEL_LEFT"], d["AGUIDE_PANEL_TOP"], 314, 198)
    ay = d["AGUIDE_ROW_Y"] - top
    draw_button(canvas, styles["button_a"], d["AGUIDE_ROUND_X"], ay)
    draw_button(canvas, styles["button_b"], d["AGUIDE_ROUND_X"], ay + pitch)
    draw_button(canvas, styles["button_r"], d["AGUIDE_ICON_X"],
                ay + pitch * 2 + 1)

    text(canvas, "CAMERA", d["CGUIDE_LABEL_X"], cy + d["GUIDE_LABEL_DROP"])
    text(canvas, "ITEMS", d["CGUIDE_LABEL_X"],
         cy + pitch + d["GUIDE_LABEL_DROP"])
    text(canvas, "PACK", d["CGUIDE_LABEL_X"],
         cy + pitch * 2 + d["GUIDE_LABEL_DROP"])
    text(canvas, "USE", d["AGUIDE_LABEL_X"], ay + d["GUIDE_LABEL_DROP"])
    text(canvas, "MINE", d["AGUIDE_LABEL_X"],
         ay + pitch + d["GUIDE_LABEL_DROP"])
    text(canvas, "JUMP", d["AGUIDE_LABEL_X"],
         ay + pitch * 2 + d["GUIDE_LABEL_DROP"])
    return canvas


# A 5x7 stand-in for the UI font, enough to judge whether a label sits level
# with its button and clears the panel edge.  It is not the game's font.
FONT_5X7 = {
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "C": ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "I": ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
    "J": ["00111", "00010", "00010", "00010", "10010", "10010", "01100"],
    "K": ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "N": ["10001", "11001", "10101", "10101", "10011", "10001", "10001"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "F": ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    "G": ["01110", "10001", "10000", "10111", "10001", "10001", "01110"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    "W": ["10001", "10001", "10001", "10101", "10101", "11011", "10001"],
    "X": ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "Z": ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
}


def text(canvas, string, x, y, color=(224, 228, 219)):
    for chr_index, chr in enumerate(string):
        rows = FONT_5X7.get(chr)
        if not rows:
            continue
        for row_index, row in enumerate(rows):
            for col, bit in enumerate(row):
                if bit == "1":
                    canvas.fill_rect(x + chr_index * 7 + col, y + row_index + 1,
                                     x + chr_index * 7 + col, y + row_index + 1,
                                     color)


CHAR_WIDTH = {"i": 3, ":": 3, ".": 3, " ": 3, "'": 3, ",": 3,
              "l": 4, "!": 4, "t": 5, "k": 6}


def string_width(s):
    """menu.c's charWidth, so a legend measures the same here as on screen."""
    return sum(CHAR_WIDTH.get(c, 7) for c in s)


def legend(styles):
    """drawWorldSetup's control legend, both phases, from menu.c's own table."""
    menu = open(os.path.join(ROOT, "src", "menu.c")).read()
    d = {}
    for m in re.finditer(r"^#define (SETUP_STRIP_\w+|LEGEND_\w+|"
                         r"WORLD_SETUP_LEGEND_Y)\s+(\d+)$", menu, re.M):
        d[m.group(1)] = int(m.group(2))
    table = re.search(r"world_setup_legend\[\]\s*=\s*\{(.*?)\};", menu, re.S)
    entries = re.findall(r"\{\s*BUTTON_ICON_(\w+)\s*,\s*\"([^\"]*)\"\s*\}",
                         table.group(1))
    names = {"A": "button_a", "B": "button_b", "START": "button_start",
             "L": "button_l", "R": "button_r", "Z": "button_z"}

    widths = [styles[names[i]]["width"] + d["LEGEND_ICON_GAP"] +
              string_width(label) for i, label in entries]
    total = sum(widths) + d["LEGEND_ENTRY_GAP"] * (len(entries) - 1)
    x0 = (d["SETUP_STRIP_LEFT"] + d["SETUP_STRIP_RIGHT"] - total) // 2

    top = d["SETUP_STRIP_TOP"]
    canvas = Canvas(320, d["SETUP_STRIP_BOTTOM"] - top + 4,
                    background=(46, 50, 43))
    canvas.fill_rect(d["SETUP_STRIP_LEFT"], 0, d["SETUP_STRIP_RIGHT"],
                     d["SETUP_STRIP_BOTTOM"] - top, (28, 31, 27))

    x = x0
    y = d["WORLD_SETUP_LEGEND_Y"] - top
    for (icon, label), width in zip(entries, widths):
        style = styles[names[icon]]
        draw_button(canvas, style, x,
                    y + (d["LEGEND_ROW_HEIGHT"] - style["height"]) // 2)
        text(canvas, label, x + style["width"] + d["LEGEND_ICON_GAP"],
             y + d["LEGEND_LABEL_DROP"], color=(150, 155, 142))
        x += width + d["LEGEND_ENTRY_GAP"]
    return canvas


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--guides", action="store_true")
    ap.add_argument("--legend", action="store_true",
                    help="the world setup screen's control legend")
    ap.add_argument("--crt", action="store_true")
    ap.add_argument("--zoom", type=int, default=6)
    ap.add_argument("-o", "--out", default="buttons.png")
    args = ap.parse_args()

    styles = load_styles()
    if args.guides:
        canvas = guides(styles)
    elif args.legend:
        canvas = legend(styles)
    else:
        canvas = sheet(styles)
    img = Image.fromarray(canvas.px)
    if args.crt:
        img = crt(img)
    img = zoom(img, args.zoom)
    img.save(args.out)
    print("%s  %dx%d" % (args.out, img.width, img.height))


if __name__ == "__main__":
    main()
