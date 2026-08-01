#!/usr/bin/env python3
"""Draw 64MON creatures from the roster tables, the way drawCreature does.

    tools/preview/mon.py embear
    tools/preview/mon.py --family fire
    tools/preview/mon.py --roster

A rig is boxes rather than a Vtx array, so this reads mon64_data.c's own
initialisers -- rig parts and species rows both -- and applies the same
scale/bulk/maturity arithmetic drawCreature uses.  A change to the table shows
up in the picture instead of quietly disagreeing with it.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from render import ROOT, Mesh, Scene, filmstrip  # noqa: E402

DATA = os.path.join(ROOT, "src", "mon64_data.c")


def _rows(text):
    """Split a brace-list body into its top-level {...} rows."""
    out, depth, start = [], 0, None
    for i, ch in enumerate(text):
        if ch == "{":
            if depth == 0:
                start = i + 1
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                out.append(text[start:i])
    return out


def _body(name):
    src = open(DATA).read()
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    i = src.index(name)
    i = src.index("{", i)
    depth, j = 0, i
    while True:
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i + 1:j]
        j += 1


def load_defines():
    src = open(DATA).read()
    return {n: int(v) for n, v in
            re.findall(r"^#define\s+(SP_\w+|RIG_\w+|MV_\w+)\s+(\d+)", src, re.M)}


def load_rigs():
    rigs = []
    for row in _rows(_body("mon_rigs")):
        head = row[:row.index("{")]
        count, height = [int(v) for v in re.findall(r"-?\d+", head)[:2]]
        parts = []
        for part in _rows(_rows(row[row.index("{"):])[0]):
            f = [t.strip() for t in part.split(",")]
            parts.append(dict(
                x=int(f[0]), y=int(f[1]), z=int(f[2]),
                sx=int(f[3]), sy=int(f[4]), sz=int(f[5]),
                tone=f[6], role=f[7], stage=int(f[8])))
        assert len(parts) == count, (count, len(parts))
        rigs.append(dict(part_count=count, height=height, parts=parts))
    return rigs


def load_species():
    out = []
    for row in _rows(_body("mon_species")):
        name = re.match(r'\s*"(\w+)"', row).group(1)
        rest = row[row.index('"', row.index('"') + 1) + 1:]
        colors = [[int(v) for v in re.findall(r"-?\d+", c)]
                  for c in _rows(rest)]
        scalars = [t.strip() for t in
                   re.sub(r"\{[^{}]*\}", "", rest).split(",") if t.strip()]
        out.append(dict(
            name=name, rig=scalars[0], type=scalars[1],
            scale=int(scalars[2]), bulk=int(scalars[3]),
            maturity=int(scalars[4]),
            base_hp=int(scalars[5]), base_attack=int(scalars[6]),
            base_defense=int(scalars[7]), base_speed=int(scalars[8]),
            catch_rate=int(scalars[9]), xp_yield=int(scalars[10]),
            spawn_weight=int(scalars[11]),
            habitat=scalars[12], evolves_to=scalars[13],
            evolve_level=int(scalars[14]), aggressive=scalars[15],
            primary=colors[0], secondary=colors[1], accent=colors[2],
            moves=[t.strip() for t in _rows(rest)[3].split(",")],
            move_levels=[int(v) for v in re.findall(r"\d+", _rows(rest)[4])]))
    return out


TONE = {"MON_TONE_PRIMARY": "primary",
        "MON_TONE_SECONDARY": "secondary",
        "MON_TONE_ACCENT": "accent"}


def box(cx, cy, cz, sx, sy, sz, color):
    """buildBox: a lit top face and darker sides, like every mob box."""
    light = tuple(min(255, int(c * 1.15)) for c in color)
    dark = tuple(int(c * 0.72) for c in color)
    v = []
    for (x, y, z) in ((-1, 1, -1), (1, 1, -1), (1, 1, 1), (-1, 1, 1),
                      (-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)):
        c = light if y > 0 else dark
        v.append((cx + x * sx, cy + y * sy, cz + z * sz) + c)
    t = [(0, 1, 2), (0, 2, 3), (4, 6, 5), (4, 7, 6),
         (0, 4, 5), (0, 5, 1), (1, 5, 6), (1, 6, 2),
         (2, 6, 7), (2, 7, 3), (3, 7, 4), (3, 4, 0)]
    return Mesh(v, t)


def build(scene, species, rigs, defines, origin_x=0.0):
    """drawCreature: scale the extents, gate on maturity, place the box."""
    rig = rigs[defines[species["rig"]]]
    scale, bulk, maturity = species["scale"], species["bulk"], \
        species["maturity"]
    for part in rig["parts"]:
        if part["stage"] > maturity:
            continue
        sx = max(1, part["sx"] * scale * bulk // 10000)
        sy = max(1, part["sy"] * scale // 100)
        sz = max(1, part["sz"] * scale * bulk // 10000)
        color = species[TONE[part["tone"]]]
        scene.add(box(origin_x + part["x"] * scale / 100.0,
                      part["y"] * scale / 100.0,
                      part["z"] * scale / 100.0, sx, sy, sz, color))
    return rig


def render(species, rigs, defines, yaw=30.0, distance=190.0, size=(160, 140)):
    scene = Scene(width=size[0], height=size[1])
    rig = build(scene, species, rigs, defines)
    top = rig["height"] * species["scale"] / 100.0
    scene.ground(0)
    scene.look_at((0, top * 0.55, 0), distance=distance, yaw=yaw, pitch=14)
    return scene.image()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name", nargs="?")
    ap.add_argument("--family", help="rig name, e.g. RIG_QUADRUPED or 'bear'")
    ap.add_argument("--roster", action="store_true")
    ap.add_argument("--yaw", type=float, default=30.0)
    ap.add_argument("--distance", type=float, default=190.0)
    ap.add_argument("-o", "--out", default="build/preview/mon.png")
    args = ap.parse_args()

    defines = load_defines()
    rigs, species = load_rigs(), load_species()
    by_name = {s["name"].lower(): s for s in species}

    if args.roster or args.family:
        if args.family:
            key = args.family.upper()
            key = key if key.startswith("RIG_") else "RIG_" + key
            picks = [s for s in species if s["rig"] == key]
        else:
            picks = species
        images = [render(s, rigs, defines, args.yaw, args.distance)
                  for s in picks]
        labels = ["%s L%d" % (s["name"], s["evolve_level"]) for s in picks]
    else:
        s = by_name[args.name.lower()]
        images = [render(s, rigs, defines, args.yaw, args.distance)]
        labels = [s["name"]]

    path = os.path.join(ROOT, args.out)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    filmstrip(images, path, labels, scale=2)
    print(path)


if __name__ == "__main__":
    main()
