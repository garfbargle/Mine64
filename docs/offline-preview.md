# Offline model preview

Most questions about how something looks are questions about arithmetic, and
none of that arithmetic needs an N64. Every model in Mine64 is a handful of
untextured boxes and quads run through `guRotateRPY` and `guPerspective`, so
`tools/preview` reads the real vertex data out of `src/graphics.c`, does the
same maths the RSP would, and writes a PNG:

```sh
tools/preview/mob.py pig --head-yaw 40
```

That takes about a quarter of a second. The same question asked through the
emulator costs a ROM build, a fifty-second title-card preroll, a scripted
timeline that has to walk to whatever it wants to look at, and a world that is
different on every run. Reach for the preview first, and keep the emulator for
what only it can show: the interface in its real framebuffer, and behaviour
that involves the game actually running.

## The three scenes that exist

`mob.py` poses a quadruped exactly as `drawQuadrupedMob` does -- four
rotations, seven anchors -- with the joint offsets read out of the
`QuadrupedModel` initialisers rather than copied, so a model edit shows up in
the picture instead of quietly disagreeing with it.

```sh
tools/preview/mob.py sheep --graze --camera-yaw 90
tools/preview/mob.py pig --strip head-turn      # a filmstrip to build/preview
```

`--strip` takes `head-turn`, `walk`, `graze` or `turnaround`. The head-turn
sweep uses `MOB_HEAD_YAW_LIMIT` from `mobs.c`, so it shows the clamp the game
enforces rather than a number typed in here.

`hand.py` poses the first-person arm and whatever it holds. `drawFirstPersonHand`
draws in camera space, so this is not an approximation of the player's view --
it is the same vertices under the same projection, with every angle taken from
the `FP_*` defines:

```sh
tools/preview/hand.py iron_sword --reach 0.5
tools/preview/hand.py wood_axe --strip
```

`hud.py` is the exception to "the emulator owns the interface". The health and
food meters are pixel sprites made of fill rectangles placed by arithmetic over
the item bar's edges, and none of that needs a running game, so it replays the
same rectangles onto a bitmap -- reading the `HudSpan` tables, the
`HudMeterStyle` colours and the `HOTBAR_*` defines out of `graphics.c`, so the
sprite in the picture is the sprite in the ROM:

```sh
tools/preview/hud.py --health 13 --food 7
tools/preview/hud.py --sheet             # full, half and empty, all at once
tools/preview/hud.py --compact           # the four-player sprites
tools/preview/hud.py --crt               # fake composite over the result
```

`buttons.py` does the same for the controller-button sprites, which are the
same kind of object: fill rectangles from `HudSpan` tables, placed by
arithmetic. `--guides` reassembles the two in-game guide panels from the
`GUIDE_*` defines `drawCButtonGuide`, `drawActionGuide` and `drawGameText`
all quote, which is the picture worth checking -- a button is only good if it
still reads at 1x with a label beside it, and the icons and the labels are
drawn by two different passes fourteen pixels apart in the frame:

```sh
tools/preview/buttons.py                 # every button, zoomed
tools/preview/buttons.py --guides --crt
```

Its `FONT_5X7` is a stand-in, not the game's font: enough to judge whether a
label sits level with its button and clears the panel edge, and no more.

`--crt` smears chroma sideways and leaves luma alone, which is roughly what a
composite cable does. It is a sanity check on whether a one-pixel border is
still a border by the time it reaches a television -- not a television.
Anything with a menu, a font or a state machine in it still belongs in
`tools/emu`.

## Anything else, in ten lines

The scripts above are just callers. `render.py` is the part worth knowing:
it parses the geometry, and `Scene` poses and rasterises it.

```python
from render import Geometry, Scene, rpy

geom = Geometry.load()
scene = Scene()
scene.cull = False
scene.add(geom.mesh("zombie_head_verts"), rpy(0, 35, 0), (0, 96, -4))
scene.add(geom.mesh("zombie_face_verts", "mob_quad_sheet_display_list", 16),
          rpy(0, 35, 0), (0, 96, -4))
scene.look_at((0, 96, 0), distance=200, yaw=200)
scene.save("build/preview/zombie.png")
```

`Geometry.load()` expands the model macros, so every `DETAIL_BOX`,
`SWORD_BLADE_VERTS`, `TOOL_HEAD_VERTS` and plain `static Vtx` array in
`graphics.c` is available by its own name -- about seventy of them -- along
with the display lists that say which triangles to draw. `define()` pulls a
numeric `#define` out of any source file so a preview can quote the game's
constants instead of restating them.

## What it reproduces, and what it does not

Faithful: `guRotateRPY`'s matrix (row-vector, X then Y then Z, the layout
`setMobRotation` builds by hand), `guPerspective` at the game's FOV and near
plane, the 320x240 framebuffer, the z-buffer, `G_SHADING_SMOOTH`'s interpolated
vertex colours, and `G_CULL_BACK` -- which the mob and first-person passes both
clear, and the preview clears with them.

Not modelled: textures, fog, the CPU-side logic that decides the angles, and
the RDP's fixed-point rounding.

So it answers *where does this end up, what occludes what, does the framing
work*. It cannot answer *is this fast enough* or *does the RDP survive it*, and
a settled model should still be looked at once in the ROM. See
[Emulator screenshots](emulator-screenshots.md) for that, and the hardware
notes in the README for what neither tool can tell you.

## Dependencies

`numpy` and `pillow`, both already in `requirements.txt`. No emulator, no
toolchain, no ROM.
