#!/usr/bin/env python3
"""Pose the first-person hand and whatever it is holding, and write a PNG.

    tools/preview/hand.py iron_sword --reach 0.5
    tools/preview/hand.py wood_axe --strip swing -o build/preview/axe.png

drawFirstPersonHand draws in camera space, so this is not an approximation of
the player's view -- it is the same arithmetic on the same vertices, framed by
the same projection.  Every angle comes from the FP_* defines in graphics.c,
so retuning the swing there changes the picture here.
"""

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL.Image import NEAREST  # noqa: E402

from render import (DTOR, ROOT, Geometry, Scene, define, filmstrip,  # noqa: E402
                    rpy)

GRAPHICS = "src/graphics.c"

# item name -> (vertex array, display list) pairs drawn by drawToolGeometry.
TOOLS = {"none": []}
for _tier in ("wood", "stone", "iron"):
    TOOLS[_tier + "_sword"] = [(_tier + "_sword_blade_verts",
                                "sword_blade_display_list"),
                               (_tier + "_sword_guard_verts",
                                "box_display_list")]
    TOOLS[_tier + "_pickaxe"] = [("tool_handle_verts", "box_display_list"),
                                 (_tier + "_pick_head_verts",
                                  "box_display_list")]
    TOOLS[_tier + "_axe"] = [("tool_handle_verts", "box_display_list"),
                             (_tier + "_axe_head_verts",
                              "box_display_list")]


class Hand(object):
    def __init__(self, geom):
        self.geom = geom
        self.fp = {name: define("FP_" + name, GRAPHICS) for name in (
            "ARM_LENGTH", "ELBOW_X", "ELBOW_Y", "ELBOW_Z", "REST_PITCH",
            "ARM_ROLL", "SWING_PITCH", "SWING_FORWARD", "SWING_INWARD",
            "SWING_RISE", "TOOL_PITCH", "TOOL_YAW", "TOOL_SLASH")}

    def pose(self, scene, item="none", reach=0.0):
        fp, g = self.fp, self.geom
        pitch = fp["REST_PITCH"] + fp["SWING_PITCH"] * reach
        roll = fp["ARM_ROLL"]
        length = fp["ARM_LENGTH"]
        # The elbow is what stays put, so the translation undoes where the
        # rotation about the hand sends it.
        trans = (
            fp["ELBOW_X"] - fp["SWING_INWARD"] * reach +
            length * math.cos(pitch * DTOR) * math.sin(roll * DTOR),
            fp["ELBOW_Y"] + fp["SWING_RISE"] * reach -
            length * math.cos(pitch * DTOR) * math.cos(roll * DTOR),
            fp["ELBOW_Z"] - fp["SWING_FORWARD"] * reach -
            length * math.sin(pitch * DTOR),
        )
        scene.add(g.mesh("first_person_arm_verts"), rpy(pitch, 0, roll), trans)
        tool = rpy(pitch + fp["TOOL_PITCH"], fp["TOOL_YAW"],
                   roll + fp["TOOL_SLASH"] * reach)
        for verts, display_list in TOOLS[item]:
            scene.add(g.mesh(verts, display_list), tool, trans)
        return scene


def frame(hand, item="none", reach=0.0, background=(88, 132, 196)):
    scene = Scene(background=background)
    # drawFirstPersonHand clears G_CULL_BACK for the arm and the tool.
    scene.cull = False
    scene.camera_space()
    hand.pose(scene, item=item, reach=reach)
    return scene.image_with_crosshair()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("item", nargs="?", default="none", choices=sorted(TOOLS))
    p.add_argument("-o", "--out", default=None)
    p.add_argument("--reach", type=float, default=0.0,
                   help="firstPersonReach, 0 at rest to 1 fully extended")
    p.add_argument("--strip", action="store_true",
                   help="the whole swing, left to right")
    p.add_argument("--frames", type=int, default=5)
    p.add_argument("--scale-up", type=int, default=2)
    args = p.parse_args()

    hand = Hand(Geometry.load())
    out = args.out or os.path.join(ROOT, "build", "preview",
                                   "hand-%s.png" % args.item)
    if not os.path.isdir(os.path.dirname(out)):
        os.makedirs(os.path.dirname(out))

    if args.strip:
        reaches = [i / float(args.frames - 1) for i in range(args.frames)]
        filmstrip([frame(hand, args.item, r) for r in reaches], out,
                  ["reach %.2f" % r for r in reaches], scale=args.scale_up)
    else:
        image = frame(hand, args.item, args.reach)
        if args.scale_up > 1:
            image = image.resize((image.width * args.scale_up,
                                  image.height * args.scale_up), NEAREST)
        image.save(out)
    print(out)


if __name__ == "__main__":
    main()
