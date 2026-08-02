# 64MON

A creature-collecting mod for Mine64, switched on when a world is created and
locked in with the rest of the world's mods. Eighteen species wander the
world; you meet them, raise them, evolve them, and fight wandering trainers or
another player with them.

It is designed to cost the console nothing it was not already spending. The
three rules that shape every part of it are in the header comment of
[`include/mon64.h`](../include/mon64.h) and repeated below, because
anything added later has to keep them true.

## Turning it on

`64MON` is the last row of the **EXTRAS** section on the create-world
card. It is a behaviour switch, not a terrain one: the same seed with it on
and off produces block-for-block the same world, so toggling it never rebuilds
the preview. The mask is written into the save header like every other mod,
so a world keeps it forever.

## Controls

| Control | Action |
| --- | --- |
| **A** facing a creature | Battle it (or take it as your first partner) |
| **Z + A** facing another player | Challenge them |
| Analog stick / D-pad | Move the battle cursor |
| **A** | Choose; advance the battle log |
| **B** | Back out of a submenu |

Nothing is ever forced. Walking into a creature does not start a fight: a
badge appears above the hotbar naming what is in front of you, and **A** is
what acts on it. The one exception is the aggressive middle-stage species,
which start fights themselves after dark — and only in a world that did not
choose **PEACEFUL**.

## The loop

**Your first creature is a gift.** With an empty team, walking up to any wild
creature and pressing **A** makes it join you rather than fight you. There is
no starter menu and no tutorial: the shortest path in is also the one where
the player picks.

**Encounters** are wild creatures that wander like the animals do. Their level
comes from how far the world's origin is behind you — the further you walk,
the older the things you meet — bounded to within a few levels of your own
lead so a fresh team is never mauled and a veteran is never farming level
threes. This costs nothing to store and makes a direction of travel a
decision.

The level is rolled *before* the species, and gates it: a middle stage may
only stand on a pad whose level has passed the threshold its own young form
evolves at. Weight alone used to decide this, which put full-grown GRIZZLEs in
starting meadows at level four while the player's own EMBEAR still had twelve
levels to go — and an evolution threshold you can walk past in the grass is a
threshold that means nothing.

**Battles** are turn-based, four commands wide: FIGHT, TEAM, BAG, RUN. They
happen where you are standing — your own view, your own terrain, the pair
squaring up a few blocks in front of you — with the world paused behind them.

**Catching** uses slime gel. There is no ball item: throwing gel at a weakened
creature is the whole mechanic, which means the loop wants you hunting slimes
at night and creatures by day, and it needed no new item, icon, or recipe.
Odds scale with how far the target's health has fallen and against its level.

**Levelling** is experience for a win, and everything else is derived — a
creature is six bytes (species, level, experience, health) and its statistics,
moves and appearance all come out of the species table. That is what makes a
balance change safe: retuning a base stat retunes every creature in every
save at once.

**Evolution** happens on the level-up that crosses the species' threshold, and
is announced. Each family's final stage never spawns wild, so a fully grown
creature is something you raised rather than something you found.

Growing up mid-battle re-derives the fighter but does not restock it: a
knockout is not a rest, so remaining power and any ATTACK or DEFENCE the fight
has built up survive it. Rebuilding the fighter outright — which is what used
to happen — handed back a full bar after every faint in a trainer battle and
threw away the CHARGE you had just won with. A move carried across keeps what
is left of it; one that arrives with the new level or the new species arrives
full.

**Trainers** are one spawn in seven. Their team is a pure function of the
coordinate and world seed they were spawned from, so the same trainer is the
same fight every time without a byte being written down. They pick their moves
by what those moves would actually do; wild creatures roll — and neither side
may spend a move that has no power left, which used to be a rule the player
alone was held to.

A trainer fields nothing you could not be holding at the same level: a drawn
species walks back down its family until its own threshold is one that level
has passed. A single step used to do this, and one step off a final stage
lands on a middle stage — which is exactly the thing a low-level trainer
should not have either, so two thirds of an early team came out a stage too
old.

A trainer is drawn as a person, not as a creature -- and not by 64MON at all
any more. The body, the gait, the hair and the matrices come from
`humanoid.c`, which is the same body the player wears and the same one the
villagers in the hamlets wear; this feature supplies where a trainer stands
and which way they are facing, and nothing else. Twice the height of anything
else roaming, they are recognisable across a field, which they have to be:
walking up to one starts a three-creature fight rather than a catch.

Which person they are, and their name, comes from the seed their team already
comes from -- so a trainer who is the same fight twice is the same person
twice, and the badge shows that name in place of TRAINER while they open the
battle with it. Before this a trainer was drawn as the species they happened
to spawn beside, and the badge was the only thing that said otherwise.

