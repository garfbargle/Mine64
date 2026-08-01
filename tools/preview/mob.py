#!/usr/bin/env python3
"""Pose one of Mine64's animals and write a PNG, in about a second.

    tools/preview/mob.py pig --head-yaw 40
    tools/preview/mob.py sheep --graze --walk 0.5 --camera-yaw -40
    tools/preview/mob.py pig --strip head-turn -o build/preview/turn.png

The parts, joints and matrices are assembled exactly as drawQuadrupedMob does,
and the joint offsets are read out of graphics.c's QuadrupedModel initialisers
rather than copied here, so a change to the model shows up in the picture
instead of quietly disagreeing with it.
"""

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL.Image import NEAREST  # noqa: E402

from render import ROOT, Geometry, Scene, define, filmstrip, rpy  # noqa: E402

# QuadrupedModel's trailing scalars, in declaration order.
JOINTS = ["neck_y", "neck_z", "hip_x", "hip_y", "front_hip_z", "back_hip_z",
          "stride", "graze_pitch"]


class Quadruped(object):
    def __init__(self, geom, name):
        fields = geom.fields(name + "_model")
        self.geom = geom
        self.name = name
        self.face_list = fields[5]
        self.face_count = int(fields[6])
        self.joint = dict(zip(JOINTS, [int(v) for v in fields[7:15]]))

    def pose(self, scene, yaw=0.0, head_yaw=0.0, graze=0.0, walk=0.0,
             scale=1.0, detailed=True):
        """drawQuadrupedMob: four rotations, seven anchors, one body."""
        g, j = self.geom, self.joint
        swing = math.sin(walk * 2 * math.pi) * j["stride"]
        graze_pitch = j["graze_pitch"] * graze

        body = rpy(0, yaw, 0, scale)
        head = rpy(graze_pitch, yaw + head_yaw, 0, scale)
        limb_a = rpy(swing, yaw, 0, scale)
        limb_b = rpy(-swing, yaw, 0, scale)

        def anchor(offset):
            """setMobPartTransform: the offset turns with the body, then moves."""
            x, y, z = [v * scale for v in offset]
            a = -yaw * math.pi / 180.0
            return (x * math.cos(a) + z * math.sin(a), y,
                    -x * math.sin(a) + z * math.cos(a))

        box = "steve_box_display_list"
        scene.add(g.mesh(self.name + "_body_verts", box), body, anchor((0, 0, 0)))
        neck = anchor((0, j["neck_y"], j["neck_z"]))
        scene.add(g.mesh(self.name + "_head_verts", box), head, neck)
        if detailed:
            scene.add(g.mesh(self.name + "_face_verts", self.face_list,
                             self.face_count), head, neck)
        for x, z, rot in ((-j["hip_x"], j["front_hip_z"], limb_a),
                          (j["hip_x"], j["front_hip_z"], limb_b),
                          (-j["hip_x"], j["back_hip_z"], limb_b),
                          (j["hip_x"], j["back_hip_z"], limb_a)):
            scene.add(g.mesh(self.name + "_leg_verts", box), rot,
                      anchor((x, j["hip_y"], z)))
        if detailed:
            scene.add(g.mesh(self.name + "_tail_verts", box), body,
                      anchor((0, 0, 0)))
        return scene


def frame(model, camera_yaw=25.0, distance=175.0, pitch=12.0, **pose):
    """camera_yaw 0 stands in front of the animal -- its front is -Z."""
    scene = Scene()
    # drawMobsForPlayer clears G_CULL_BACK, which is what lets a face sheet's
    # eyes and ears show whichever way their quads happen to be wound.
    scene.cull = False
    scene.ground(y=0)
    model.pose(scene, **pose)
    scene.look_at((0, 40, 0), distance=distance, yaw=camera_yaw + 180.0,
                  pitch=pitch)
    return scene.image()


def head_sweep():
    """The clamp mobs.c actually enforces, so the strip shows the real limit."""
    limit = define("MOB_HEAD_YAW_LIMIT")
    return [-limit, -limit / 2, 0.0, limit / 2, limit]


STRIPS = {
    # The head-turn from mobs.c: an animal tracks a held apple up to the clamp,
    # and its body keeps pointing where it was already going.
    "head-turn": lambda m: (
        [frame(m, head_yaw=h) for h in head_sweep()],
        ["head %+.0f" % h for h in head_sweep()]),
    "walk": lambda m: (
        [frame(m, walk=p / 5.0, yaw=20) for p in range(5)],
        ["phase %d/5" % p for p in range(5)]),
    "graze": lambda m: (
        [frame(m, graze=g / 4.0) for g in range(5)],
        ["graze %d%%" % (g * 25) for g in range(5)]),
    "turnaround": lambda m: (
        [frame(m, camera_yaw=a) for a in (0, 45, 90, 135, 180)],
        ["camera %d deg" % a for a in (0, 45, 90, 135, 180)]),
}


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mob", nargs="?", default="pig", choices=["pig", "sheep"])
    p.add_argument("-o", "--out", default=None, help="PNG to write")
    p.add_argument("--head-yaw", type=float, default=0.0)
    p.add_argument("--graze", nargs="?", type=float, const=1.0, default=0.0,
                   help="0..1 of the model's graze_pitch")
    p.add_argument("--walk", type=float, default=0.0, help="stride phase, 0..1")
    p.add_argument("--yaw", type=float, default=0.0, help="which way it faces")
    p.add_argument("--scale", type=float, default=1.0, help="1.0 adult, ~0.6 calf")
    p.add_argument("--camera-yaw", type=float, default=25.0,
                   help="0 is head on, 90 is broadside")
    p.add_argument("--distance", type=float, default=175.0)
    p.add_argument("--plain", action="store_true", help="drop face and tail")
    p.add_argument("--strip", choices=sorted(STRIPS), help="filmstrip instead")
    p.add_argument("--scale-up", type=int, default=2,
                   help="zoom factor; 320x240 is the real framebuffer")
    args = p.parse_args()

    model = Quadruped(Geometry.load(), args.mob)
    out = args.out or os.path.join(ROOT, "build", "preview",
                                   "%s-%s.png" % (args.mob,
                                                  args.strip or "pose"))
    if not os.path.isdir(os.path.dirname(out)):
        os.makedirs(os.path.dirname(out))

    if args.strip:
        images, labels = STRIPS[args.strip](model)
        filmstrip(images, out, labels, scale=args.scale_up)
    else:
        image = frame(model, camera_yaw=args.camera_yaw,
                      distance=args.distance, yaw=args.yaw,
                      head_yaw=args.head_yaw, graze=args.graze,
                      walk=args.walk, scale=args.scale,
                      detailed=not args.plain)
        if args.scale_up > 1:
            image = image.resize((image.width * args.scale_up,
                                  image.height * args.scale_up), NEAREST)
        image.save(out)
    print(out)


if __name__ == "__main__":
    main()
