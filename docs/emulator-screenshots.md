# Emulator screenshots

Capturing Mine64's interface without a flashcart, a controller, or a TV. One
command replays a written timeline of controller input and writes a labelled PNG
per screen:

```sh
tools/emu/run.sh tools/emu/scripts/gui-tour.txt
```

That produces `build/shots/01-title.png` through `09-camera.png` at native
640×480, straight out of the emulator's framebuffer. The run needs no keyboard,
no window focus, and no desktop permissions, and it lands on the same screens
every time.

## 1. Install the emulator

```sh
brew install mupen64plus
```

Nothing else is needed. The plugin below builds against the mupen64plus headers
Homebrew installs, and `run.sh` builds it on first use.

## 2. How the input works, and why

mupen64plus reads its controller from an input plugin, so Mine64 supplies its
own: `tools/emu/mine64_input.c` replaces `mupen64plus-input-sdl` with one that
ignores the keyboard entirely and replays a script instead. It also holds the
core library handle, which lets a script ask for a screenshot at an exact point
in the timeline rather than at a guessed frame number.

The alternative -- launching the emulator and driving it with synthetic
keystrokes -- was tried first and is worse in every way. macOS blocks keystroke
injection without an Accessibility grant, SDL only sees keys while its window
holds focus, so the run fights whatever else is on screen, and holding a
direction means hammering the key rather than holding it. The plugin has none of
those problems and produces byte-identical framing across runs.

## 3. Writing a timeline

Scripts live in `tools/emu/scripts/`. One command per line, `#` starts a
comment:

```
wait 400                # neutral controller for 400 frames
shot title              # capture, named 01-title.png
press START 8           # hold START for 8 frames, then release
stick 0 70 150          # analog stick at (x=0, y=70) for 150 frames
press A+Z 8             # several buttons at once
stop                    # end emulation
```

Buttons are `A B Z START L R`, the C pad as `CUP CDOWN CLEFT CRIGHT`, and the
D-pad as `DUP DDOWN DLEFT DRIGHT`. Stick axes run -80..80. Every `shot` takes an
optional label; `run.sh` matches labels to captures in order and renames the
PNGs, so the filenames stay meaningful when a script grows.

## 4. Durations are rendered frames, not polls

This is the one thing worth remembering. Mine64 reads the pad roughly twice per
rendered frame while it is playing, and far faster than that while it is
generating a world -- the loading screens poll in a tight loop. A timeline that
counts `GetKeys` calls therefore races through world generation and drops
presses the game never observes: the first version of this tool sat on the
CREATE WORLD screen while its script believed it was three screens further on.

So the plugin counts the video plugin's per-frame `RenderCallback` and advances
the timeline only when a frame has actually been drawn, holding one controller
state across every poll within that frame. With that, the core's reported
capture frames match the script's arithmetic exactly, which is the quickest way
to confirm a timeline did what it says.

## 5. Timings that a script has to respect

Two waits in `gui-tour.txt` are not padding:

- **The world preview.** `menuAct` refuses to launch while
  `menu_preview_requested` is set, because the terrain drawn behind the menu
  must match the slot being selected. START is ignored for roughly the first 900
  frames; pressing early does nothing at all.
- **A `shot` immediately before `stop`.** The core writes a screenshot at the
  end of the frame, so stopping on the next one loses it. The plugin holds
  `stop` back five frames to cover this -- worth knowing if the last capture of
  a script ever goes missing.

## 6. What it is not good for

Emulation here is glide64mk2 with the HLE RSP. It is good enough for looking at
interface layout and nothing more:

- **It flatters the GUI, for three specific reasons.** `initVideo` selects
  `osViModeNtscLan1`, so the console renders 320x240 into an *RGBA5551*
  framebuffer -- 32 levels per channel, not 256 -- and the VI's anti-alias
  filter softens every edge on the way out. The emulator gives you 8-bit
  colour with no VI filter, then the display path adds its own blur on top.
  Captures therefore show none of the banding, dither, or edge softening that
  decides whether HUD text is actually readable. (Gamma is not a difference:
  `osViSetSpecialFeatures(OS_VI_GAMMA_OFF)` turns the VI's boost off, which is
  what the emulator does anyway.)

  `run.sh` captures at 320x240 so at least the geometry is 1:1. `RES=640x480`
  doubles it for inspection, but that is the video plugin resampling, not
  detail the console ever had.
- **Frame pacing is not representative.** See the pacing baselines in the README
  rather than timing anything here.
- **The two hardware faults in the README do not reproduce.** Both the RDP
  pipe-sync hazard and the priority inversion in `callbackGfx` run cleanly under
  emulation, which is exactly why they cost so much to find.
- **Nothing that touches a save can be tested here.** mupen64plus emulates no
  flashcart, so `initStorage` fails, every slot reads as empty, and the title
  screen always takes the *generate* path. The load path -- `beginLoadGame`,
  the sliced payload, checksum verification, backup recovery -- never executes
  under emulation at all. Changes to `src/storage.c` have to go to hardware.

## 7. Cover the states a fresh save does not reach

`gui-tour.txt` starts a new world, so its player carries nothing, and a HUD with
an empty hotbar hides real problems -- the held-item name and the stack-count
labels never draw at all. A tour that is meant to check the HUD has to mine
something first. This is the general trap: an emulator run only exercises the
screens the script walks through, and the states it skips are exactly the ones
that go unreviewed.

## 8. Measuring, not just looking

`loading-progress.txt` is the other kind of script: it does not tour screens,
it samples one thing at a fixed interval so the numbers can be compared. Its
shots are spaced evenly across a world build, and reading the filled width of
the bar out of each PNG is what turned "the bar looks stuck" into a measurement
-- first that the three generation stages take the same wall time as each
other, then that the bar advances 7-9 points per 55 frames from one end of a
build to the other. Both of those went straight into the stage weights in
`worldGenerationProgress` and the `BAR_SHARE` split in `main.c`, which is why
those comments claim measurement rather than estimation.

## 9. What this has turned up so far

None of these are fixed:

- **The held-item name collides with the food bar.** `drawHUD` draws it at
  y=190 with a 7-pixel font, and `drawHealth` puts the food pips at y=192..197
  in single-player, so the name is struck through the middle of the hunger row.
  Visible on hardware the moment anything is held; invisible to a tour that
  never picks an item up.
- **The PLANKS recipe icon does not draw its texture.** It captures as a solid
  black square even though `planks_texture` exists and in-world planks render
  correctly. It appears in a different wrong colour on hardware, which is the
  usual signature of a combiner or TLUT state the two rasterisers resolve
  differently.
- `03-world-naming`: "NO CART SAVE DEVICE" is drawn through the `KEY` legend and
  collides with the START button. Three strings share that bottom strip.
- The INFO screen is unreachable. `src/menu.c` renders the full controls list
  and `menuAct` handles leaving it, but nothing anywhere assigns
  `current_screen = INFO`, so the help screen cannot be opened.
