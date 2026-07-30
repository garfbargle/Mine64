# Mine64 revival roadmap

## North star

Mine64 should feel like a 1997 first-party adventure that happens to be made
of blocks: immediate controls, bold silhouettes, a world with memorable
places, expressive low-cost animation, and systems that combine in surprising
ways. It should borrow Minecraft's legibility without treating modern
Minecraft feature parity as the goal.

The stock 4 MiB console remains the baseline. The Expansion Pak may eventually
improve sight distance or ambience, but no core mechanic should require it.

## The v0.4 foundation

This pass establishes the systems that later content can share:

* A 160×32×160 live world in a four-bit-per-block representation.
* Coal, iron, bedrock, mossy waystones, caves, waterways, and a full
  wood-to-stone-to-iron tool arc.
* Proper shaded models for held tools, avatars, pickups, pigs, and slimes.
* Vaulting, fall damage, food, apples, species drops, and slime-gel cushioning.
* A bounded polymorphic mob pool with passive, flee, chase, contact-attack,
  spawn, and despawn behaviours.
* A compact compass, progressive adventure cards, and a collapsible C-button
  guide designed for 320×240 output.
* Three-row terrain-height sampling, incremental mesh repair, render-distance
  caps, and fixed entity visibility budgets.
* Camera-following resident geometry: the whole packed world remains live,
  while dual display-list arenas contain only the nearby column neighbourhood.

## Hardware contract

The engine is deliberately built around predictable ceilings:

* No gameplay heap and no unbounded entity or pickup creation.
* Two fixed terrain display-list arenas permit safe incremental rebuilding
  while the RSP may still be reading the previous arena. Only a bounded
  camera-following column set is resident; no arena scales with world area.
* World access stays packed; there is no expanded byte-per-block mirror.
* Solo can spend more on atmosphere and guidance. Split-screen reduces view
  and visible-entity budgets before duplicating expensive render work.
* Generation caches only three neighbouring height rows rather than a second
  world-sized heightmap.
* Block IDs currently fill the complete four-bit terrain namespace. Future
  interactive blocks should first use compact side tables or shared visual
  families; expanding every stored block to five or eight bits must be an
  explicit memory-budget decision.

Every milestone should finish with release, audio, and SDK-debug builds plus a
linked-memory report. Real-hardware captures remain the final authority for
frame pacing, controller feel, colour, and legibility.

## Next milestones

### 1. Sensory life

Add surface-aware footsteps, landing weight, short mining transients, leaf and
ore particles, water-edge motion, distant night calls, and restrained ambient
loops. Reuse tiny deterministic particle pools and synthesized effects. Give
every action one clear visual or audible response before adding more menus.

### 2. Places, not just terrain

Divide generation into broad named regions and place small composable
encounters: ruined arches, wells, camps, exposed mine mouths, unusual trees,
and landmark chains that hint at another site. Store only what cannot be
reconstructed from the seed. The compass can briefly acknowledge discovered
landmarks without becoming a modern GPS.

### 3. A real ecology director

Keep the shared mob representation, then add roles rather than one-off AI:
herbivore, skittish, territorial, ranged, burrowing, and flying ambience.
Spawn by time, surface, light proxy, distance, and local population. A few
agents with strong reactions are preferable to many inert models.

### 4. Useful building

Add doors, beds or campfires, compact storage, and a purpose for shelter after
dark. Interactive block state belongs in bounded sparse side tables so ordinary
terrain remains four bits. Recipes should open new verbs—light, store, cook,
defend—not merely add decorative inventory rows.

### 5. Animation pass

Build a shared pose layer for anticipation, recoil, recovery, breathing,
looking, landing, carrying, eating, and tool weight. Drive it from gameplay
state and reuse the same timing in first- and third-person. Two or three
excellent key poses with strong easing will read better on N64 than dense,
float-heavy skeletal animation.

### 6. Beyond the current world data

Render geometry now streams independently of the packed 160×160 terrain. If
the world data itself later needs to grow beyond that boundary, move to
deterministic terrain-sector streaming rather than another permanent array
increase. Keep a compact edited-sector journal and reuse the resident mesh
layer already established here. This remains a separate milestone: it should
not be attempted until generation time, save size, mesh churn, and worst-case
player edits have hardware measurements.

## Definition of “more”

The game is deeper when a small rule participates in several stories: rain
changes visibility and sound, a campfire creates safety and cooking, a slime
drop changes traversal risk, or a waystone helps both navigation and lore.
Prefer those multiplying interactions over long flat item lists.