They no longer compete with the creatures for a slot, either. A trainer takes
one of the shared people slots and a creature takes one of the two here, so
meeting somebody on the road does not cost you the sight of the animal
standing behind them.

**Player versus player** is **Z + A** while facing another local player. Each
side's commands come from that side's controller, and the battle takes the
whole screen. Both sides earn from what they knock out; only the challenger
used to, so player two could win the whole fight and come away with nothing.

**Recovery** is automatic: one health point per team member every four
seconds. There is nowhere to rest in a world you may be a thousand blocks from
spawn in, and a losing streak should cost time rather than the save. Losing
every creature leaves the team at one health each rather than stranded.

## Types

Six types, eight arrows, every one of them symmetric — if it doubles one way
it halves the other, so there are only eight facts to learn:

```
FIRE  > GRASS > WATER > FIRE      (the triangle)
EARTH > SPARK > STONE > EARTH     (the other triangle)
SPARK > WATER                     (electricity in water)
GRASS > EARTH                     (roots split soil)
```

Moves may also be *plain*, which is always neutral. No creature is plain; it
exists so every creature has something to hit an awkward matchup with while
the chart itself stays a readable six by six.

## The roster

Six families, one animal each, and one naming rule that is not really a rule:
a name is a blend where the element word and the animal word already share
their sounds. EMBEAR because "ember" and "bear" are most of the same word;
TOADSTOOL because it already is one. A name that has to be explained is a
name that failed.

| Type | Animal | Young | Grown | Elder |
| --- | --- | --- | --- | --- |
| Grass | frog | TADPOLLEN | LEAFROG | TOADSTOOL |
| Fire | bear | EMBEAR | GRIZZLE | PYREBRUIN |
| Water | eel | DRIZZEEL | TIDEEL | WHIRLEEL |
| Earth | iguana | MUDLET | MUDGUANA | IGUANEOUS |
| Spark | owl | SPAROWLET | SPAROWL | THUNDOWL |
| Stone | ape | PEBBOON | GRANILLA | BABOULDER |

A family keeps its animal — and its rig — through all three stages, because a
bear that evolves into something which is no longer a bear makes the name a
lie. Growing up is therefore three things at once: **scale**, **bulk** (width
and depth only, so an elder reads as heavier rather than merely taller), and
**boxes it has grown into**. Each rig carries two or three parts gated behind
a maturity threshold: the bear grows a shoulder hump and then a heavy brow,
the owl grows ear tufts and then a chest ruff, the eel grows side fins and
then a tail fan. A cub is a simple round shape; an elder is the same shape
with age on it.

Bulk moves the parts it widens. An offset left on plain scale puts a box where
the unbulked body used to be, which is inside the bulked one — and because
nothing about that is visible in the table, the two most heavily built elders
in the game spent their whole existence with no eyes: TOADSTOOL's and
WHIRLEEL's had been swallowed by their own bodies. THUNDOWL's chest ruff was
a plainer miss, authored at `z -9` inside a body whose front face is at `-14`,
so no owl ever wore the one feature that was supposed to make an old one look
old. `tools/preview/mon.py --audit` now fails on any part fully inside
another, on any rig that does not stand on the floor, and on any move levelled
past its own species' evolution.

Habitats come from the block a creature is standing on — grass, sand, stone,
or a shore with water within two cells — plus time of day. There is no biome
map anywhere; a beach and a cave floor draw from different rosters because the
pad they spawned on answers a different question.

The owl line is the one that uses the clock: all three stages are nocturnal,
so spark is a type you go out after dark for. That flag used to sit on the
hatchling alone and on no other creature in the game, which made the whole
night half of the habitat system dead weight and left the adults of a family
of owls wandering around at noon. It is also the one change here a player will
feel as balance rather than as a fix — spark is the answer to water and stone,
and it is now a nightly errand. One flag per row reverses it.

## What it costs

**The entity count does not grow.** Roamers do not add a pool beside the mob
pool — they take four slots out of it. Turning the mod on trades farm animals
for creatures; it never asks the RSP to transform more boxes than a vanilla
world already does. See `mon64RoamerReserve` and its two call sites in
`mobs.c`.

**A battle is a pause, not a scene.** It is a mode inside `GAME` rather than a
`Screen` value: the world is already meshed and the camera is already there,
so a battle costs two creature models and a flat overlay while movement, mobs,
items, trees, streaming and the world clock all stop. It is the cheapest frame
the game draws with terrain on it. Making it a screen would have meant
auditing every `current_screen == GAME` test in the project for something
that is, in every way that matters, still the game.

