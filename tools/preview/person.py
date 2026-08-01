#!/usr/bin/env python3
"""Pose one of Mine64's people and write a PNG, in about a second.

    tools/preview/person.py                    the player, three-quarter view
    tools/preview/person.py kalia --walk 0.25  mid-stride
    tools/preview/person.py --everyone         the whole cast, front on
    tools/preview/person.py sam --turn         a filmstrip of a head turning

Players, 64MON's trainers and the villagers are all the same body: the boxes,
the look table and the anchor offsets are read out of src/humanoid.c rather
than copied here, so a change to the model shows up in the picture instead of
quietly disagreeing with it.  The garment colours are applied the way the RDP
applies them -- primitive colour times vertex shade -- which is why the boxes
in the source are white.
"""

import argparse
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from render import ROOT, Geometry, Mesh, Scene, filmstrip, rpy  # noqa: E402

HUMANOID_C = os.path.join(ROOT, "src", "humanoid.c")

# humanoidDraw's anchor offsets, above the ground the feet stand on.
BODY_Y, HEAD_Y, ARM_Y, LEG_Y = 66, 104, 88, 44
ARM_X, LEG_X = 25, 10


def looks():
    """Every HumanoidPerson, plus the player, as (name, colours, style)."""
    src = re.sub(r"/\*.*?\*/", "", open(HUMANOID_C).read(), flags=re.S)
    people = []
    table = re.search(r"humanoid_people\[[^\]]*\]\s*=\s*\{(.*?)\n\};", src, re.S)
    for chunk in table.group(1).split('{"')[1:]:
        name = chunk.split('"')[0]
        triples = [[int(v) for v in re.findall(r"\d+", g)]
                   for g in re.findall(r"\{\s*\d+,\s*\d+,\s*\d+\s*\}", chunk)]
        style = "LONG" if "HAIR_LONG" in chunk else "SHORT"
        people.append((name, triples[:4], style))
    player = re.search(r"humanoid_player_look\s*=\s*\{(.*?)\};", src, re.S)
    triples = [[int(v) for v in re.findall(r"\d+", g)]
               for g in re.findall(r"\{\s*\d+,\s*\d+,\s*\d+\s*\}", player.group(1))]
    people.insert(0, ("PLAYER", triples[:4], "SHORT"))
    return people


def tinted(geom, name, rgb):
    """PRIM * SHADE: the boxes carry shading, the look carries the colour."""
    mesh = geom.mesh(name)
    out = Mesh([(0, 0, 0, 0, 0, 0)], mesh.tris)
    out.verts = mesh.verts
    out.colors = mesh.colors * [c / 255.0 for c in rgb]
    return out


def rotate_y(x, y, z, yaw):
    """rotateY(offset, -body_yaw), as setPartTransform applies it."""
    a = -yaw * math.pi / 180.0
    return (x * math.cos(a) + z * math.sin(a), y,
            -x * math.sin(a) + z * math.cos(a))


