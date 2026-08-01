# Working on Mine64

## Look at models offline before reaching for the emulator

`tools/preview` draws the game's own geometry on this machine in about a
quarter of a second:

```sh
tools/preview/mob.py pig --strip head-turn
tools/preview/hand.py iron_sword --reach 0.5
```

It reads the vertex arrays, joint offsets and `#define`s straight out of
`src/graphics.c` and `src/mobs.c` and runs the same `guRotateRPY` /
`guPerspective` arithmetic the RSP would, so it is the right tool for any
question about pose, framing or occlusion: which way a head ends up facing,
whether a swing crosses the crosshair, what a model looks like from behind.
Writing a ten-line script against `tools/preview/render.py` is the normal way
to answer a question it has no flag for -- see
[docs/offline-preview.md](docs/offline-preview.md).

`tools/emu` is for the interface and for behaviour that needs the game
running: menus, the HUD, inventory flows, loading. It costs a ROM build and a
fifty-second title-card preroll, the world is different on every run, and a
script has to walk to whatever it wants to look at. That price is worth paying
for a menu layout. It is not worth paying to find out which way a pig is
looking.

Neither tool settles performance or RDP behaviour. Those belong on hardware --
see the hardware notes in the README.