**Every battle number is an integer.** Levels, damage, catch odds and
experience never touch the FPU. Both hardware freezes this project has chased
ended in float edge cases, and a turn-based system has no reason to offer a
third.

**Models are built, not baked.** A creature's boxes are written into a
double-buffered scratch each frame from the rig table and the species palette.
Eighteen species times eleven boxes of static `Vtx` would be a substantial
slice of RDRAM in a build with this little to spare; rebuilding costs at most
eighty-eight vertex writes per creature per frame, there are never more than
two on screen, and it buys per-species size, bulk and colour for nothing. The draw path itself is exactly the mob path — two
matrices, one `gSPVertex`, one shared display list per box.

**Creatures cast shadows.** Trees, players, mobs and even dropped items all
had a ground blob and creatures had none, which reads as pasted on rather than
as standing there. This is what each rig's authored height has always been
for — it was written down six times and never once read, and all six values
had drifted as much as thirty per cent below the model they described. The
blob is sized from the same scale and bulk the model is, so a cub's shrinks
with the cub. It costs no slots the pass was not already spending: the ones
creatures take here are the mob slots `mon64RoamerReserve` took out of the
pool, which is the same trade the whole feature is built on. A trainer casts a
person's blob, because a person is what is drawn.

Measured cost of the whole feature: about 57 KiB of the link, of which roughly
32 KiB is code, 11 KiB is the render slots' matrices and vertex scratch, and
under 2 KiB is tables and live state. It ate into the headroom the audio
variant is waiting on, and that variant no longer links; the current figures
for both builds are in [RAM budget](ram-budget.md).

## Where the code is

| File | Contents |
| --- | --- |
| `include/mon64.h` | Every type, the budget rules, the whole public surface |
| `src/mon64_data.c` | Rigs, species, moves, type chart — all `const` |
| `src/mon64.c` | Statistics, party, roamers, encounters, the battle |
| `src/mon64_draw.c` | Models, the battle interface, the encounter badge |

Integration is six small hooks, each one commented where it lands:

- `mods.h` / `mods.c` — the `MOD_64MON` bit and its setup-card row.
- `mobs.c` — passive budget and initial seeding minus the roamer reserve.
- `player.c` — battle input routing, the **A** button hook, `mon64Update`.
- `main.c` — `init64MON` beside `initMobs`, and holding the world clock.
- `graphics.c` — the entity pass, the interface, and suppressing the HUD.
- `menu.c` — the setup card now derives its row pitch instead of fixing it.

`generate_assets.py` also gained five font glyphs (`!`, `,`, `'`, `/`, `?`).
The atlas held capitals, digits and four symbols, so any text with sentence
punctuation came out with a seven-pixel hole where the mark should be.

## Not wired yet: the party in a save

`mon64SaveSize`, `mon64SaveBlob` and `mon64LoadBlob` produce and
consume the party as a flat 144-byte blob, and nothing calls them.

That is deliberate. Saves still write the original fixed footprint and are
refused away from spawn, and the per-chunk diff format that fixes it will move
every offset in the file — adding a section to the save format twice is how
save formats acquire the bug that eats worlds. When that version bump lands,
the party belongs in the same one:

```c
/* in the header/writer, beside the other per-world sections */
mon64SaveBlob(section_pointer);
/* in the loader, guarded by the version that introduced it */
mon64LoadBlob(section_pointer, section_length);
```

`mon64LoadBlob` already drops any slot whose species or level is outside
the tables, so a save written by a build with a different roster loads
without indexing off the end of anything.

## Looking at it

`tools/preview/mon.py` draws creatures from the roster tables on the host, in
about a quarter of a second, applying the same scale, bulk and maturity
arithmetic `drawCreature` does and the same box shading `buildBox` does:

```sh
tools/preview/mon.py thundowl          # one creature
tools/preview/mon.py --family bird     # a family, side by side
tools/preview/mon.py --roster          # all eighteen
tools/preview/mon.py --audit           # the invariants, no picture
```

Reach for it before the emulator for anything about proportion, palette or
which boxes a stage wears. Every geometry bug listed above was invisible in
the table, invisible in a battle, and obvious in one strip of frames.

`tools/emu/scripts/64mon.txt` walks the setup card down to the 64MON
row, turns it on, and enters the world. `tools/emu/scripts/64mon-ui.txt`
captures the battle interface — it expects a build whose
`ROAM_INTERACT_RANGE` has been temporarily widened, because encounters are
opt-in and spawn positions are random, so an unmodified build cannot be driven
into a fight by a fixed script.

As always: the emulator is only good enough for looking at layout. Frame
pacing, legibility on a composite signal, and the RDP hazards are hardware
questions.