def add_person(scene, geom, look, ground=(0, 0, 0), body_yaw=0.0,
               head_yaw=None, head_pitch=0.0, walk=0.0, swing=1.0, bob=0.0):
    """humanoidDraw: eight anchors, two matrices each, one shared body."""
    shirt, trousers, skin, hair = look[1]
    style = look[2]
    head_yaw = body_yaw if head_yaw is None else head_yaw
    step = math.sin(walk) * 28.0 * swing

    def anchor(part, offset, pitch, yaw):
        x, y, z = rotate_y(offset[0], offset[1], offset[2], body_yaw)
        return (rpy(pitch, yaw, 0),
                (ground[0] + x, ground[1] + y, ground[2] + z))

    body = anchor("body", (bob, BODY_Y, 0), 0, body_yaw)
    head = anchor("head", (bob, HEAD_Y, 0), head_pitch, head_yaw)
    hair_anchor = anchor("hair", (bob, HEAD_Y, 0), head_pitch,
                         head_yaw + math.sin(walk * 0.5) * 6.0)
    left_arm = anchor("arm", (-ARM_X + bob, ARM_Y, 0), step, body_yaw)
    right_arm = anchor("arm", (ARM_X + bob, ARM_Y, 0), -step, body_yaw)
    left_leg = anchor("leg", (-LEG_X + bob, LEG_Y, 0), -step, body_yaw)
    right_leg = anchor("leg", (LEG_X + bob, LEG_Y, 0), step, body_yaw)

    scene.add(tinted(geom, "humanoid_body_verts", shirt), *body)
    scene.add(tinted(geom, "humanoid_arm_verts", skin), *left_arm)
    scene.add(tinted(geom, "humanoid_arm_verts", skin), *right_arm)
    scene.add(tinted(geom, "humanoid_leg_verts", trousers), *left_leg)
    scene.add(tinted(geom, "humanoid_leg_verts", trousers), *right_leg)
    scene.add(tinted(geom, "humanoid_head_verts", skin), *head)
    # The face sheet carries its own colours and is drawn under a plain tint.
    scene.add(geom.mesh("steve_face_verts", "steve_face_display_list"), *head)
    if style == "LONG":
        for box in ("humanoid_long_crown_verts", "humanoid_fringe_verts",
                    "humanoid_left_strand_verts", "humanoid_right_strand_verts"):
            scene.add(tinted(geom, box, hair), *head)
        scene.add(tinted(geom, "humanoid_ponytail_verts", hair), *hair_anchor)
    else:
        for box in ("humanoid_crown_verts", "humanoid_nape_verts"):
            scene.add(tinted(geom, box, hair), *head)
    return scene


def scene_for(geom, people, camera_yaw, distance, walk=0.0, body_yaw=0.0,
              head_yaw=None, width=320, height=240):
    scene = Scene(width=width, height=height)
    # Both entity passes clear G_CULL_BACK, which is what lets the face sheet
    # sit on the head instead of vanishing from one side.
    scene.cull = False
    scene.ground(0)
    spread = 78
    left = -spread * (len(people) - 1) / 2.0
    for index, look in enumerate(people):
        add_person(scene, geom, look, ground=(left + index * spread, 0, 0),
                   body_yaw=body_yaw, head_yaw=head_yaw, walk=walk)
    scene.look_at((0, 62, 0), distance=distance, yaw=camera_yaw, pitch=6)
    return scene


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("who", nargs="?", default="player",
                    help="a name from humanoid_people, or 'player'")
    ap.add_argument("--everyone", action="store_true")
    ap.add_argument("--walk", type=float, default=0.0,
                    help="phase of the gait, in radians")
    ap.add_argument("--yaw", type=float, default=0.0, help="which way they face")
    ap.add_argument("--head-yaw", type=float, default=None)
    ap.add_argument("--camera-yaw", type=float, default=160.0,
                    help="160 stands in front of them; 0 is behind")
    ap.add_argument("--distance", type=float, default=240.0)
    ap.add_argument("--turn", action="store_true",
                    help="a filmstrip of the head turning")
    ap.add_argument("-o", default="build/preview/person.png")
    args = ap.parse_args()

    geom = Geometry.load()
    everyone = looks()
    if args.everyone:
        people = everyone
    else:
        wanted = args.who.upper()
        people = [p for p in everyone if p[0] == wanted]
        if not people:
            raise SystemExit("no such person: %s (have %s)" %
                             (args.who, ", ".join(p[0] for p in everyone)))

    path = args.o if os.path.isabs(args.o) else os.path.join(ROOT, args.o)
    directory = os.path.dirname(path)
    if directory and not os.path.isdir(directory):
        os.makedirs(directory)

    if args.turn:
        steps = (-70, -35, 0, 35, 70)
        frames = [scene_for(geom, people, args.camera_yaw, args.distance,
                            body_yaw=args.yaw, head_yaw=args.yaw + step).image()
                  for step in steps]
        filmstrip(frames, path, labels=["%+d" % s for s in steps])
    else:
        distance = args.distance * (2.4 if args.everyone else 1.0)
        scene_for(geom, people, args.camera_yaw, distance, walk=args.walk,
                  body_yaw=args.yaw, head_yaw=args.head_yaw,
                  width=640 if args.everyone else 320,
                  height=300 if args.everyone else 240).image().save(path)
    print(path)


if __name__ == "__main__":
    main()
