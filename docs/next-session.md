# Next session: make the world keep what the player makes

Paste-ready brief for continuing Mine64.  State as of commit `b8b91e2`
(2026-07-31).

## Ground yourself first

1. `README.md` → *Hardware notes*: the two RDP/scheduler faults, and the
   **Freeze forensics** section — the debugging rig is permanent and it is
   how every hard bug gets solved here.  Do not theorize from source;
   instrument, run on device, read the counters.
2. `docs/streaming-world-plan.md` — tasks 2–5 are done; read *What landed
   after task 5* for the subsystems built on top, and *Task 6* for what is
   next.  Trust its cost model.
3. Auto-memory covers the rest (RDRAM budget, control scheme, freeze
   history).

## Where things stand

**The streaming arc is finished.**  The world is walkably unbounded: a
32×32-column residency window, rings at terrain 12 / structures 11 /
decoration and mesh 10, a ~80-block view behind sky-matched fog, one 1 MiB
mesh arena with incremental relocation defrag, and a deferred underground
carve that keeps far columns cheap.  Origin rebasing, the single arena, and
the long-walk stability are hardware-verified.  The old freeze families are
absorbed by self-healing guards that confess on screen (`L` collision
clamps, `G` position snaps, `K` repaired window keys — overlay toggles with
Z+D-pad-Up and auto-shows on any tick).  A frozen console writes
`mine64/freeze.txt`; `./tools/resolve_freeze.sh` turns it into named
functions.

**The work since has been gameplay on top of it**: procedural hamlets and
ruins, torches/stairs/doors/windows as sparse detail records, a player-edit
journal, zombies and spiders, hunger, and an idle camera sway.

## Not yet on the console

The two most recent commits are **built by nobody yet** — `mine64.out`
predates them:

- `9fc5c99` removes the pool scans from the mesher's inner loop (the "loading
  is 10× slower" regression: ~600 M wasted iterations per 64 columns).  It
  should restore load times and the post-naming lag; that needs confirming.
- `b8b91e2` gives the save files 8.3-legal names.  **Saving has been broken
  since the world outgrew 64×64** and every slot has read as empty on the
  title screen for just as long, because FatFs rejected the path before
  touching the card.  Confirm on hardware: create a world, save, power
  cycle, reload.  If storage still fails, the title screen now names which
  of the four steps failed.

## Priorities

1. **Retune the fog for the new horizon.**  `fog_start` is 993, chosen for
   the old ~44-block ring; the mesh ring is now ~80 blocks, and the
   tunable range cannot currently reach that far.  Fog is a function of
   *screen* depth, which the 10/14000 projection compresses savagely, so
   this is arithmetic before it is taste — work out where the band can
   actually be placed, then tune the rest on a CRT with Z + D-pad
   Left/Right (P row; Z + D-pad Down toggles for an A/B).  Watch W/B for
   the two-cycle fill cost.
2. **Task 6 — persistence.**  Now the biggest hole in the game: saves still
   write the original fixed footprint, so saving away from spawn is refused,
   `world_seed` never reaches disk, and **placed torches, stairs, doors and
   windows do not survive a reload** — they come back as the crafting tables
   that proxy them.  Per-chunk diffs plus the detail and edit pools, in one
   version bump.  This is the one file where a mistake destroys player
   worlds; do it once, carefully.
3. **Audio.**  The audio variant overruns its heap by ~20 KiB and needs
   roughly 384 KiB.  A pooled (indirected) block window is the likely
   source, or trimming after diff saves land.
4. **Wire up the sword specials.**  `useMobWeaponSpecial` and
   `mobWeaponSpecialCooldown` are implemented, and their effects are
   simulated every frame, but nothing calls them.  Either bind an input and
   draw the cooldown, or delete them — dead simulated state is worse than
   neither.
5. **Open forensics hunts** (background, whenever a counter ticks):
   - `K > 0`: a stray store corrupts `window_keys` — the freeze report's
     KVAL/KSLOT carry the corrupted bit pattern; its shape (float? nibble
     pair? s16s?) should name the writer.
   - `L > 0`: collision goes degenerate.  The floored-grid fix in `f3bb73d`
     addressed one cause (negative coordinates producing a negative
     collision time); if L still climbs, the S and N rows appear in place of
     C and K with the speed and boundary time that triggered it.
   - The known WIP from `dc798e2`: holding **R** to jump while walking can
     stop the player running and make L climb until R is released.

## Discipline that has repeatedly paid off

- One variable per hardware test; land increments the console can verify.
- `tools/gentest/run.sh` before trusting any generation change.
- Never busy-wait on the graphics thread; gate on `pendingGfx`.
- `gDPPipeSync` before reconfiguring the RDP.
- No unbounded pool walk anywhere `blockAt`/`blockGet` can reach — the
  mesher calls it 24,448 times per column.
- Keep the build under its RAM ceiling (`check_ram.py` runs automatically).
- Deploy with `./live-load` (it archives matching symbols automatically).
