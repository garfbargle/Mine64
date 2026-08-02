#!/usr/bin/env python3
"""Draw 64MON creatures from the roster tables, the way drawCreature does.

    tools/preview/mon.py embear
    tools/preview/mon.py --family fire
    tools/preview/mon.py --roster
    tools/preview/mon.py --audit

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

from render import ROOT, Geometry, Mesh, Scene, filmstrip  # noqa: E402

DATA = os.path.join(ROOT, "src", "mon64_data.c")

# The same triangle list drawCreature hands the RSP for every box.
BOX_TRIS = Geometry.load().lists["box_display_list"]


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
    """buildBox, vertex for vertex.

    The +Z face carries the species colour and the -Z face a darkened copy:
    there is no RSP light in the entity pass, so a box carries its own sense
    of which way is toward the viewer.  Shading it top-to-bottom instead --
    which is what a preview reaches for by reflex -- puts the light on a face
    the console never lights, and then the picture and the hardware disagree
    about which side of an animal you are looking at.
    """
    dark = tuple(c * 180 // 255 for c in color)
    x0, y0, z0 = cx - sx, cy - sy, cz - sz
    x1, y1, z1 = cx + sx, cy + sy, cz + sz
    v = [(x0, y1, z1) + tuple(color), (x1, y1, z1) + tuple(color),
         (x1, y0, z1) + tuple(color), (x0, y0, z1) + tuple(color),
         (x1, y1, z0) + dark, (x0, y1, z0) + dark,
         (x0, y0, z0) + dark, (x1, y0, z0) + dark]
    return Mesh(v, BOX_TRIS)


def build(scene, species, rigs, defines, origin_x=0.0):
    """drawCreature: scale the extents, gate on maturity, place the box.

    Width and depth take scale *and* bulk, in the extents and in the offsets
    alike -- poseOffset does the same, and it has to: an offset left on plain
    scale puts a part where the unbulked body used to be, which is inside the
    bulked one.  Height takes scale only, in both places, because an elder is
    heavier rather than taller.
    """
    rig = rigs[defines[species["rig"]]]
    scale, bulk, maturity = species["scale"], species["bulk"], \
        species["maturity"]
    wide = scale * bulk / 10000.0
    for part in rig["parts"]:
        if part["stage"] > maturity:
            continue
        sx = max(1, part["sx"] * scale * bulk // 10000)
        sy = max(1, part["sy"] * scale // 100)
        sz = max(1, part["sz"] * scale * bulk // 10000)
        color = species[TONE[part["tone"]]]
        scene.add(box(origin_x + part["x"] * wide,
                      part["y"] * scale / 100.0,
                      part["z"] * wide, sx, sy, sz, color))
    return rig


def render(species, rigs, defines, yaw=30.0, distance=190.0, size=(160, 140)):
    scene = Scene(width=size[0], height=size[1])
    rig = build(scene, species, rigs, defines)
    top = rig["height"] * species["scale"] / 100.0
    scene.ground(0)
    scene.look_at((0, top * 0.55, 0), distance=distance, yaw=yaw, pitch=14)
    return scene.image()


def audit(rigs, species, defines):
    """The roster invariants that are invisible in the table and in the game.

    Every one of these has been wrong at some point, and none of them showed
    up as anything a player could name: a move levelled past its own species'
    evolution is simply a move nobody ever has, and an eye box swallowed by a
    bulked body is a creature that just looks slightly wrong.  Cheap to check
    here, so there is no reason to find out on hardware.
    """
    bad = []
    by_id = {i: s for i, s in enumerate(species)}

    def parent(index):
        for j, s in enumerate(species):
            if s["evolves_to"] == "SP_" + by_id[index]["name"]:
                return j
        return None

    for i, s in enumerate(species):
        p = parent(i)
        floor = species[p]["evolve_level"] if p is not None else 0

        # A move learned after the species has already evolved away.
        limit = s["evolve_level"] if s["evolves_to"] != "MON_NONE" else 50
        for move, level in zip(s["moves"], s["move_levels"]):
            if level > limit:
                bad.append("%s learns %s at %d but evolves at %d"
                           % (s["name"], move.replace("MV_", ""), level, limit))

        # Reachability.  A middle stage may carry a weight -- pickSpecies
        # gates it on the pad's rolled level against exactly this `floor`, so
        # a GRIZZLE is findable once you could have raised one and not
        # before.  What must not happen is a family with no way in at all, or
        # a final stage standing in a field it was supposed to be too rare
        # for.
        if s["spawn_weight"] == 0 and floor == 0:
            bad.append("%s is a young form that never spawns: its family has "
                       "no way in" % s["name"])
        if s["spawn_weight"] and s["evolves_to"] == "MON_NONE":
            bad.append("%s is a final stage and must not spawn wild"
                       % s["name"])

        # Geometry: nothing inside anything, and feet on the floor.
        rig = rigs[defines[s["rig"]]]
        wide = s["scale"] * s["bulk"] / 10000.0
        boxes = []
        for j, part in enumerate(rig["parts"]):
            if part["stage"] > s["maturity"]:
                continue
            sx = max(1, part["sx"] * s["scale"] * s["bulk"] // 10000)
            sy = max(1, part["sy"] * s["scale"] // 100)
            sz = max(1, part["sz"] * s["scale"] * s["bulk"] // 10000)
            cx, cy, cz = part["x"] * wide, part["y"] * s["scale"] / 100.0, \
                part["z"] * wide
            boxes.append((j, (cx - sx, cx + sx), (cy - sy, cy + sy),
                          (cz - sz, cz + sz)))
        for j, ax, ay, az in boxes:
            for k, bx, by, bz in boxes:
                if j != k and all(lo <= a[0] and a[1] <= hi for a, (lo, hi)
                                  in ((ax, bx), (ay, by), (az, bz))):
                    bad.append("%s part %d is entirely inside part %d"
                               % (s["name"], j, k))
        feet = min(b[2][0] for b in boxes)
        if feet > 1.5 or feet < -0.5:
            bad.append("%s does not stand on the ground (feet at %+.1f)"
                       % (s["name"], feet))

    for name, rig in zip(("bear", "iguana", "frog", "eel", "owl", "ape"), rigs):
        if rig["part_count"] != len(rig["parts"]):
            bad.append("%s rig declares %d parts and lists %d"
                       % (name, rig["part_count"], len(rig["parts"])))
        young = [p for p in rig["parts"] if p["stage"] == 1]
        top = max(p["y"] + p["sy"] for p in young)
        if abs(top - rig["height"]) > 2:
            bad.append("%s rig height is %d, young silhouette tops at %d"
                       % (name, rig["height"], top))

    for line in bad:
        print("  " + line)
    print("%d problem%s" % (len(bad), "" if len(bad) == 1 else "s"))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name", nargs="?")
    ap.add_argument("--audit", action="store_true",
                    help="check the roster invariants and exit")
    ap.add_argument("--family", help="rig name, e.g. RIG_QUADRUPED or 'bear'")
    ap.add_argument("--roster", action="store_true")
    ap.add_argument("--yaw", type=float, default=30.0)
    ap.add_argument("--distance", type=float, default=190.0)
    ap.add_argument("-o", "--out", default="build/preview/mon.png")
    args = ap.parse_args()

    defines = load_defines()
    rigs, species = load_rigs(), load_species()
    by_name = {s["name"].lower(): s for s in species}

    if args.audit:
        raise SystemExit(audit(rigs, species, defines))

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
