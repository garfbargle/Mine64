# Hardware notes

Things that only show up on a real console. Current budgets and thresholds are
`#define`s and script constants — read them from the source, not from here.

## Memory

Mine64 fits in the stock console's RDRAM; an Expansion Pak is not required.
NuSystem pins the framebuffers at fixed addresses, and the large allocations
(block window, mesh arena, home store, task buffers, doubled render state)
have to live under them.

`tools/check_ram.py` runs at the end of every build and fails it on an
overrun, because **nothing at link time notices when BSS grows into addresses
NuSystem pins at runtime**. It also warns when headroom gets thin. If you add a
static allocation, that script is the thing that will tell you.

Headroom was bought back by dropping the graphics microcodes the single-task
render path never selects (`src/ucode_stubs.c` keeps NuSystem's table resolving
without them), supplying our own RDP FIFO instead of the SDK's larger one, and
sizing the audio heap to this game's voice count rather than the SDK's
sequence-player default. [RAM budget](ram-budget.md) covers what each
allocation is for and what is still reclaimable.

## Rendering on real hardware

The same world mesh is rendered for every camera rather than duplicated per
player. Split-screen viewports share the framebuffer, and a per-player visible
column cap keeps the frame's display list inside its fixed budget including
avatars and pickups.

All per-frame display lists and referenced matrices are double-buffered, so the
RSP can never read a camera transform while the CPU builds the next frame. This
matters on hardware in a way it does not under emulation.

## Two faults emulators do not reproduce

Both cost a long debugging session. Both present as "the picture is fine and
then the console stops dead".

**Drain the RDP pipe before reconfiguring it.** Changing cycle type, render
mode, combine mode, or the loaded texture tile while a primitive is still in
flight is an RDP hazard. Hardware locks up hard — power cycle required. Whether
it bites depends on how busy the pipe is, so it tracks scene complexity: dense
terrain locks, flat terrain survives, and the same ROM looks intermittent.
Torn frames and garbled glyphs are the warning signs.

`beginText()` opens with `gsDPPipeSync()`, and every branch of `drawHUD()`
drains the pipe before `drawMenu()` reconfigures the RDP for text. Issuing a
texture rectangle while the RDP is still in `G_CYC_FILL` — for example straight
after `clearBuffers()` — locks the console immediately and reproducibly. If new
drawing code mixes fill rectangles and textured primitives, sync between them.

The same rule covers the RDP's *attribute* registers, `gDPSetFillColor` among
them, where the penalty is corruption rather than a lock: change one while a
primitive is still draining and it lands on the tail of that primitive instead
of the next one. Small sprites are where this shows, because a few stolen spans
are most of them. `drawHudMeter()` therefore draws the health and food rows in
three passes grouped by colour — every outline, then every empty interior, then
every fill — which costs three syncs a row instead of forty. Code that alternates
two fill colours per symbol will look correct in every emulator.

**Never busy-wait on the graphics thread.** `nuGfxTaskAllEndWait()` spins on a
counter that is cleared by the scheduler's own graphics thread, which runs at a
*lower* priority than `callbackGfx` (the priority is set in `nusched.c`, not in
any header). The N64 scheduler is strictly priority-based with no time slicing,
so the spin starves the thread that would end it and the wait never returns. To
serialise against the RSP, gate on the `pendingGfx` argument NuSystem already
passes to `callbackGfx` and do arena-rewriting work *before* the next `draw()`
submits a task — never wait.

## Freeze diagnostics

Mine64 cannot be run under a debugger: it runs on real hardware and the
console's only output is the screen. Development builds ship a self-diagnosing
freeze rig that turns multi-session bisection hunts into single-command
answers. Keep it until the streaming work is long stable.

**On-screen overlay** — off by default, toggled with `Z + D-pad Up`. It also
switches itself on whenever an integrity counter ticks, so an absorbed anomaly
is never silent. Rows cover player position, a frame heartbeat, origin rebases,
display-list peak and overflows, streaming and arena census, worst frame gap
and worst gated-CPU cost, and counters for loop guards, position snaps and
repaired window keys. The row legend lives with the drawing code; two rules
don't:

- A frozen heartbeat means no frames are being built at all.
- Of the two timing rows, the CPU cost tracking the frame gap blames callback
  CPU work; a high frame gap with low CPU cost blames the RSP/RDP.

With the overlay up, `Z + D-pad Left/Right` bias the auto fog's resting point
against the frontier, `Z + D-pad Down` toggles fog for an A/B against the bare
streaming edge, and `Z + C-left` cycles the LOD/visibility presets. Columns
re-LOD over a few seconds, so let the timing rows settle before reading FPS.

Rows are occasionally borrowed while a specific fault is being reproduced —
during a collision-marcher hunt the arena census is replaced by the speed and
candidate boundary time that tripped the guard.

**The phase square** (lower left) is painted into the framebuffer by the CPU —
no RSP, no display list — and a high-priority watchdog thread repaints it into
*both* framebuffers once the graphics-callback heartbeat stalls, so it survives
buffer swaps, infinite loops, and the death of the graphics thread. On a frozen
screen it reads in three bands: top = which subsystem was running (colour
legend in the source; red is an RSP/RDP task hang), middle = last player
sub-step, bottom = white if the CPU took an exception, black if it's spinning.

**The SD post-mortem.** Shortly into a freeze the watchdog writes
`mine64/freeze.txt` to the cartridge SD card: faulting thread, PC, CAUSE,
BADVADDR, RA, SP, raw player-position bits, render origin, and every counter.
Deploy scripts archive the matching symbols (`build/mine64-deployed.out`) on
every upload, so one command turns the report into named functions:

```sh
./tools/resolve_freeze.sh
```

Turn the console off first — it owns the SD card while running. This is how a
long-standing "walks far, then dies" freeze was resolved to a single
instruction: an FPU unimplemented-operation fault in `guTranslate`, fed by a
corrupted window key.

**Loop guards over hangs.** Any loop whose termination rests on float
edge-cases carries a bounded iteration guard that breaks out and increments an
on-screen counter instead of hanging a thread that nothing preempts. A rising
counter with no freeze is a confession — the bug becomes observable and
survivable while the root cause is hunted.
