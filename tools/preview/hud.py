#!/usr/bin/env python3
"""Draw the health and food meters on this machine, without a console.

The two meters are pixel sprites made of axis-aligned fill rectangles, and
their placement is arithmetic over the item bar's edges -- neither of which
needs an N64 to evaluate.  This reads the span tables, the colours and the
hotbar constants straight out of src/graphics.c and replays the same fill
rectangles onto a bitmap, so the picture is the sprite the ROM draws rather
than a drawing of what it is supposed to be.

    tools/preview/hud.py                      # full health and food
    tools/preview/hud.py --health 13 --food 7
    tools/preview/hud.py --compact            # the four-player sprites
    tools/preview/hud.py --sheet              # every value, zoomed

What it does not reproduce: the VI's anti-alias and de-flicker filters, and
composite video.  `--crt` fakes the latter well enough to judge whether a
1px outline survives; it is a sanity check, not the television.
"""

import argparse
import os
import re

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GRAPHICS_C = os.path.join(ROOT, "src", "graphics.c")

SCREEN_WD, SCREEN_HT = 320, 240


def _source():
    with open(GRAPHICS_C) as handle:
        return handle.read()


def _define(src, name):
    m = re.search(r"^#define\s+%s\s+(\d+)" % name, src, re.M)
    if not m:
        raise SystemExit("no #define %s in graphics.c" % name)
    return int(m.group(1))


def _spans(src, name):
    """Pull one `static const HudSpan <name>[] = { ... }` out of the source."""
    m = re.search(
        r"static const HudSpan\s+%s\s*\[\]\s*=\s*\{(.*?)\};" % name, src, re.S
    )
    if not m:
        raise SystemExit("no HudSpan table named %s in graphics.c" % name)
    return [tuple(int(v) for v in row) for row in
            re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}",
                       m.group(1))]


