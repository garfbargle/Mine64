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

**Trainers** are one spawn in seven. Their team is a pure function of the
coordinate and world seed they were spawned from, so the same trainer is the
same fight every time without a byte being written down. They pick their moves
by what those moves would actually do; wild creatures roll.

A trainer is drawn as a person, not as a creature: `mon_trainer_bodies` holds
Steve's own measurements written in the rig table's format, and the head wears
the same 24-vertex face sheet the player's does. Twice the height of anything
else roaming, a trainer is recognisable across a field — which it has to be,
because walking up to one starts a three-creature fight rather than a catch.
Before this the trainer was drawn from the species it happened to spawn
beside, and the HUD badge reading TRAINER was the only thing that said
otherwise.

There are eight of them — SAM, TOBY, MILO, ELI, KALIA, MAYA, NORA and IVY —
and the seed picks which one, so a trainer who is the same fight twice is the
same person twice, down to the name the badge shows in place of TRAINER and
the line they open the battle with. All of it comes out of the seed the team
was already derived from; none of it is stored.

They share two bodies, and the bodies share their six box torso: everything
past it is hair, which is the only thing that tells two blocky people apart at
this resolution. The short body has a crown, a nape panel and a box at each
temple; the long one a deeper crown that clothes the back of the skull, a
fringe, a full strand down each temple, and a ponytail on the tail role, so it
swings when she walks. Hair is a fourth tone (`MON_TONE_HAIR`) that no species
uses.

The short body's crown and nape are the player's own — `steve_hair_crown_verts`
and `steve_hair_nape_verts`, ride the head transform, cost no matrix. Steve
had nothing but the sheet's painted fringe and sideburns for a long time,
which passes right up until something stands next to him with hair on top of
its head.

Every hair box overlaps its neighbours rather than butting against them, and
the crowns are carried *past* the face plane rather than up to it: hair that
stops level with the head leaves a bare line of scalp between the crown and
the face sheet's painted fringe. The temple boxes exist for the same reason
the fringe does — to cover the sheet's painted sideburns, which are a fixed
brown and would otherwise pin every trainer to one hair colour.

**Player versus player** is **Z + A** while facing another local player. Each
side's commands come from that side's controller, and the battle takes the
whole screen.

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

Habitats come from the block a creature is standing on — grass, sand, stone,
or a shore with water within two cells — plus time of day. There is no biome
map anywhere; a beach and a cave floor draw from different rosters because the
pad they spawned on answers a different question.

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

Measured cost of the whole feature: about 55 KiB of the link, leaving
~96 KiB free below NuSystem's framebuffer reservation. Roughly 30 KiB of that
is code, 11 KiB is the render slots' matrices and vertex scratch, and under
2 KiB is tables and live state. This does eat into the headroom the
audio variant is waiting on; see *Known gaps* in the README.

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

`tools/emu/scripts/64mon.txt` walks the setup card down to the 64MON
row, turns it on, and enters the world. `tools/emu/scripts/64mon-ui.txt`
captures the battle interface — it expects a build whose
`ROAM_INTERACT_RANGE` has been temporarily widened, because encounters are
opt-in and spawn positions are random, so an unmodified build cannot be driven
into a fight by a fixed script.

As always: the emulator is only good enough for looking at layout. Frame
pacing, legibility on a composite signal, and the RDP hazards are hardware
questions.
