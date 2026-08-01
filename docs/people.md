# People

Everybody in Mine64 who is a person — the players, 64MON's wandering trainers,
and the villagers who live in the hamlets — is one body, one gait and one draw
path, in [`src/humanoid.c`](../src/humanoid.c).

That was not true for long. The player was seven boxes with their colours baked
into the vertices, and a trainer was the player's measurements copied by hand
into the creature rig table: two representations of the same body, kept in
agreement by nothing at all. Change the torso and only one of them moved.

## What makes one path serve three callers

**Geometry is shared and colourless.** The boxes carry shading only — white on
the lit face, grey on the far one — and the garment colour arrives as the
primitive colour, which the entity combiner already multiplies against shade.
So a hundred people would cost one set of vertices, and the per-person price of
looking different is one RDP colour per part instead of eight vertex writes.
The day/night tint is folded into that colour on the CPU rather than replacing
it, which is why nobody's shirt stays noon-bright at dusk.

**Pose is data, not a player.** `humanoidDraw` takes angles, so whatever
computes them — a controller, a roamer wandering a field, a villager walking
home at dusk — gets the same swinging limbs and the same head that turns
independently of the shoulders. What is still specific to a controller stays in
`graphics.c`: the head that follows the camera, the swing that carries a
pickaxe, the tuck of a vault, the shove of being hit.

**Matrices come from one pool.** Players hold the low slots and are never
displaced; everyone else claims what is left, per viewport, nearest first.
`HUMANOID_NPC_SLOTS` is the whole budget of the feature and the only number
that has to grow for a busier world — see
[docs/ram-budget.md](ram-budget.md) before turning it.

## The cast

Eight people, in `humanoid_people`: a name for whatever badge goes over their
head, and a look. Two hair styles do the work of telling them apart, because at
this resolution hair is the only thing that ever has. Nobody wears the player's
blue over the player's own skin — a person walking toward you in co-op must not
read as the other player.

Every one of them is chosen by a seed and nothing about them is stored. A
trainer's seed is the coordinate their roamer spawned at; a villager's is the
cottage they live in. The same house in the same world is always the same
person, in the same clothes, under the same name.

## Hair

The face sheet paints a hairline and sideburns flat onto the front of the head.
On its own that is a fringe drawn on a bald skull, so every style starts with a
box above the crown.

Two rules, both learned the hard way:

- **Overlap, never abut.** A crown that stops level with the head leaves a line
  of bare scalp between it and the painted fringe. Invisible from straight on,
  and the first thing the eye finds from any other angle. The crowns are
  carried *past* the face plane, and the long style's fringe shares the crown's
  width and height so no step shows across the top of the skull.
- **Cover what is painted.** The sheet's fringe and sideburns are a fixed
  brown. Any style that wants a hair colour of its own has to put geometry in
  front of them, which is what the long style's fringe and temple strands are
  for.

## Villagers

A cottage the generator stamps is a house nobody lives in, which reads as a
ruin rather than a home. Villagers are the difference, and they are the cheapest
inhabitants that still read as inhabitants — see
[`src/villagers.c`](../src/villagers.c).

**They are not stored.** A villager is a pure function of their cottage.
Walking away and coming back meets the same person at the same house.

**They do not grow the entity count.** The pool is taken out of the passive mob
budget while a hamlet is in range, the way 64MON's roamers are: a village trades
sheep for the people who live there. Away from a hamlet the reserve is zero and
nothing changes, which is almost everywhere.

**They keep to their doorstep.** A leash of nine blocks, and after dark they
walk home and stay there. It is the cheapest schedule that reads as a day: a
village that empties at dusk and is busy again in the morning.

**They look at you.** A villager who has stopped turns their head to whoever is
near, which costs a comparison and a heading and is most of the difference
between a village and a set of props.

Where a villager stands when their cottage wakes up is a search, not an
assumption — each side of the house in turn, because a cottage may have the
well, a path, another cottage or the hillside it was cut into on any given
side. The generation harness found exactly that: one cottage in twenty-seven
with nothing standable to its east, which a single-side rule would have left
empty forever. `tools/gentest/run.sh` now checks every cottage the hamlet query
names has both a floor under it and a doorstep beside it.

**What they are not, yet, is anybody you can talk to or trade with.** That is a
design question rather than a modelling one, and an economy is not something to
invent by accident.

## Looking at them

`tools/preview/person.py` poses any of them offline, reading the boxes, the
anchor offsets and the look table straight out of `humanoid.c`:

```sh
tools/preview/person.py                    # the player
tools/preview/person.py kalia --walk 0.25  # mid-stride
tools/preview/person.py --everyone         # the whole cast
tools/preview/person.py sam --turn         # a filmstrip of a head turning
```