def _style(src, name):
    """Pull one `static const HudMeterStyle <name> = { ... }` apart."""
    m = re.search(
        r"static const HudMeterStyle\s+%s\s*=\s*\{(.*?)\};" % name, src, re.S
    )
    if not m:
        raise SystemExit("no HudMeterStyle named %s in graphics.c" % name)
    body = m.group(1)
    head = body.split("{", 1)[0]
    names = re.findall(r"[a-z_]+_spans", head)
    colors = [tuple(int(v) for v in c) for c in
              re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", body)]
    # outline_spans, inner_spans, width, height, pitch
    counts = [int(v) for v in re.findall(r"\d+", head)][:5]
    return {
        # A compact style passes NULL for its outline and draws one
        # silhouette in two colours, so only the inner table is named.
        "outline": _spans(src, names[0]) if counts[0] > 0 else [],
        "inner": _spans(src, names[-1]),
        "outline_spans": counts[0],
        "inner_spans": counts[1],
        "width": counts[2],
        "height": counts[3],
        "pitch": counts[4],
        "outline_color": colors[0],
        "empty_color": colors[1],
        "fill_color": colors[2],
    }


def rgba5551(color):
    """The framebuffer keeps five bits a channel; quantise like the RDP."""
    return tuple((c >> 3) * 255 // 31 for c in color)


class Canvas:
    def __init__(self, width, height, background=(96, 132, 186)):
        self.px = np.zeros((height, width, 3), np.uint8)
        self.px[:, :] = background

    def fill_rect(self, x0, y0, x1, y1, color):
        """gDPFillRectangle: inclusive on both corners."""
        h, w = self.px.shape[:2]
        x0, y0 = max(0, x0), max(0, y0)
        x1, y1 = min(w - 1, x1), min(h - 1, y1)
        if x1 >= x0 and y1 >= y0:
            self.px[y0:y1 + 1, x0:x1 + 1] = rgba5551(color)


def draw_spans(canvas, spans, count, x, y, clip_width, color):
    for span in spans[:count]:
        x0, y0, x1, y1 = span
        if x0 >= clip_width:
            continue
        if x1 >= clip_width:
            x1 = clip_width - 1
        canvas.fill_rect(x + x0, y + y0, x + x1, y + y1, color)


def draw_meter(canvas, style, x, y, value, max_value):
    units = max_value // 2
    for i in range(units):
        draw_spans(canvas, style["outline"], style["outline_spans"],
                   x + i * style["pitch"], y, style["width"],
                   style["outline_color"])
    for i in range(units):
        if value < (i + 1) * 2:
            draw_spans(canvas, style["inner"], style["inner_spans"],
                       x + i * style["pitch"], y, style["width"],
                       style["empty_color"])
    for i in range(units):
        unit = min(2, value - i * 2) if value > i * 2 else 0
        if unit > 0:
            draw_spans(canvas, style["inner"], style["inner_spans"],
                       x + i * style["pitch"], y,
                       style["width"] if unit == 2
                       else (style["width"] + 1) // 2,
                       style["fill_color"])


def meter_width(style, max_value):
    return (max_value // 2 - 1) * style["pitch"] + style["width"]


class Hud:
    """The layout arithmetic of drawHealth and drawHotbar, on constants read
    out of graphics.c so a change to either shows up here."""

    def __init__(self, compact=False):
        src = _source()
        self.compact = compact
        self.slot_count = _define(src, "HOTBAR_SLOT_COUNT") if re.search(
            r"#define\s+HOTBAR_SLOT_COUNT\s+\d", src) else 9
        self.slot_size = 14 if compact else _define(src, "HOTBAR_SLOT_SIZE")
        self.margin = 4 if compact else _define(src, "HOTBAR_MARGIN")
        self.max_health = self._player_define("PLAYER_MAX_HEALTH")
        self.max_hunger = self._player_define("PLAYER_MAX_HUNGER")
        suffix = "_compact_style" if compact else "_style"
        self.health = _style(src, "health" + suffix)
        self.food = _style(src, "food" + suffix)
        self.special_w = _define(src, "SPECIAL_BAR_WIDTH")
        self.special_h = _define(src, "SPECIAL_BAR_HEIGHT")
        self.view_w = SCREEN_WD // 2 if compact else SCREEN_WD
        self.view_h = SCREEN_HT // 2 if compact else SCREEN_HT
        self.bar_width = self.slot_count * self.slot_size
        self.bar_x = (self.view_w - self.bar_width) // 2
        self.bar_y = self.view_h - self.slot_size - self.margin

    def _player_define(self, name):
        with open(os.path.join(ROOT, "include", "player.h")) as handle:
            return _define(handle.read(), name)

    def draw_special(self, canvas, charge, y):
        """drawSpecialCharge: the sword bar between the two meters.

        Its whole risk is horizontal -- the meters grow from both ends of the
        same row, and this fills what is left.  Drawing it here is what makes a
        collision visible instead of shipping."""
        if self.compact:
            return  # drawHealth suppresses it on the four-way split.
        x = self.bar_x + self.bar_width // 2 - self.special_w // 2
        y += (self.health["height"] - self.special_h) // 2
        canvas.fill_rect(x - 1, y - 1, x + self.special_w,
                         y + self.special_h, (10, 14, 18))
        canvas.fill_rect(x, y, x + self.special_w - 1,
                         y + self.special_h - 1, (34, 46, 55))
        filled = int(charge * self.special_w)
        if filled:
            ready = charge >= 1.0
            canvas.fill_rect(x, y, x + filled - 1, y + self.special_h - 1,
                             (214, 240, 252) if ready else (84, 138, 166))

    def draw(self, canvas, health, hunger, y_base=0, charge=None):
        bar_y = self.bar_y + y_base
        # The item bar, drawn the way drawHotbar draws it, for context.
        canvas.fill_rect(self.bar_x - 2, bar_y - 2, self.bar_x + self.bar_width + 1,
                         bar_y + self.slot_size + 1, (20, 20, 20))
        for slot in range(self.slot_count):
            x = self.bar_x + slot * self.slot_size
            selected = slot == 2
            tone = 250 if selected else 78
            inner = 118 if selected else 42
            canvas.fill_rect(x, bar_y, x + self.slot_size - 1,
                             bar_y + self.slot_size - 1, (tone, tone, tone))
            canvas.fill_rect(x + 2, bar_y + 2, x + self.slot_size - 3,
                             bar_y + self.slot_size - 3, (inner, inner, inner))

        y = bar_y - 2 - 2 - self.health["height"]
        draw_meter(canvas, self.health, self.bar_x - 2, y, health,
                   self.max_health)
        draw_meter(canvas, self.food,
                   self.bar_x + self.bar_width + 1
                   - meter_width(self.food, self.max_hunger) + 1,
                   y, hunger, self.max_hunger)
        if charge is not None:
            self.draw_special(canvas, charge, y)


def crt(img):
    """A crude composite pass: full-bandwidth luma, chroma smeared sideways."""
    a = np.asarray(img).astype(np.float32)
    y = a @ np.array([0.299, 0.587, 0.114], np.float32)
    cb, cr = a[:, :, 2] - y, a[:, :, 0] - y
    k = np.array([0.15, 0.2, 0.3, 0.2, 0.15], np.float32)
    for ch in (cb, cr):
        pad = np.pad(ch, ((0, 0), (2, 2)), mode="edge")
        ch[:] = sum(k[i] * pad[:, i:i + ch.shape[1]] for i in range(5))
    ky = np.array([0.25, 0.5, 0.25], np.float32)
    pad = np.pad(y, ((0, 0), (1, 1)), mode="edge")
    y = sum(ky[i] * pad[:, i:i + y.shape[1]] for i in range(3))
    out = np.stack([y + cr, y - 0.344 * cb - 0.714 * cr, y + cb], -1)
    return Image.fromarray(out.clip(0, 255).astype(np.uint8))


def zoom(img, scale):
    return img.resize((img.width * scale, img.height * scale), Image.NEAREST)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--health", type=int, default=20)
    ap.add_argument("--food", type=int, default=20)
    ap.add_argument("--charge", type=float, default=None,
                    help="sword charge 0..1; omit for no sword in hand")
    ap.add_argument("--compact", action="store_true",
                    help="the four-player sprites, in a quarter viewport")
    ap.add_argument("--sheet", action="store_true",
                    help="a column of values instead of one")
    ap.add_argument("--crt", action="store_true",
                    help="fake composite video over the result")
    ap.add_argument("--zoom", type=int, default=3)
    ap.add_argument("-o", "--out", default="hud.png")
    args = ap.parse_args()

    hud = Hud(compact=args.compact)
    strip_h = hud.slot_size + hud.margin + 24

    if args.sheet:
        rows = [(20, 20, 1.0), (17, 20, .75), (13, 11, .4), (7, 5, .1),
                (1, 0, 0.0), (0, 0, None)]
        canvas = Canvas(hud.view_w, strip_h * len(rows))
        for i, (health, hunger, charge) in enumerate(rows):
            hud.draw(canvas, health, hunger,
                     y_base=strip_h * i - hud.bar_y + strip_h - hud.slot_size
                     - hud.margin, charge=charge)
    else:
        canvas = Canvas(hud.view_w, strip_h)
        hud.draw(canvas, args.health, args.food,
                 y_base=strip_h - hud.view_h, charge=args.charge)

    img = Image.fromarray(canvas.px)
    if args.crt:
        img = crt(img)
    img = zoom(img, args.zoom)
    img.save(args.out)
    print("%s  %dx%d" % (args.out, img.width, img.height))


if __name__ == "__main__":
    main()
