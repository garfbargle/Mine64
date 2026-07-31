# Next session: stop seeing through the illusion

Paste-ready brief for continuing Mine64's streaming-world arc.  State as of
commit `e4612a1` (2026-07-31).

## Ground yourself first

1. `README.md` → *Hardware notes*: the two RDP/scheduler faults, and the
   **Freeze forensics** section — the debugging rig is permanent and it is
   how every hard bug gets solved here.  Do not theorize from source;
   instrument, run on device, read the counters.
2. `docs/streaming-world-plan.md` — the full bisection log (runs 1–7) and
   the remaining task ladder.  Trust its cost model.
3. Auto-memory covers the rest (RDRAM budget, control scheme, freeze
   history).

## Where things stand

The world is walkably infinite and stable on real hardware.  Near terrain
renders from baked vertex buffers (no per-quad matrices), far terrain in
the ring is a surface shell, streaming is time-sliced (~10 ms/callback)
with heading-biased prefetch, and the old freeze families are absorbed by
self-healing guards that confess on screen (`L` collision clamps, `G`
position snaps, `K` repaired window keys — overlay toggles with Z+D-pad-Up
and auto-shows on any tick).  A frozen console writes `mine64/freeze.txt`;
`./tools/resolve_freeze.sh` turns it into named functions.

The player's own words: "infinite worlds is amazing. we just need to not
see through the illusion so easily."

## The illusion problem, in priority order

1. **Hide the loading edge.** — *landed, pending hardware tuning:*
   sky-matched RSP fog now wraps the gameplay terrain/water pass (2-cycle,
   `G_RM_FOG_SHADE_A`, combiner `G_CC_PASS2`, restored to 1-cycle before
   entities), with the start tunable live (Z + D-pad Left/Right, P row;
   Z + D-pad Down toggles).  Tune on a CRT, bake the chosen default into
   `fog_start`, and watch W/B for the 2-cycle fill cost.  Original notes:  At sprint speed the player can reach the
   mesh edge and watch columns arrive in visible rows — worse, with no
   floor/walls in the unbuilt region they see cave cross-sections.  The
   N64's answer is **distance fog** (RSP fog blended toward the day-cycle
   sky color) placed just inside the decorated ring, so unbuilt terrain is
   behind haze, plus consideration of: not drawing the outermost
   in-progress ring, and a skirt or ground-plane fill below the fog line.
   Fog interacts with render modes — remember `gDPPipeSync` before any RDP
   reconfiguration, and expect the usual hardware-only hazards.  This is
   the fastest illusion win and should come first.
2. **Single mesh arena with free-list + relocation defrag** (plan task 5,
   step 1).  With `pendingGfx == 0` guaranteed per callback, one column per
   frame can be relocated and its pointer swapped atomically — the second
   1 MiB arena and the whole compaction scheme go away.
3. **Widen the window 16 → 32** (power of two only — slot math is masks in
   `blockGet`'s hot path).  Radii become terrain 15 / waystones 14 /
   decorated+mesh 13: **~104-block view distance, 2.4× today**, far ring
   all shell.  Retune `CULL_SPAN`, visible caps, budgets, `tree_at_root`
   span; verify on hardware incrementally — this touches everything.
4. **Open forensics hunts** (background, whenever a counter ticks):
   - `K > 0`: a stray store corrupts `window_keys` — the freeze report's
     KVAL/KSLOT carry the corrupted bit pattern; its shape (float? nibble
     pair? s16s?) should name the writer.
   - `L > 0`: collision goes degenerate repeatedly — likely terrain
     streaming in against/inside the player box.  Root-cause rather than
     rely on the clamp forever; the clamp now halts instead of tunneling,
     which also stopped players falling out of the world.
5. **After the illusion holds**: per-chunk diff saves + `world_seed`
   persistence (plan task 6 — kills "Too far from spawn to save"), then
   audio in the default ROM (needs the RAM from step 2), then the gameplay
   ladder already scoped in the session task list: light + torches,
   durability/furnace/chests/beds, night hostiles + mob gravity, QoL.

## Discipline that has repeatedly paid off

- One variable per hardware test; land increments the console can verify.
- `tools/gentest/run.sh` before trusting any generation change.
- Never busy-wait on the graphics thread; gate on `pendingGfx`.
- `gDPPipeSync` before reconfiguring the RDP.
- Keep the audio build linking under its RAM ceiling (`check_ram.py`).
- Deploy with `./live-load` (it archives matching symbols automatically).
