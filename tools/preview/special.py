#!/usr/bin/env python3
"""Frame a sword special from the player's own first-person eye, and write a PNG.

    tools/preview/special.py
    tools/preview/special.py iron --strip -o build/preview/wave.png

buildSpecialFlash lands a translucent plate in front of a camera sitting at
PLAYER_EYE_HEIGHT, which makes it two questions rather than one: does the plate
leave the crosshair clear, and does it stay inside the frustum at all?  Both
were answered wrongly on the first attempt -- a flat cleave read as a grey
floor across the whole view, and a shockwave centred on the player put its
brightest part directly under the camera.

Every dimension comes from the SPECIAL_* defines in graphics.c and the eye
height and duration from their own headers, so retuning the effect there
changes the picture here.  What it cannot show is alpha: render.py rasterises
opaque, so the plate is drawn at full strength and the picture is the worst
case for coverage.  The real one peaks at SPECIAL_FLASH_ALPHA in the middle and
reaches zero all the way round the rim.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

from render import Geometry, Mesh, Scene, define, filmstrip  # noqa: E402

GRAPHICS = "src/graphics.c"

BLOCK = define("BLOCK_SIZE", "include/graphics.h")
EYE = define("PLAYER_EYE_HEIGHT", "include/player.h")
DURATION = define("MOB_SPECIAL_EFFECT_DURATION", "include/mobs.h")


def _plate(prefix, upright, scale_prefix, rgb):
    return (
        upright,
        define("SPECIAL_%s_SIDE" % prefix, GRAPHICS),
        define("SPECIAL_%s_SPAN" % prefix, GRAPHICS),
        define("SPECIAL_%s_REACH" % prefix, GRAPHICS),
        define("SPECIAL_%s_HEIGHT" % prefix, GRAPHICS),
        define("SPECIAL_%s_SCALE_MIN" % scale_prefix, GRAPHICS),
        define("SPECIAL_%s_SCALE_SPAN" % scale_prefix, GRAPHICS),
        rgb,
    )


# The three branches of buildSpecialFlash, with its own colours.
KINDS = {
    "wood": _plate("RUSH", True, "BLADE", (238, 216, 170)),
    "stone": _plate("CLEAVE", True, "BLADE", (214, 222, 234)),
    "iron": _plate("WAVE", False, "WAVE", (172, 226, 248)),
}


def plate(geom, kind, time_left):
    upright, half_side, half_span, reach, height, base, span, rgb = KINDS[kind]
    # 0 the frame it fires, 1 as it disappears -- buildSpecialFlash's progress.
    progress = 1.0 - time_left / DURATION
    scale = base + span * progress
    half_side *= BLOCK * scale
    half_span *= BLOCK * scale
    reach *= BLOCK
    height *= BLOCK

    # Framed at yaw 0, where forward is -Z and the side axis is +X.
    verts = []
    grid = (-1.0, 0.0, 1.0)
    for i in range(9):
        col, row = i % 3, i // 3
        side = grid[col] * half_side
        offset = grid[row] * half_span
        if upright:
            verts.append((side, height + offset, -reach) + rgb)
        else:
            verts.append((side, height, -(reach + offset)) + rgb)
    return Mesh(verts, geom.lists["shadow_blob_display_list"])


def frame(geom, kind, time_left):
    scene = Scene(background=(96, 140, 200))
    # The pass clears G_CULL_BACK: a plate has to be visible from either side.
    scene.cull = False
    scene.ground(y=0)
    # A body a little past two blocks out, so the plate is judged against
    # something the player would be swinging at rather than against sky.
    scene.add(geom.mesh("zombie_body_verts"), None, (0, 0, -BLOCK * 2.2))
    scene.add(plate(geom, kind, time_left))
    # Level gaze from exactly where the camera sits.
    scene.eye = np.array([0.0, EYE * BLOCK, 0.0])
    scene.center = np.array([0.0, EYE * BLOCK, -1.0])
    return scene.image_with_crosshair()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", nargs="?", choices=sorted(KINDS),
                        help="one tier, or every tier when omitted")
    parser.add_argument("--at", type=float, default=None,
                        help="fraction of the effect remaining, 1 is the "
                             "frame it fires")
    parser.add_argument("-o", "--output",
                        default="build/preview/special-flash.png")
    args = parser.parse_args()

    geom = Geometry.load()
    kinds = [args.kind] if args.kind else sorted(KINDS)
    fractions = [args.at] if args.at is not None else [1.0, 0.5]

    images, labels = [], []
    for kind in kinds:
        for fraction in fractions:
            images.append(frame(geom, kind, DURATION * fraction))
            labels.append("%s %d%% left" % (kind, 100 * fraction))

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    if len(images) == 1:
        images[0].save(args.output)
    else:
        filmstrip(images, args.output, labels=labels)
    print(args.output)


if __name__ == "__main__":
    main()
