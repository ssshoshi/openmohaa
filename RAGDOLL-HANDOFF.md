# Ragdoll physics — handoff

## Who you are

You are a ragdoll physics specialist. Your background is character physics in
shipped games: constrained rigid-body and position-based dynamics solvers, joint
modelling against real anatomy, skinning artifacts, and the practical business of
making a dead body look dead rather than look like a simulation that has stopped.

You have seen every failure mode in this document before in other engines. You
are not impressed by a solver that satisfies its own constraints; you care
whether the corpse reads correctly to a player standing over it.

Two things matter about how you work here:

- **The previous session's principal failure was tuning symptoms instead of
  modelling joints.** Read "The pattern" below before touching a constant.
- **The previous session was repeatedly misled by its own metrics.** Every
  measurement in the harness has been wrong at least once, usually by measuring
  something adjacent to what mattered. Distrust the numbers until you have
  confirmed the metric actually observes the defect you are chasing.

The user is the project owner, plays the result in multiplayer against bots, and
reports defects with screenshots. Screenshots have found three bugs the harness
could not see. Take them as primary evidence.

---

## State

Branch `feat/client-ragdoll`, 12 commits ahead of `main`, working tree clean.

```
8545b4f6 fix(cgame): stop the untwist pass turning legs inside out at the hip
33dd18be fix(cgame): hand a limb's roll reference over gradually, not at a stroke
ef3a95dd fix(cgame): write down the knee's bend plane when the leg dies straight
38a7c708 docs: record the pelvis shear, and what the harness could not see
7140cd97 fix(cgame): give the root bone its own orientation, not the pelvis's
2f0331c7 fix(cgame): limit how far the legs may open at the hip
6e019517 fix(cgame): let the knee's bend plane roll with the hip
6432589b fix(cgame): lower the shot impulse, which was throwing limbs about
9af8bec2 fix(cgame): correct sideways knee travel gently, not at full rate
20deb90f fix(cgame): defend the torso the player can see, not a thinner one
37d0daea fix(cgame): smooth only the bend of the spine, not its roll
072ee1e5 feat(cgame): client-side ragdoll physics for dead bodies
2ecc02cb build: support cross-compiling for Windows with mingw-w64
```

`code/cgame/cg_ragdoll.cpp` is ~3670 lines and contains the whole subsystem.
The commit messages are written to carry the reasoning, including what was
measured and rejected. Read them; they are the densest source of context here.

### Build and deploy

```sh
ninja -C .cmake cgame                 # native, for the harness
ninja -C .cmake-win cgame             # MinGW cross-build (toolchain in cmake/toolchains/)
cp .cmake-win/RelWithDebInfo/cgame.dll "/mnt/d/Medal of Honor/openmohaa-ragdoll/cgame.dll"
```

The DLL is locked while the game runs — the copy fails with permission denied,
which is the signal to ask the user to close it. Screenshots arrive in
`/mnt/c/Users/xxsch/AppData/Roaming/openmohaa/main/screenshots`.

---

## Design constraints (do not relitigate)

- **Client-side only.** The server has five networked bone controllers and the
  `msg.cpp` field tables are positional, so a protocol change is out. Everything
  is computed in cgame and emitted as bone overrides.
- **23 particles, position Verlet, Gauss-Seidel relaxation.** Particles carry no
  orientation. Every rotational quantity is inferred from particle positions,
  which is the source of an entire class of bug (see "Reconstruction" below).
- **The death animation still plays** and drives the particles for the first
  200 ms (`cg_ragdoll_blendtime`), then the solver takes over.
- **The killing shot pushes the body.** Impact position and direction are
  recovered from the flesh-impact messages the client already parses for hit
  sounds (`cg_parsemsg.cpp`, `CGM_BULLET_8`), so this needs nothing on the wire.
- **Explosions push it too, and differently** (`befc4f51`). A blast has no
  direction of its own, so `CG_RagdollNoteExplosion` keeps the *place* and each
  particle is pushed along its own line from it, falling off with distance. That
  falloff is what turns the body over — near side harder than far side — where
  the shot's push is mostly one velocity for the whole body. Sizes come from the
  effect the message already resolves, so both protocols share one mapping.
  Scaled by `cg_ragdoll_blastimpulse`, kept apart from `cg_ragdoll_impulse`
  because a bullet and a grenade want different numbers: the shot's energy goes
  into the limbs, so a setting big enough to throw a body from a blast leaves
  rifle deaths with a leg in the air.

Tuning is exposed through `cg_ragdoll*` cvars; `cg_ragdoll 0` restores stock
behaviour entirely. `cg_ragdoll_debug 1` draws the constraint web and prints
impact/impulse diagnostics.

---

## The pattern (most important section)

**Three separate bugs, one cause: a constraint that memorised the death pose
instead of modelling the joint.**

| bug | what was stored at death | consequence |
|---|---|---|
| torso twist | Spine2's roll reference | chest rotated away from the shoulders, ~35° |
| shin standing vertical | the knee's bend *plane*, in torso coordinates | body on its back could not lower its leg; foot 21 units up, stable, asleep |
| legs splayed in a V | the hip cone axis + foot-to-foot span | a man shot mid-stride keeps his stride forever |
| pelvis/torso shear | the root bone's orientation, taken from the pelvis | a quarter turn re-injected into the pelvis every frame, forever |

In each case the constraint was correct on the frame it was seeded and wrong
forever after, because the body's orientation relative to the world changed and
the stored quantity did not. In each case the fix was to model the actual joint
(a knee bends in a plane the *hip may rotate*; a hip abducts ~45°) rather than
to tune the stored value.

**Assume more of these exist.** Grep for anything captured in `CG_RagdollSeed`
and ask whether it is a property of the joint or a property of that one pose.
Candidates not yet examined: `spineRest` (the seeded spine curvature),
`twistRest`, `segTrunkScale`, `selfPair[].minLen`.

---

## The harness

`/home/shoshi/projects/openmohaa-ragdoll-harness/` — **preserved out of `/tmp`,
which is where it previously lived and would have been lost.**

```sh
cd ~/projects/openmohaa-ragdoll-harness
./build.sh                              # builds ./rdsim against the working-tree solver
./build.sh /path/to/variant.cpp         # or against a variant
./rdsim                                 # all 49 scenarios
RD_ONLY="anim death_run03" ./rdsim      # one
```

It links the real solver against stubbed `cgi` imports and runs it over a floor
plane, an optional box (platform or wall), and **24 real death animations**
extracted from the retail `Pak0.pk3` by `skcdump.py` and run through forward
kinematics. It is fast (whole suite in seconds), deterministic, and it is the
only reason any of this was tractable.

### Its blind spots — read this before trusting a number

- **`cg.time` is 0 on every harness run and never 0 in a game.** The impact ring
  buffer treats a zero timestamp as an empty slot, so anything recorded on the
  first frame is silently discarded. The blast buffer keeps a `used` flag
  instead; the impact one still does not. It cost half an hour of a blast that
  measurably did nothing at all.

Every one of these produced a wrong conclusion that was acted on:

- **`seed reproduction error` read 0.0 by construction, not by correctness.** It
  was sampled on the first frame, where the blend weight is zero and
  `CG_RagdollBuildPose` slerps all the way back to the animation, so it handed
  back what it was given whatever the reconstruction had done. It now gets its own
  single frame with `cg_ragdoll_blendtime` forced to 0. It found the 90° at once.
- **Neither twist metric could see a reconstruction defect.** `worstTwist` is built
  from particle positions, and the particles carry no twist at all. `worstJointTwist`
  reports the worst single step between *neighbouring* spine bones, so a turn
  applied evenly down the whole chain leaves it undisturbed. `pelvis to chest roll
  added` and `root adrift from pelvis` were added to cover both gaps.
- **The harness's own skeleton has the root and the pelvis pointing the same way**,
  so the 90° appears only on the real animations. Anything that depends on the rig
  rather than on the pose will hide in the synthetic scenarios.
- **Eight of the 24 animation scenarios were mislabelled** — the names encoded an
  older ordering of `rd_anims[]`, so `anim death_choke` was running `death_crotch`
  and seven others were similarly crossed. Names corrected; the animations run are
  unchanged.
- **`backBends`, the "knee back" column, was never incremented.** It was declared,
  printed and gated on, so `backBends == 0` passed vacuously. Now wired to the
  knee's offset from the hip-to-foot chord. It is not zero.
- **Nothing measured particles buried in the world until `bb26b744`.** The
  answer to "why is this corpse in the air" was in the collision code the whole
  time and no metric could see it. The trace's `C` record carries the contact,
  ground and buried masks now.
- **Corpses not sleeping is a cvar, not a bug.** Six traces all ran their full
  budget; the seventh showed why. `cg_ragdoll_sleepvel 0` disables sleeping by
  design, and with it set the quiet timer is reset on every frame that runs a
  physics step no matter how still the body is. Check the tuning cvars in a
  trace header before reading anything into how long a corpse simulated.
- **`limb held up by nothing` counted anything nearby as support, not anything
  underneath.** A foot floating level with the pelvis scored as resting on the hip.
  It now requires the support to lie below the limb, within 60° of straight down.
  The honest numbers are much worse than the old ones: worst over the real
  animations went from 10.1 units to 17.7, mean 2.7 to 4.3. Treat the pre-fix
  history of this metric as meaningless.
- **`bone rolled about itself` reads 0.00 on all 49 scenarios**, and always did.
  It baselines on the pose the ragdoll was handed and then reports drift from it,
  but `CG_RagdollUntwist` pins that exact quantity to the value it was handed, so
  the metric is zero by construction. It is the metric that ought to catch a boot
  drawn with its sole facing the wrong way, and it cannot. Still unfixed: the
  obvious repair, comparing against the animation instead, is the rejected
  experiment recorded above.
- **`worst roll step in one frame`** was added to catch a bone snapping about its
  own length while nothing moves. It reports the worst step and, separately, the
  worst once the body has settled — the settled figure is the one that matters, as
  large steps during a tumble are usually real motion. It found the hip
  singularity in minutes.
- **(historic, now fixed) `limb held up by nothing` counted the body itself.** A foot 21
  units in the air beside the pelvis scored **+1.2**. This is why sweeps kept
  reporting the legs were fine while the user was photographing legs in the air.
  Not fixed.
- **Only 11 animations until very late, all deaths from a standing start.** The
  animations that splay are the *running* and *prone* deaths. Adding
  `death_run01/02/03`, `death_twist`, `death_knockedup`, `death_prone1`
  immediately reproduced a defect that had been invisible for the whole session.
  The retail pak has dozens more (`death_mortar_*`, `death_grenade`,
  `death_fire`, the `*_pain_*todeath` family) that are still not in.
- **No leg-splay metric existed at all** until the last hour.
- **No vertical wall** in the world model until the last hour, though bodies die
  against walls constantly. A wall increases splay by 3–8°.
- **Aggregate means buried the failures.** 25 synthetic scenarios diluted 13 real
  ones 2:1; a change that was neutral overall was a disaster on real deaths.
  **Score real animations separately.** They are the rows beginning `anim `.
- Historic metric bugs, since fixed, that each caused a wrong shipped change:
  self-intersection baselined before the animation window; bone-roll mixing in
  joint bend and reporting phantom 180° flips; torso twist measured absolutely so
  a body dying mid-turn read as twisted.

### Where it stands, measured over 100 corpses

Traced in the game at `impulse 1 / blastimpulse 2 / stiffness 1 / limptime 1500 /
solvegain 0.7 / sleepvel 0.2`:

| | p10 | median | p90 | worst |
|---|---|---|---|---|
| settles and sleeps at | 1176 ms | **1942 ms** | 3408 ms | 8797 ms |
| residual movement, median | 0.02 | **0.10** | 0.18 | — |
| thrown (units) | 5.9 | 29 | 212 | 320 |
| turned (deg) | 16 | 70 | 92 | 124 |
| joints in contact at rest | 15 | **20** of 23 | 22 | 23 |

**97 of 97 corpses that lived long enough settled and slept.** The three that did
not are the three whose traces end inside a second, because the body was removed
that fast; they were still in mid-impact and two had not finished blending.

Three things in that data look like faults and are not, so do not chase them:

- **A corpse ending perfectly flat** (z extent under 1 unit). Collision uses one
  box radius for every particle, so a body on level ground lands with every joint
  centre at the same height. The drawn mesh still has thickness.
- **Particle distances drifting ~5% from the seed.** The drawn skeleton does not
  use them: `CG_RagdollBuildPose` rebuilds each bone from its parent with the
  offset captured at seed, so drawn bone lengths are exact by construction. This
  is the `particleCollapse` figure, not visible stretching.
- **A tall resting pose with few "supports".** `CG_RagdollSupportCount` only
  counts faces within 45° of level, and a staircase is mostly risers. A body
  draped down steps reads as 3 supports while holding 18 contacts.

### Hands in the chest — found, and it was not shape memory

`cg_ragdoll_armfree` (`61ed311f`), default **0.5**.

1000 in-game corpses: the deepest limb in the body is a **hand in Spine2** in
**26%** of them (median arm depth 0.10, p90 0.35 of a hand's thickness).

The seeded `segTrunkScale` for forearm-to-hand is **already 1.0** — letting it
"recover" is byte-identical across the suite, and the branch does run. The
nominal clearance is simply too small: the collision radius is the bone's, the
visible hand is a fist in a sleeve. So those two segments now ask for `1 + armfree`
times their clearance, ramped in on the limpness clock.

Gridded over limptime × stiffness: where active, limb-in-trunk 4/4, limb-in-limb
4/4, selfX 4/4; twist within a degree either way. **Not a trade.**

**It is inert at `cg_ragdoll_limptime 0`** — the ramp has no clock.

### The legs — confirmed in game, fixed

`cg_ragdoll_legfree` (`edc974ca`), default **0.4**. Of 295 corpses, 18 had a limb
well inside another limb and **16 were leg-against-leg**. Same diagnosis as the
hands (target too small, not enforcement: the push rate flattens above 0.2), but it
needs a **fast ramp (250 ms)**, not the limpness clock — by the time the slow ramp
is full the corpse has settled and friction holds the legs crossed. Applied
instantly it stretches the body past standing height. Grid: crossing 6/6, passes
+3/−0 at every limptime > 0. Regresses at `limptime 0`.

## ROOT CAUSE: the drawn chest is not where the simulated chest is

**This is the top open problem and it explains most of the screenshots.**

Measured over 300 in-game corpses, comparing each dump's final `P` (particle) and
`B` (drawn) records:

| | median | p90 | worst |
|---|---|---|---|
| angle: **particle** shoulder line vs **drawn** shoulder line | **24.5°** | **85.6°** | 174° |
| drawn hand / forearm distance from its own particle | 4.2–4.6 | ~14 | 22.4 |
| drawn foot / calf / head / spine2 from its particle | 1.3–1.4 | ~4 | 8 |

- **174 of 300 corpses (58%)** exceed 20° of shoulder-line rotation.
- Correlation between that angle and how far the shoulder is drawn from its
  particle: **r = 0.925**.
- Bone **lengths** match exactly (ratio 1.00) and bone **directions** match exactly
  (median 0.0°). Nothing is stretched or bent. The whole arm is correct and
  *translated bodily* — up to 21 units — because its anchor is wrong.

### Why

`rd_bones[]` (`:189`): *"The spine takes its roll from the pelvis and passes it up,
one bone to the next."* The drawn chest roll is therefore a function of the pelvis
alone and **never looks at where the shoulder particles are**. The solver is free
to rotate the shoulders about the spine axis; the drawn chest does not follow.

The arms hang off the *drawn* chest, so they are drawn wherever that rotation puts
them, while collision — which is correct, and which `armfree`/`legfree`/the
world push-out all operate on — acts on particles somewhere else entirely. Hence
arms through the ground, arms through the body, and "bent oddly", all at once, with
every skeleton metric scoring clean.

### Fixed behind `cg_ragdoll_chestroll` (`c64aad8f`), default **0**

Measured against what the uncorrected chest *would* have been, then spread a third
each across Spine/Spine1/Spine2, accumulating through the transport. The pelvis is
excluded — its hip line is a median 6° out against the chest's 24°.

Grid at 0.5 over limptime × stiffness: chest-vs-particle **4/4**, cross **4/4**,
arm **4/4**, passes unchanged **4/4**. **Drawn twist roughly doubles, 4/4.**

**That twist is real and was being concealed.** The body has it; the arms have been
paying for the concealment by being drawn in the wrong place. Whether showing it
reads better is a judgement by eye — which is why it is off by default and why the
last session to decide this from a table alone got it wrong.

New harness metric `drawn chest vs simulated chest`. Every other twist figure in
the suite compares the drawn pose **against itself** and is structurally blind to
this; that is what condemned `37d0daea`.

### CONFIRMED in game at 0.5, and the residual clipping is the same cause

300 corpses, `chestroll 0.5` vs 0: shoulder-line error **median 24.5°→19.2°, p90
85.6→57.1, worst 174→86**; corpses past 20° **174/300→145/300**. Body-clipping
metrics are now clean: hand/forearm-in-trunk **0.00 including the worst case**,
limb-in-limb worst 0.35 (0.99 four batches ago). The user's "arm through body"
class disappeared from the screenshots entirely.

**Crucially, the added drawn twist did not draw complaints** — 1–2 of 10 shots,
versus 8 of 18 for arm-in-body before. The concealed-twist trade is worth taking.

**The remaining ground clipping is residual chest error, not the particle radius.**
For the three photographed cases the drawn arm sits **4.1–5.9 units below its own
particle in z**, while the particle rests correctly ~2.9 above the surface — the
particle is on the floor and the drawn arm is through it. Forearm and Hand offsets
are *identical*, i.e. the whole arm translated rigidly. Do not chase `rd->radius`
for this.

**Use 0.75.** Harness: chest 26.9→23.4 with **no scenario lost**; 1.0 reaches 21.5
but fails `anim run01, wall` and adds noticeably more twist.

### The correction must be CLOSED LOOP (`f1ec09c2`), default **1.0**

Open loop at 0.75 left the shoulders a median **14.2°** out in game and **17% of
hands drawn >3 units below their own particle** — through the floor, since the
particle rests <3 above it.

**Two passes sit between the frame this measures and the positions the renderer
gets: the seed `correction[]` and `CG_RagdollSmoothSpine`.** An open loop modelling
neither leaves most of the error whatever the gain. So it reads back the shoulders
*as actually drawn last frame* (`rolledAxis[Spine2]` + the arms' `localOffset`) and
corrects what it finds, at gain 0.35 — driven whole it hunts and the corpse shivers.

Grid at 1.0: chest-worst **4/4**, **chest-at-rest roughly halves 4/4** (20.2→7.3,
18.2→11.2, 21.8→13.9, 14.8→7.8), cross **4/4**, arm **4/4**, **+1 scenario pass**.
Closing the loop removed the scenario loss that made 0.75 the open-loop ceiling.

### In game at 1.0: median fixed, tail was the clamp (`RD_MAX_CHEST_ROLL` 60→90)

200 corpses. Shoulder-line error **median 7.7°** (24.5 → 19.2 → 14.2 → 7.7 across
chestroll 0 / 0.5 / 0.75 / 1.0). Past 20°: **58% → 21%**. Arms drawn >3 units below
their particle: 17% → **12%**.

But **p90 is still 49°**. The clamp never bound for the typical corpse so it never
showed in the median — and it bound on exactly the minority left visibly wrong. Of
11 photographed corpses, 5 had a drawn limb >10 units from its particle (max 19).
At 90: chest-at-rest **4/4 better**, chest-worst **4/4**, arm **4/4**, 9 passes (best
recorded). 120 is worse at rest — 90 is a ceiling, not a direction.

## Bodies collide with each other and pile up (`cg_ragdoll_bodypush` 0.35)

`d71eb6d1`. Trunk + limbs as one list of **12 capsules**; capsule-vs-capsule after a
bounding-sphere reject. Trunk measured across its **narrow** axis — this pass cannot
know which way two men met, and asking half a torso's *width* leaves a visible gap.

**A sleeping corpse is immovable and takes none of the push.** That is most of what
makes a heap (the body on the floor is what the next lands on) and it is the cheap
half; waking it gives a pile that squirms. Two awake bodies each give half.

**Sleep gate had to change.** A body on a heap could never sleep: the sleep test asks
for *contact*, contact comes from tracing the *world*, and there is no world under a
man lying on another man. `onBodyMask` joints now count as supported — **upward
pushes only**, or two bodies propped against a wall each decide the other is the floor.

### Piling: works when bodies MEET, fails when they SPAWN overlapped

Measured over 100 in-game corpses by cross-referencing dumps (they share a clock and
world coordinates, so pairs alive at the same instant can be compared directly):

- **3 pairs came to rest 0.5–2.8 units apart** — one man lying through another. All
  were consecutive dump numbers, i.e. deaths in the same place within ~100 ms.
- Reproduced in the suite: `RD_PILE=8` (spawn overlapped) moves them **not at all**
  (2.5 → 2.3). `RD_PILE=40` (dropped from above) works: **1.0 → 4.0**.

Cause: the blend drags every particle back toward the animation it died in, and the
bone sticks re-assert after, so the in-solve push gives its ground straight back.

**REJECTED: a post-solve body push-out.** The world equivalent carries `pPrev` along
so it adds no energy; for bodies that kills the very speed that would carry them
apart. The working case regressed 4.0 → 2.6 and one body was thrown **64 units** from
its origin.

### BUG (fixed, `9dc33b5f`): `onBodyMask` cleared after it was set

It was cleared inside `CG_RagdollCollide`, which runs **after** the solve that fills
it in. Every mark was wiped before anything read it: **zero of 100 corpses ever
reported being held up by another**, so none could sleep on a heap. Cleared at the
top of `CG_RagdollStep` now.

### Spazzing on geometry: cause found and addressed (`6f3fffe9`)

Over 200 corpses: those still moving after 2 s have a bone inside the world for a
**median of 50 frames**; those that settle, **0**. The three photographed had joints
buried in solid for 26 / 50 / 79 frames with movement still spiking 1.2–3.3 units a
step at the end of life.

**The fight is between the push-out (which frees the joint) and gravity plus the
constraints (which put it back).** Neither wins; they trade the limb to and fro for
the corpse's whole life, and the motion keeps resetting the quiet timer so it can
never sleep.

A joint buried for **30 steps (half a second)** is now left where it is and held
there, and counts as support — a limb wedged in a step holds the body up more firmly
than the floor does. *A limb resting inside a step looks wrong; a limb shivering
inside a step looks broken.*

### SETTLED: blasts are NOT the cause (measured, not proxied)

The `K` line answers it directly. Of 200 corpses, **22 were blown up**:

| | restless after 2 s |
|---|---|
| blown up (22) | **13.6%** |
| not blown up (178) | **14.6%** |

Of the restless corpses, **10% had been blown up against 11% of the population** — no
enrichment at all. The user's impression was confounded by most deaths being
explosive. **Do not revisit.**

### The world, by contrast, is overwhelming

| | restless after 2 s |
|---|---|
| a bone inside geometry (70) | **30.0%** |
| clear of it (130) | **6.2%** |

Worst corpse was inside the world on **721 of its 722 frames**.

### CONFIRMED in game: holding stuck joints works (`cg_ragdoll_stuckhold` 0.1, ON)

300 corpses, `stuckhold 0.1` vs 0:

| | before | after |
|---|---|---|
| still moving after 2 s | 14.5% | **8.3%** |
| …with a bone in geometry | 30.0% | **22.1%** |
| …clear of geometry | 6.2% | **2.0%** |
| residual motion p90 / worst | 0.109 / 1.201 | **0.042 / 0.413** |

**The hanging risk did not materialise.** 24 of 300 rest on <8 contacts, but **22 had
traces under 200 frames** — men removed before ever settling. Only 2 were long-lived
with few contacts. Defaulted on.

**The suite is wrong about this one** and says so loudly (−2 wall scenarios): it has
no scenario where a joint stays buried for the half second the rule waits, so it
measures the cost and never the benefit. Trust the 300 bodies.

### NEXT: a late rotation burst, unrelated to geometry

**8 of 249 corpses turn more than 90° after 1.2 s** (worst 158°). They overlap with
the restless ones — but the two the user photographed (dump69, dump189) have
**`Lframes` 0 and 3 buried frames**, i.e. essentially no geometry contact.

Not a tumble: median per-frame turn is **0.00°**, then dump69 turns 11–12° per frame
for three frames at t≈1.46 s. An isolated lurch in a settling body.

The `R` line shows the chest correction **responding** (roll 25°→41.6° chasing errors
of 11–17°), not causing it. Something moves the *particles'* shoulder line in a body
touching nothing. **Unexplained — this is the open thread.**

### Leaving a stuck joint alone is not enough — it must be HELD

`6f3fffe9` stopped the push-out fighting a buried joint. That did not help in game
(14.5% restless, no better): the push-out stopped freeing it, but **gravity and the
bone sticks went on moving it every step**, so the trading continued.

Held properly — invMass scaled down *and* skipped by the integrator — **every scenario
settles**: residual motion 0.047 → **0.0000**, zero non-settling, best ever recorded.
Also best `cross` (−5.42) and `arm` (−46.71).

**`cg_ragdoll_stuckhold`, default 0 (off), because holding a joint still is how a
corpse comes to hang off one.** A hand stuck in a wall leaves the body dangling: the
suite reports *limb held up by nothing +14.4 units* and two wall scenarios flip to
FAIL. Twitching and dangling are both wrong; which is worse is for the eye. **0 is
byte-identical to shipped.**

*Note: `onBodyMask` is still 0 across 200 corpses — but no two corpses came near each
other in these rounds, so this is not evidence the piling is broken.*

### The blast hypothesis is NOT supported (by the proxy available)

The user's impression was that spazzing follows explosions. Launch speed (peak move
in the first 300 ms) says no:

- hardest-launched quarter: **14.3%** restless; gentlest half: **14.9%**
- restless corpses' median launch **10.7** vs **11.3** for everyone else
- one of the three photographed launched at **6.0**, which is nothing

Likely confounded by most deaths being explosive in these rounds. The trace now
carries a **`K` line** (blast, radius, speed) so the next batch answers it directly
rather than by proxy.

### Spazzing on geometry is confirmed and measurable

Corpses still moving after 2 s have **median 51 frames with a bone inside world
geometry**; corpses that settled have **0**. dump42: 314 frames, residual 0.32.
This is the same class as dump44 (472 frames) and dump106 (105). **Still the largest
open defect**, and unrelated to body collision.

### The suite can now simulate two bodies (`RD_PILE=<height>`)

It only ever ran one man, so none of this was testable. `RD_PILE` drops a second body
from that height onto the first and reports closest approach.

| bodypush | closest approach |
|---|---|
| 0 | **1.5 units** (one lying through the other) |
| 0.2 | 1.9 |
| 0.35 | **5.1** |
| 0.6 | 6.5 |

Nothing is thrown (furthest particle from origin unchanged). A lone corpse is
**byte-identical**. Second body costs ~1/7 of the first.

*Caveat: `anim death_chest` fails with or without the pile — pre-existing, not caused
by this.*

## Corpses react to shots and explosions (`cg_ragdoll_shove` 1.0, `18daeb50`)

**Bullets are never traced against a corpse.** `CG_RagdollNoteBullet` is handed the
line the bullet *actually took*, after the fact, from `CG_MakeBulletTracerInternal`,
and moves whatever it passed through. Nothing about what a bullet hits changes.

That is the design, not a shortcut: **corpses exist only on the client**, so a body
that stopped bullets would stop them on one machine and not another, and a player
could be sheltered by a corpse his killer cannot see.

Explosions previously were recorded *only for the man about to die in them*.
`CG_RagdollNoteExplosion` now also throws the bodies already lying there.

### Waking a settled corpse — three things it needs

1. **`wakeUntil`** — the lifetime cap would otherwise re-sleep a body past its 5 s on
   the very frame it is hit. A shove buys `RD_WAKE_TIME` (2.5 s) against it.
2. **`sleepDrop` carried across** — a sleeping body rides its entity down as it sinks
   while its particles stay put (`pinsleep`). Without carrying the sink, a woken
   corpse jumps back up to where it stopped simulating.
3. **`stuckMask` cleared** — whatever just hit it may have freed a stuck joint.

### Harness can fire at corpses now: `RD_SHOVE="ms x y z kind"`, `RD_SHOT="ms x1 y1 z1 x2 y2 z2"`

| | pelvis moves |
|---|---|
| grenade 40 units away, 4 s after death | **12.8** |
| round through the pelvis | **2.9** |
| round passing 30 units away | 0.1 |

An untouched corpse is **byte-identical**. *Aim carefully when testing — the first
bullet test read 0.0 because the line missed the body (it rests at z≈3, not z=20).*

## WARNING: batch-to-batch variance is ~4 points. Calibrate claims against it.

Two consecutive 300-corpse batches at **identical settings**:

| | batch B | batch C |
|---|---|---|
| still moving after 2 s | **8.3%** | **12.7%** |
| …with a bone in geometry | 22.1% | 28.2% |
| …clear of geometry | 2.0% | 4.6% |
| residual p90 / worst | 0.042 / 0.413 | 0.077 / 0.806 |

**Nothing changed between them.** Different rounds, different maps and situations.

This means the `stuckhold` result (14.5% → 8.3%) is **not established**: the claimed
effect (~6 points) is comparable to the noise (~4). It may still be real — the
mechanism is sound and the direction agrees — but a single before/after batch pair
cannot show it. **To settle any in-game effect, run two batches back-to-back in the
same session, toggling only the cvar.** Do not compare across sessions.

## Radii: head + forearms + hands (`cg_ragdoll_jointsize` 2, default)

Extended after a corpse was photographed with an arm through the ground while every
measurement came back clean — never touched geometry, settled, drawn within 2 units
of its particles. **The bone was where it belonged; the sleeve was in the floor.**

Grid: arm **4/4**, twist 3/4, cross 3/4, **+2 passes, none lost** (one config's worst
crossing 76% → 8%). Contrast: *all* joints is a clear loss (7 break, 2 fix) because
the trunk figures are half a torso's width.

## SOLVED: the arms were inheriting a welded shoulder (`cg_ragdoll_shoulderslack` 6)

`85d5419e`. The single biggest win of the project. Three measurements found it:

1. **Decompose the shoulder error.** The part rolling the spine *can* remove is
   median **1.1°**, p90 7.1 — the chest correction is finished. What remains is a
   **tilt out of that plane**: median 9.1°, p90 **42.5°**, which rolling can never
   reach. That is why the loop reported converged while arms stayed wrong.
2. **Walk the drift out from the root.** Whole spine tight (p90 3–4 units, pelvis to
   head, thighs too). **UpperArm alone is p90 10.4, worst 17.7.** One step.
3. **Walk down the arm.** Shoulder, elbow and hand are displaced *identically* to two
   decimals; error added shoulder→hand is median **+0.00**. The arm adds nothing —
   it inherits everything from the shoulder.

Cause: shoulders hang off Spine2 by an offset captured at death, welded into the
dying pose, while the simulated shoulders keep moving. A real shoulder girdle slides
over the ribs. They may now move up to 6 units toward their particles, clamped.

Grid: chest-at-rest **4/4, 3–4× better**; chest-worst 4/4; cross 4/4; arm 4/4;
twist 3/4; +1 pass. **9 is better again on all of those** if 6 proves too little.
Only `selfX` (the old joint-distance metric, superseded by `cross`) worsens.

### Two hypotheses tested and killed on the way

- **Roll error accumulating down the arm** — no: the chain adds +0.00.
- **Shoulders swapped by the line-symmetry wrap** (180° ambiguity; worst error 17.7
  ≈ the 18-unit shoulder span, which looked damning) — no: **1 corpse in 100** is
  swapped, and of the 24 worst only 2 would improve. Test before believing.

## VERDICT: per-model collision meshes are NOT worth building

Asked directly; answered with measurements rather than opinion.

**The cheap approximation of a collision mesh already fails to show a clear win.**
Every world trace uses one radius for all 23 joints (`rd->radius` = 3.0 × bodyScale)
while `jointRadius[]` — already measured, already per-model — sits unused for this.
The mismatch is real and large: head 4.5, pelvis 5.5, thigh 5.5 against 3.0.

- **All joints, per-joint radii: clear net regression.** 7 scenarios break, 2 mend.
  The trunk figures are half a *torso's width*, right for holding another limb off
  and far too much for holding a body off the floor — an inflated pelvis perches on
  ledges it should roll off.
- **Head only** (`cg_ragdoll_jointsize`, `5fb94c24`, **off**): gridded over 4 configs
  — twist 3/4, chest-worst 3/4, both limb metrics 2/4, **−1 pass in 3/4**. One config
  showed cross 12.7→3.3; the other three say that was luck.

If the *cheap* version is a draw, an authored mesh per model — vastly more work, and
needing format and asset pipeline changes — cannot be justified. **A convex hull is
also largely wasted on this solver**: collision here is a box trace at a point, so a
hull collapses back to a radius. Hulls pay off with rigid bodies, contact manifolds
and friction, which is a rewrite.

### On Source/VPhysics: concepts transfer, numbers do not

Their ragdolls are rigid bodies with mass, inertia tensors and convex hulls, solved
with impulses. Ours is 23 point particles with distance constraints, solved
positionally. Damping/friction/stiffness figures have no counterpart.

The useful insight is what it names: **our limbs have no rotational inertia at all** —
a bone is two point masses on a stick, so its resistance to being spun about its own
length is zero. Every corrective pass here (untwist, chest roll, spine twist limit)
is a hand-built substitute for angular dynamics the representation does not carry.
Closing that properly means rigid bodies, i.e. a rewrite — not a tuning pass.

We have independently converged on the shape of a Havok ragdoll constraint anyway:
swing cone plus twist limit. The cones were always here; the twist limit was missing
until `f7e0d20f`.

### DO NOT auto-tune parameters against these metrics

Kept because the argument is worth more than the experiment that prompted it.

The chest correction **was** an automated closed loop, and it drove its own reported
error to a median of **0.2°** while emitting a pose **89°** wrong (`b3348cfd`). An
optimiser does not avoid that failure — it is the fastest route to it, because it
finds the blind spot in the objective and optimises into it, confidently.

The metrics here have been wrong about ten times: radii copied from the solver's own
table, median-shift feature ranking that picked a non-discriminating feature, a
"penetration" figure that was a geometric constant. Every real defect was found by a
person looking at the screen; the suite passed throughout and largely could not have
found them — its world is a floor and a few walls, its arms average 78° of fold, and
its poses are tame enough that a *wrong* reconstruction of the shoulder line agrees
with the right one to four figures.

Automate collection, parsing, population statistics and outlier ranking. Keep the
suite as a gate that **blocks**, never a target that **guides**. There is no ground
truth to diff against for HL2 — encode its *properties* as invariants (settles fast,
stays put, limits respected, no interpenetration) rather than treating any number as
a target.

## Target: Half-Life 2 quality (user's words, and they say it is close)

Useful frame for what is left. HL2 ragdolls are Havok constraint bodies whose
distinguishing properties are: **real per-joint angular limits**, bodies that
settle fast and stay put, limbs with heft rather than rubberiness, and poses that
read as *dead* — slumped and relaxed — rather than posed.

Measured against that, the gaps in order:

1. ~~No torsional limit on the trunk~~ — fixed, `cg_ragdoll_spinetwist` (`f7e0d20f`).
2. **Limits taken from LIVING anatomy.** A living maximum is reached by pulling with
   muscle; a corpse has none. The elbow fix (150°→120°) is the template — **the knee
   is still at 135° (`0.383f`) and is the obvious next one.** Same for the neck.
3. **Drawn-vs-particle displacement tail** — p90 ~15 units, worst 21. The limb is
   drawn away from where it collides. Median is solved; the tail is not.
4. **Corpses fighting the world** — dump44 spent **472 frames** with 5 bones inside
   geometry. Distinct from static clipping; nothing addresses it yet.
5. Cone limits are still seeded from the death pose (the memorisation pattern).

### The trunk had NO torsional stiffness (fixed, `cg_ragdoll_spinetwist` 45°)

Every brace holding the shoulders is **symmetric about the spine axis**, so rotation
about it changes none of their lengths. A distance constraint anchored on an axis
cannot resist rotation about that axis at any stiffness. The chest was free to wind.

Diagonal braces (the textbook fix for racking) moved it **0.3°** and cost 2 passes —
a diagonal resists a little at every angle; a joint needs nothing until a limit and
then firmly. So the angle is measured directly and the shoulders turned back about
the spine, 25% of the excess per iteration, with the **hips as anchor** (their drawn
line is a median 6° out against the chest's 24°).

Grid at 45°: twist **3/4**, chest-at-rest **3/4**, chest-worst **3/4**, −1 pass in one
config. `anim death_twist` — named for exactly this — goes FAIL→PASS.

### The elbow was folding to a LIVING arm's limit (fixed, `120°`)

The 150° stop is AAOS **voluntary** maximum flexion — biceps pulling. A dead arm
has nothing to pull with. At 150, **26% of corpses rested within 5° of the stop**,
folded flat, and **5 of 8 photographed corpses had an elbow past 144°** — one
labelled by the user "bent in a little too far", two more reading as "arm through
torso", which is where a flat-folded arm puts the hand.

Now `0.500f` (=cos(60°), 120°) on both `RD_?UARM→RD_?HAND` rows.

**The suite cannot see this and did not vote on it.** Harness arms average 78° of
fold; every metric is identical at 150 / 130 / 120 / 110. Free there, worthless
there. Evidence is 100 in-game bodies. *Consider the knee (135°, `0.383f`) next —
same argument, no complaint yet.*

### UNPROVEN: `RD_MAX_CHEST_ROLL` 60→90 in game

The grid supported it (chest-at-rest 4/4). In game across the next 100 corpses the
median held at ~8° but **p90 went 49→59 and past-20° 21%→24%** — i.e. no measurable
tail improvement, possibly noise at n=100 across a different round. Do not cite it
as a win without a controlled batch.

### Diagnosing from unlabelled screenshots: what worked

`cg_ragdoll_dumplabel` digits are readable when cropped by colour
(cyan ≈ RGB 51,229,255) and enlarged — the seven-segment decode is legible by eye.
Automated segment OCR was **not** worth it (resampling dims the 1px strokes below
threshold).

**Ranking features by median-shift/σ produced a false lead**: elbow fold scored
highest (flagged 145° vs rest 90°) but does **not** discriminate — 100/200 corpses
are past 110° and it catches 6/11, an in-set rate of 6–8% against a 5.5% base. A
bimodal distribution makes median shift meaningless. **Always convert a candidate
to precision-against-base-rate before believing it.**

Real (if modest) discriminator: drawn-vs-particle gap >10 units — 17% in-set vs 6%
base, catching 5/11.

Real but non-predictive population fact: **32% of corpses rest with an elbow within
5° of its 150° limit.** Worth revisiting on its own; a corpse does not hold maximal
flexion.

### PROVEN: `CG_RagdollUntwist` cannot fight the chest correction

Positions are hung off `rolledAxis` — the axes **before** untwist (`:4048` comment).
That pass changes how a bone is *drawn*, never where the bone below it *goes*.
Driving its target by 45° failed **all 49 scenarios** and moved the chest
measurement by **exactly 0.0**. Don't spend time here again.

### Next: corpses that fight the world

`Lframes` separates them cleanly. dump106 (*"spazing out"*) 105 frames over 6 bones;
dump95 (*"contorted"*) 119 over 7. Meanwhile the photographed *static* clips have
`Lframes` **0**. Two different bugs; this is the one left.

### Both new limits verified in game

Elbow: median **115°**, p90 120.7, worst 123.7 — was median 114, p90 150, worst 153
with **26% pinned at the stop**. Spine twist: particle trunk twist p90 **44.8°**
against the 45° limit. Both doing exactly what they were built to do.

### REJECTED: fading the twist limit on the limpness clock

dump22 (*"spasm and moved"*) writhes: its trunk twist wanders **34°–62° for the whole
late phase**, drifting 0.17°/frame and never arriving, mean 52.7° against a 45° limit
— permanently active, permanently losing. The feedback path is real: turning the
shoulders turns the torso frame, the cones and hinges are expressed in that frame,
they move the limbs, the limbs pull the shoulders back.

Fading the limit out as the body settles made **everything** worse — twist 21→32,
chest 20.8→26.7, and residual motion 0.071→**0.109** with more non-settling
scenarios, i.e. worse at the very thing it was meant to fix. Reverted.

`RD_SPINE_TWIST_RATE` sweep (0.15/0.25/0.40/0.60) gives **no consistent direction**:
0.15 has the best settling mean and pass count but costs limb-in-limb 12.7→19.2, and
the settling mean is driven by 3 outlier scenarios (`nonzero` is 3–4 at every rate).
**The suite cannot resolve this.** Left at 0.25.

### RESOLVED: the loop was converging on its own reconstruction

The `R` line settled it. Over 100 corpses the correction reported a **median error
of 0.2°, p90 1.9°** — converged — while the *emitted* shoulders were up to **89°**
from the particles (dump47: reported 0.1°, actual 89.3°). Only **10/100** ever
touched the clamp, so **the clamp was never the problem** and raising it 60→90 was
chasing a phantom.

Cause: it rebuilt the drawn shoulder line from `localOffset × rolledAxis` instead of
reading the emitted `bonePos`. **A loop that measures its own reconstruction drives
the reconstruction to zero and leaves the pose where it was.** Now reads `bonePos`
(`b3348cfd`), which cannot fail that way.

**The suite cannot tell the two apart** — every figure identical, because in a tame
pose the reconstruction *is* right. A negation probe is useless here (the shoulder
line is a line; ±180° wraps to the same value); use an asymmetric bias (+20°) to
prove such a path is live.

### Corpses were not sliding — the ground was (`cg_ragdoll_pinsleep`, on)

3 of 6 photographed corpses had "slid across ground". The trace: **entity origin
travelled ~141 units while the ragdoll's own `maxdisp` read 0.00.** A sleeping body
emits frozen *model-space* matrices, so it rides its entity wherever the server
takes it. Riding it *down* is the point (that is how a corpse sinks and is removed);
riding it sideways is not. The sleeping pose is now rebuilt each frame from the
world positions it settled on, offset only by how far the entity has sunk.

### The clamp: harness and game disagree — was a symptom, see above

Harness prefers higher monotonically (chest 22.8/12.6 at 45 → 20.8/10.4 at 90).
In game the shoulder-error tail has gone **p90 49.4 → 59.3 → 64.7** since raising it.
One cross-round comparison, so not conclusive either way — do not flip the constant
on it. The trace now carries `R <chestrollfix> <chestrollerr>`, which distinguishes
*saturating at the clamp* from *not converging*. **Next batch settles this.**

### REJECTED: relaxing the cone axes toward anatomical neutral

**This was the handoff's long-standing top hypothesis. It is wrong — do not retry.**

The cones are narrow (hip 30°, shoulder 45°) and centred on the direction the limb
died in, so a man shot mid-stride keeps that stride. Tried easing `coneAxis` toward
an anatomical neutral (legs down the body, arms down and slightly out) on the
limpness clock.

- **Sprawl, the thing it was predicted to fix, did not move: 2.50 → 2.48.**
- At full relax it **breaks 6 scenarios to fix 1** (passes 8→6).
- The breakages are the *running* deaths (`death_run01`, `run01 wall`, `death_back1`)
  — and that is the lesson: **a man shot mid-stride really does have his legs apart.**
  The death pose is not all error. Pulling the cone to neutral fights real posture.

### Item 2 does NOT generalise to the knee

The elbow fix worked because arms are light and get folded flat by impacts: median
fold 114°, p90 150°, **26% pinned at the stop**. The knee measures median ~30°, p90
50–98° against a 135° limit — **it never approaches it**. Tightening it would be
copying the shape of a fix rather than its reason.

### REJECTED: diagonal trunk braces

`LTHIGH↔RUARM` + `RTHIGH↔LUARM` at 0.94–1.06. Reasoning was sound — every existing
brace (shoulder↔shoulder, hip↔hip, pelvis→shoulder) is **symmetric about the spine
axis**, so rotation about it changes none of their lengths, and the trunk has *no*
torsional stiffness at all. A diagonal is the ordinary way to brace against racking.
**It moved chest disagreement by 0.3° and cost 2 scenario passes.** Reverted.

Open question this raises: with `chestroll 0` the drawn chest is offset even when
physical twist is low, which suggests much of the disagreement is baked into the
seed `correction[]` at death rather than accumulated live. Worth testing before
attacking the physics again.

### The fix, and why the earlier attempt failed

`37d0daea` tried anchoring Spine2's roll to the shoulder line and was reverted: it
doubled the *drawn* twist metric (15°→35°) and pinched the mesh, because it put the
entire pelvis-to-shoulder discrepancy into one joint (`:191`).

**Distribute it, do not anchor it.** Compute the roll discrepancy between the
pelvis-transported frame and the actual shoulder line, then spread it evenly across
pelvis→spine→spine1→spine2 so each joint takes a quarter. That gives the chest the
right orientation without the hourglass pinch the comment describes.

The metric that condemned `37d0daea` compared **drawn against drawn** and could not
see this; the measurement above compares **drawn against particle**. Re-judge that
revert on the new measurement, not the old one.

Ship behind a default-off cvar and grid it. This is the code the project's worst
regression came from.

### TRAP: changing a cvar default does NOT reach the user

Every ragdoll cvar is `CVAR_ARCHIVE`, and
`%APPDATA%/openmohaa/main/configs/omconfig.cfg` pins each one with `seta`. A saved
value **always wins over a new default**. An entire 300-corpse batch was collected
believing it tested `armfree 1.0` when the config held it at 0.5; the trace header
is what caught it. **Always read the header before analysing, and ask the user to
set the cvar explicitly rather than relying on a default.**

### The extremities: radius, not the segment gap

The push-out names the right bone in 5 of 8 photographed world-clipping corpses.
The 3 misses (dump10 *"right hand clipping step"*, dump248, dump125's left hand)
have **`Lframes` 0 and `buriedJoints` 0** — neither the joint nor the bone middle
was ever inside solid, yet it visibly clips.

`rd->radius` is a single `3.0f * bodyScale` for *every* particle, ignoring the
per-joint `jointRadius[]` that already exists, and it is smaller than a visible
hand or boot. **Same mesh-vs-capsule mismatch as the torso** (see `armfree`).
Untested: raising it also lifts the body off the floor, since resting contact
planes come from the same traces. Do not conflate that with anything else in one
batch.

### MEASURED: a third of all corpses had a bone inside the world

The `L` line (`9f6ab87a`) settles it: **96 of 293 corpses** had a limb bone found
inside world geometry. Legs slightly more than arms (R Calf-Foot 49, L Thigh-Calf
41, L Forearm-Hand 36 …). Previously invisible to every measure.

**"Body freaking out" == fighting the world.** dump157 (*"body freaked out"*) has
**328 frames** with a bone in the world, dump18 137. The limb re-enters as fast as
the push-out frees it. That fight is the next thing to look at after twist.

### The capsules are narrower than the body, and the mesh is wider still

`cg_ragdoll_armfree` **1.0** (`6f9ef801`), raised from 0.5. Eight of eighteen
labelled screenshots were an arm inside the torso and **all eight scored clean on
every measure**. dump125 settles why: an officer in a leather greatcoat whose arm
leaves his body at the waist, not at a shoulder — the bone is outside the capsule
and the sleeve is inside the coat.

So **a negative reading on the capsule metric is correct, not excessive**: an arm
"clear of the trunk by 14% of its thickness" is resting on the coat. Grid at 1.0:
arm 4/4, cross 4/4, selfX 4/4, twist 3/4, +1 passing scenario at the played
settings. Costs 2 passes at lt1500/st0.4 (a corner nothing sets). 1.25 starts to
stand the arms off the body.

### The world was never tested against a bone, only against a joint

`CG_RagdollCollide` boxes each **particle** from `pPrev` to `p`. Nothing tested the
bone *between* two joints, so a forearm sits inside a beam with the elbow out one
side and the hand out the other and no trace objects — the identical blind spot
self-collision had. `CG_RagdollLimbPushOut` (`9f6ab87a`) samples each limb bone at
0.35 and 0.65 and puts it back on the surface.

**This is a structural gap, not a metric result** — the suite is unmoved because its
world is a floor and a few walls, where a joint catches whatever the bone would.
The new trace `L` line names which bones were found inside the world; the drawn
pose cannot show it, since the bone is put back before anything reads it.

Labelled screenshots (`cg_ragdoll_dumplabel`, `daa6d9b2`) made this findable: 3 of 7
were an arm sunk into ground or timber, and every skeleton measure scored those
bodies clean.

### Twist is confirmed as what "bent all weird" means

dump192, labelled *"body bent all weird"* by the user, carries **48.2°** pelvis-to-chest
twist — 4th of 295. The torso faces one way and the pelvis is wrung the other.
**38 of 295 exceed 30°, 10 exceed 40°.** This is the strongest validated link between
a number and the user's eye. Next target; see the untwist-decay plan below.

### DO NOT use "lowest joint below origin" as a penetration measure

It conflates a body underground with a body that fell a long way from its entity
origin. Dumps 203/208/137 read −313/−193/−93 while resting on 19–21 contacts with
zero sink. Use the solver's own `buried` mask and the new `L` line instead.

### DANGER: do not grade the solver with the solver's own ruler

The in-game metrics took their radii from `rd_joints[]` — **the solver's own
table**, whose comment says the trunk values are *"deliberately smaller than half a
torso's width."* So the metric cannot report an arm inside the visible chest: it
agrees with the solver about where the chest is. Shoulder-to-shoulder measures
**18 units**; the table's chest radius is 4.8.

Compounding it, the `ATTACH` rule excuses limb-vs-trunk pairs at the attachment
joint — correct for a shoulder resting on the chest, and it excuses an arm driven
*through* the chest along with it.

**Screenshots showed arms through the torso that every skeleton metric scored
clean**, including a from-the-shoulder-down sample against a realistic chest
radius. Remaining hypotheses, in order:
1. **Skinning, not the skeleton.** Twist p90 is 32.5°, worst 47.5°; linear-blend
   skinning collapses at that and produces exactly the fused/through-the-body look.
   The bones can be correct while the mesh is not. **This is the lead.**
2. **Sampling.** `RD_MAX_OPEN_TRACES` is 4, so in a busy round most deaths are
   never traced. The dumped corpses are a biased sample: the ones that died when a
   slot was free.

### Two more metrics that were measuring nothing

- **In-game "limb in trunk" was junk.** It swept all 8 limb segments and was
  dominated by calf-near-pelvis, which is geometric: p10 0.26 → p90 0.36, a
  constant, not a defect. It could never respond to any lever, which is why the
  reverted radius change read "0.33 either side". Split it: **arms** (median 0.10,
  spans 0→0.47, discriminates) and legs (median 0.27, still pinned).
- The harness `armInTorso` was right all along — forearm/hand vs trunk at *full*
  radii, the visual bar, not the solver's allowance. Trust it over the in-game one.

### Limb inside limb — real, was invisible, now optional

Self collision works on joint distances, so two bones crossing at their middles
with all four ends apart were never seen. The harness measures it now
(`limb inside another limb`): it runs at **30–40% of a limb's own thickness**
across the real deaths.

`cg_ragdoll_limbpush` (`ff1c49a4`) is the rate; **0 is the shipped behaviour**.
Gridded over six stiffness × limptime combinations, 0.1 improves limb-in-limb in
5 and limb-in-trunk in 5 (once 38% → 26%), and raises twist in 5. Confining it to
the legs keeps the twist but barely moves the crossing — most crossing is arms,
and an arm hangs from the top of the chest, so pushing one levers the torso.

That is a trade to judge by eye, which is why it is a cvar and not a default.

### Arms in the chest: what else was tried

**Measure any change here over a grid of `stiffness` × `limptime` before
believing it.** The metric is the mean over scenarios of each scenario's *worst*
case; it swings between 18 and 39 across ordinary settings, and an effect of 3
does not survive that. A change validated at one or two settings means nothing.

That is not hypothetical. `a73de289` raised `limbRadius` from 0.85 of the joint
to 1.0, `RD_TRUNK_DEEP_RATIO` from 0.90 to 1.0 and lowered `RD_SEGMENT_RATE` to
0.25 to match, and measured well at the two settings it was tried on. Over nine
combinations it reduced the limb in the trunk in four and raised it in five,
and made twist worse in eight. Reverted in `2eecab16`. A thousand corpses from
the game put the median at 0.33 either side of it.

The reasoning still looks right — defending an arm thinner than the arm being
drawn ought to be wrong — which is worth remembering as a warning about
reasoning: whatever it is worth is smaller than the noise, and the twist it costs
is not.

Measured and rejected:

| tried | result |
|---|---|
| Letting the seeded trunk clearance recover as the shape memory fades | **catastrophic.** Every scenario fails: the solver shoves limbs out of a body that has already settled. arm 32 → 42, particle distortion 14 → 44, twist 5.6 → 17. The concession is not only about the first frame |
| Raising `RD_SEGMENT_MIN_FRACTION` 0.70 → 0.80, or the 0.85 in the clearance seeding → 0.95 | **no effect whatever, byte for byte.** Arms are not touching the chest at the instant of death, so the seeded fraction is already 1.0 and neither path is reached. The penetration is *acquired* during the fall, at full asked clearance |
| `RD_SEGMENT_RATE` 0.5 and 0.7 | worse on clearance *and* on gate passes |
| `limbRadius` 0.85 → 1.0 with `RD_TRUNK_DEEP_RATIO` → 1.0 and rate → 0.25 | wins on 4 of 9 settings, loses on 5, worse twist on 8. Shipped and reverted |
| `RD_TRUNK_WIDE_RATIO` 1.10 and 1.20 | lower average, worse worst case: 68 → 99. Past 1.0 the torso asks for more room than it occupies |
| `cg_ragdoll_iterations` 10 → 14 | worse (passes 9 → 5) |

**The interaction worth knowing:** `limptime 1500` nearly doubles arm-in-trunk
(18 → 32) because the shape-memory springs that were holding arms off the chest
let go. Anything measured for this must be measured at both settings.

### Measured over 1000 corpses — the current baseline

`impulse 1 / blastimpulse 2 / stiffness 1 / limptime 1500 / solvegain 0.7 /
sleepvel 0.2`. 991 parsed.

| | p10 | median | p90 | p99 | worst |
|---|---|---|---|---|---|
| settles at | 975 ms | **1987 ms** | 3801 ms | 5042 ms | 11050 ms |
| residual movement | 0.03 | **0.10** | 0.18 | 17.21 | 36.90 |
| contacts at rest | 17 | **20** of 23 | 22 | 23 | 23 |
| limb inside the trunk | 0.24 | **0.33** | 0.36 | 0.38 | 0.62 |
| **joint past its range** | 0.00 | **0.00** | 0.00 | 0.00 | **0.00** |

**937 of 991 settle.** Of the 54 that do not: 42 are traces under a second where
the entity was removed, 9 were still settling when the trace was cut, 2 were
stuck holding two or three contacts, and 1 fell out of the world. That is three
corpses in a thousand with anything actually wrong, and no corpse in a thousand
folds a joint past its anatomical range.

There is no systematic defect left in this data. Anything further is taste.

**Sinking is not a thing — do not chase it.** It reads p90 6.7 units measured
over the whole trace (that is bodies *falling*), and still 0.58 measured over
quiet supported frames (those frames are not contiguous, so the body moves
between them). Over an *unbroken* quiet run the worst offenders descend 0.00 and
2.01 units. Four separate metrics have now been caught measuring something
adjacent to what mattered; assume the fifth is too until it survives that kind
of check.

### Measured over 349 explosive deaths

`impulse 1 / blastimpulse 2 / stiffness 1 / limptime 1500 / solvegain 0.7 /
sleepvel 0.2`. 323 of 349 settled and slept, median 1.9 s, 20 of 23 joints in
contact, residual movement median 0.10.

**No hanging corpses at rest.** Seven looked like hangers and all seven were
either draped along stairs, holding contacts down their whole length, or still
in flight when the trace ended.

Resting pose, measured properly (limb *segments* against trunk *capsules*,
excluding limbs that grow out of the trunk segment they are tested against):

| | p50 | p90 | worst |
|---|---|---|---|
| limb inside the trunk, as a fraction of clearance | 0.33 | 0.36 | 0.41 |
| joint folded past its anatomical range | 0° | 0° | 4° (one corpse in 323) |

The trunk figure looks worse than it is: `segTrunkScale` deliberately asks for as
little as 70% of the ideal clearance where a pair starts close, so a steady ~33%
shortfall is the design tolerance rather than clipping. **Measure this with
segments and exclude attached pairs**, or it reads 0.00 (joint centres only) or
0.53 (counting a thigh overlapping its own pelvis) — both were measured on the
way to the number above.

Of the 26 that never slept: 19 are traces under a second where the entity was
removed, 3 were still restless, 1 fell out of the world (pelvis z 177 → −6120),
and 2 were wedged with only one level contact — fixed in `5e1166cf`.

### Stress tested: 100 corpses, sleeping switched off

A busy round at `sleepvel 0`, so nothing was frozen and every corpse simulated
its full budget:

| | p10 | median | p90 |
|---|---|---|---|
| residual movement | 0.01 | **0.03** | 0.16 |
| joints in contact at rest | 16 | **20** of 23 | 22 |
| turned (deg) | 24 | 67 | 86 |
| sink over the settled half | 0.00 | **0.00** | +0.50 |

96 of 100 settled. The four that did not are traces of 10 and 25 frames — 81 ms
and 144 ms — where the entity was removed before the body had finished blending;
their large numbers are a corpse still in mid-flight, not a fault.

The churn fix holds: re-seeds within 3 s of a trace ending went from **143 to 4**,
and median trace length from 993 ms to 8002 ms. Whole-body burial still happens
(one corpse in a hundred had its pelvis and both thighs inside geometry for the
whole trace) but it no longer sinks, which is the upward fallback doing its job.

### A corpse gets one ragdoll, and losing it is final

Recycling a slot does not end the story for the body it was taken from: that
corpse is still lying there dead, so it asks for another on the next frame,
takes one from a different settled corpse, and that one asks in its turn. Every
re-seed starts from the death animation's pose, so what the player sees is
corpses flickering and resetting for as long as the round stays busy.

Found only because the trace count did not match the body count: 200 captures
from 52 bodies, one re-seeded 12 times, each new trace starting a median of 21 ms
after the last ended. **If dumps ever outnumber deaths again, suspect this.**

`CG_RagdollWasEvicted` now refuses a second ragdoll; the record clears in
`CG_RagdollEntityReset`, when the entity number is reused by a different body.

### cg_ragdoll_maxcount caps *solving*, not corpses

Sleeping bodies do not count against it (`c88…`, see git log). They cost nothing:
the step is skipped and they re-emit the matrices they settled on. Counting them
made a busy round evict corpses seconds after they settled — traced over 200
deaths, 117 of the 167 that settled were thrown away within half a second of
stopping, the median on the very frame it stopped. If corpses ever start
vanishing or snapping back to their death pose again, look here first, and check
`used` against `solving` in `CG_RagdollAlloc`.

### Tracing costs file handles, and the engine has few

`cg_ragdoll_dump N` holds one open file per corpse being traced. Asking for a
hundred in a busy round took every handle the engine had and killed the server
with `FS_HandleForFile: none free`. At most four traces are open at once now, a
capture that cannot get a handle is not counted against N, and a trace lets go
three seconds after its corpse sleeps. **Do not raise `RD_MAX_OPEN_TRACES`
without knowing what the engine's handle budget actually is.**

### Tracing a corpse from the game

The harness cannot reproduce a real map. `cg_ragdoll_dump 1` (cheat) captures the
next body to fall into `ragdoll_dump.txt` in the home path and clears itself, so
one command gives one corpse. Read it with `rdtrace.py` in the harness directory:

```sh
./rdtrace.py ragdoll_dump.txt            # summary, worst roll step per bone
./rdtrace.py ragdoll_dump.txt --bone 13  # one bone frame by frame
```

Each frame carries the particles, the pose the animation would have drawn, and
the pose actually drawn. The particles hold no orientation, so **a bone that turns
while they sit still is the reconstruction, not the physics** — the reader flags
exactly that, and it is the distinction almost every defect here has turned on.
`RD_DUMP=1 ./rdsim` makes the harness write the same format, which is how the
reader is tested.

### Useful technique

Ablation. Disabling one constraint pass at a time (`CG_RagdollHinges`,
`CG_RagdollCones`, `CG_RagdollSelfCollide`, `CG_RagdollSegmentCollide`) and
tracing one joint's position over time found both leg bugs in minutes after
weeks of parameter sweeps found nothing. Add a `printf` behind a `getenv` in a
scratch copy of the solver; do not add debug code to the repo file.

---

## Rejected experiments — do not repeat

With numbers, because several of these look obviously correct.

| tried | result |
|---|---|
| Spine2 roll referenced to the shoulder line | real-animation twist **15° → 35°**. Shipped, then reverted. This was the session's worst regression. |
| Scaling hinge lateral slop by body scale | looks like an oversight; measured worse (selfX p90 40 → 55) |
| Velocity-neutral correction for levitation | far worse (fidget 0.448 → 3.24, passes 14 → 3). Wrong theory. |
| Sprawl fade | shipped with no sprawl metric, user reported worse, reverted |
| Seed-timing fix | net negative once isolated; removed entirely |
| Raising knee/elbow extension limits (0.985 → 1.0) | legs are not pressed against the limit; no effect, real passes 6 → 3 |
| Limb-vs-limb segment collision | genuinely missing (shins *can* pass through each other) but trades legs for arms at every rate: worst arm-in-trunk 57% → 96% |
| Distributing the hinge correction across the joint chain | the "principled" momentum-conserving fix; worse than either alternative (sprawl 2.65 → 2.82, floating limbs 6 → 9) |
| Fading the hinge in as the body settles | knee leaves its plane during the fall and nothing can undo it afterwards; out-of-plane ~6.0 at every threshold |
| Widening the hip cone (30° → 90°) | no effect on knee fold; the cone constrains the hip, the fold is the knee |
| Tightening the foot-to-foot span | never binds; legs reach only 23–28° in the scenarios that have it |
| Disabling spine smoothing entirely | mean twist better, worst case 94° → 162° |
| Measuring drawn limb roll against the animation's current pose | looks like a defect metric and is not one. The foot topped every scenario at up to 69°, which turned out to be the animation's *own* ankle roll between the frame the ragdoll took over and the frame it ends on: `death_prone1` rolls its ankle −69.2° by itself, `death_collapse` +31.6°. The drawn foot was holding its handoff value faithfully. Removed. |

Also worth knowing: **bend-only spine smoothing failed the first time** (a 166°
flip in `death_back1`) and succeeded later unchanged, because the flip was caused
by an unrelated bug — two bone-table rows sharing the pelvis particle, so it was
driven twice per substep. A cloud code review flagged that as a *nit*. If an
experiment fails on one scenario with a wild outlier, suspect a second bug rather
than the experiment.

---

## Open problems, ranked

### 1. Pelvis/torso shear — found and fixed, worth re-checking by eye

Screenshot 63 showed the lower body rotated ~90° from the torso with flat grey
sheets at the thigh and knee. That was a real 90°, and the reconstruction was
putting it there: `CG_RagdollSeed` took each bone's seed correction from its
*joint* rather than from the bone, and `Bip01` and `Bip01 Pelvis` share a joint.
The root was drawn with the pelvis's orientation, the quarter turn between them
went into `twistRest`, and `CG_RagdollUntwist` re-injected it every frame. Fixed
in `7140cd97`; `seedErr` over the real animations goes 90° → 0, drawn torso twist
10.4° → 3.5°.

**Get a screenshot before assuming this is closed.** The harness says the shear is
gone; whether the corpse now reads right to someone standing over it is a separate
question, and the remaining `pelvis to chest roll added` is still 2.7° mean / 11°
worst.

### 2. Arms clipping into the chest

Worst limb sits 57–92% of its own thickness inside the trunk depending on
scenario. The trunk is modelled as a chain of capsules with an elliptical cross
section (`RD_TRUNK_WIDE_RATIO` / `RD_TRUNK_DEEP_RATIO`, now 1.00/0.90 after
raising them from 0.75/0.65 — the solver was defending a volume much thinner than
the torso being drawn). Raising further is worse, not better.

Note the interaction: the Spine2 change that fixed clipping *caused* the torso
twist. These two are coupled through the reconstruction, so a real fix for (1)
may move this on its own.

### 3. Knees out of plane

Worst 12–14 units on the newly added running/prone animations, against a gate of
5. This is the cost of the two leg fixes and has not been paid down.

### 4. Folded elbows still flip, same cause as the hip did

`CG_RagdollTwistBetween` carries the parent's roll onto the child along the
shortest rotation between their directions, and that rotation does not exist when
the two run opposite: every axis across the parent turns one onto the other and
each answer is half a turn from the last. `8545b4f6` took the thighs and upper
arms out of the untwist pass because their roll is already decided geometrically
and they sit on that singularity permanently — a thigh runs down the leg while
the pelvis runs up the spine.

The forearms are still in the pass, and a fully folded elbow puts the forearm
opposite the upper arm, which is the same singularity. Worst settled flip over the
real animations is 75° on `Bip01 L Forearm` in `anim wall, collapse`. Fading the
correction near the singularity was tried for the hip case and measured *worse*
than doing nothing (mean step 66.8 → 70.6); don't reach for it again without a
better reason than it being the obvious move.

### 5. Joints buried in world geometry — fixed, but still frequent

A particle whose trace starts inside a brush used to be dropped back where it
was, i.e. back inside, every step for ever, anchoring the corpse in mid air.
`bb26b744` places it on the surface instead, traced from the joint it hangs
from. Worst floating limb over the real deaths 17.7 → 8.7 units, and the deaths
against a wall 17.7 → 5.5.

**Why it happened is now known and fixed** (`c5040211`): the step ended on the
constraint solve, which honours only planes already *remembered*, so a limb
meeting a wall for the first time was pushed into it and the step ended there.
A push-out pass now runs last. On flat ground burial went from 91 frames in 93 to
1 in 70.

Against a wall the count stays high (257 of 259) and that is the metric's
wording, not a fault: `buriedMask` counts push-out events, and a limb *resting*
against a wall is pushed out on every step by design. Read it as "was inside at
the end of the solve", not "is stuck".

**The ragdoll and its entity end up far apart.** Measured in the game, the pelvis
settles 200–224 units from the entity origin on a staircase, because the corpse
is simulated on the client while the entity falls on the server and the two take
different routes. What is drawn is the ragdoll, so it looks right, but the entity
is what culling, sound and removal use, and once the body sleeps the frozen
matrices ride it. Nothing has gone wrong from this yet — in every trace the
entity has moved 0.0 units after sleep — but it is the mechanism behind any
corpse that pops, vanishes early, or jumps as it is removed.

### 6. Remembered contact planes — expiry added, unproven in the harness

A trace hit is remembered so the solver can re-project against it without
tracing again, and `CG_RagdollProjectContacts` honours it every iteration: a
particle can never pass below a plane it remembers. The only release was
drifting 16 units *above* it, and **a particle hanging underneath a plane never
rises above it**. A corpse was found dangling upside down in open air, held by
contact planes at two toe tips, its entity long since on the floor below.

`69791931` asks the world: a plane unconfirmed for 250 ms gets a short trace
along its own normal. Age alone is not the test — a settled particle does not
move, so its sweep strikes nothing and going unconfirmed is normal; expiring on
age was measured and is ruinous (fidget 0.03 → 1.29, half the suite failing).

**The harness cannot show this working.** Its world is a floor and a box, where
surfaces do not disappear; the probe fires a few dozen times per corpse and drops
a plane twice in the whole suite. Every metric is unchanged within noise. The
evidence for the fix is the game trace, not the harness — which is the shape of
most of the remaining problems here.

Confirmed in the game afterwards: four corpses traced on the map that produced
the hanging one, all four settling and sleeping, none hanging, and burial down to
9–88 frames of 835 from the 370-of-400 it used to run at.

### 7. Shape memory: found in the game, fade added, value not settled

`CG_RagdollSolveConstraints` pulls every soft constraint back to the distance it
had at death, for ever. Traced from the game (`ragdoll_dump6`), a corpse on a
staircase held its hip within a few degrees of 90° and its knee of 50° for five
seconds, and its waist was driven to 29° by the impact and pulled back to 8°.
It balanced on head, neck and shoulder with the centre of mass 4 units off that
base. A mannequin balances there; a body folds.

`cg_ragdoll_limptime` (`f206e45b`) fades the bias, leaving every hard limit. It
**ships at 0**, because the numbers do not pick a value:

| limptime | 0 | 750 | 1500 | 3000 |
|---|---|---|---|---|
| floating limb, worst | 17.7 | 9.9 | **6.6** | 27.2 |
| floating limb, mean | 4.3 | 1.2 | 1.3 | 4.2 |
| torso twist, mean | **3.5** | 7.4 | 6.9 | 5.4 |
| sprawl, worst | **3.4** | 5.3 | 4.2 | 4.6 |
| synthetic passes | 11 | 8 | 13 | 14 |

1500 is the best measured value. It buys the biggest improvement anyone has got
on limbs left hanging in the air and costs a doubling of twist, which is the
thing the user says reads worst. Settle it by eye, in the game, and do not sweep
it: 3000 is *worse than off* for floating limbs, so the curve is not monotonic
and a sweep will mislead.

### 8. The rest of the death pose is still memorised

The shear was one instance of a pattern that is still everywhere. Every rotational
quantity in the drawn pose is a constant captured at death — `twistRest`,
`correction`, `spineRest`, `coneAxis` (whose own comment says "how far a limb may
swing **from the direction it died in**"), `hingeBendRest`, `selfPair[].minLen`,
`segTrunkScale`. The particles carry no orientation, so nothing in the simulation
can ever revise any of it: a man shot mid-stride keeps his stride, his back's arch
and his limbs' heading for the whole five seconds.

The fix shape is the one that worked for the knee and the hip — give the joint a
range and let the value relax toward it, rather than holding what the animation
had. Worth doing behind a cvar so it can be judged by eye. Re-open the Spine2 roll
decision *last*: `37d0daea` records that squaring it against the shoulders doubled
torso twist, but that was measured with the 90° shear present.

### 9. Two rules about a contact plane must agree about distance

A plane is kept until the particle drifts `RD_CONTACT_FORGET` (16 units) clear of
it, and the probe that asks whether the surface is still there must reach at
least that far. It used to trace a fixed short distance, so anything drifting
between the two numbers was reported as a vanished surface and dropped
(`b0ac9e1`-era bug, fixed). The symptom was a corpse that never slept: a hand
lost its plane every 250 ms, fell, caught the step again, and the four frames of
movement reset the quiet timer just short of the 400 ms it needed.

If either constant is ever changed, change the other with it.

### 10. Fidget, and why the obvious fixes for it fail

A Verlet integrator cannot tell a position correction from a velocity, so every
constraint the solver satisfies hands the body a little motion it never had.
The damping that takes that back out is the same damping that stops a corpse
sliding, which is why fidget and the "corpses stop dead on stairs" complaint are
one problem and not two. Measured over the suite, worst late movement is 2.77
units a step, on `anim wall, right` and `anim death_prone1`.

Tried and rejected, with numbers:

| tried | result |
|---|---|
| Holding velocity explicitly, and choosing how much of a solve counts as motion | **worked**, `76b38c00`. See above |
| Coulomb friction (fixed speed off per step) instead of multiplicative | **runaway.** A fixed subtraction cannot bound a solver that adds every step; a corpse on flat ground span up like a turntable, a toe sweeping 5–6 units a step and *rising*. Shipped as `24897ebf`, reverted as `4c3e01e5` |
| Coulomb plus a viscous term to bound it | at every strength that stops the runaway the body slides *less* than with no change at all: 0.90 gives fidget 3.65 / travel 68, against 2.77 / 100 for leaving it alone |
| Damping only the motion that disagrees with the rest of the body, keeping the shared slide | fidget 0.436, travel 808. The premise is wrong: a *spinning* body is coherent too, so this cannot tell a slide from a spin |
| Making `CG_RagdollProjectContacts` velocity-neutral | fidget 0.50 → 0.71 and particle distortion worst 15% → 98%. The handoff already recorded this once; it fails the same way |
| Recording the contact plane in the push-out pass | halves fidget (0.23 → 0.13) and costs twist (3.0° → 4.3°) and self-intersection. Parked: twist reads worse than fidget |

**Done, in `76b38c00`.** Each particle now carries its velocity in units per
second; `pPrev` means only where it was when the step began, and nothing writes
to either to mean something else. How much of the solver's correction counts as
motion is `cg_ragdoll_solvegain`, default 0.7.

Worst residual movement in a settled corpse 2.77 units a step → 0.99, average
0.23 → 0.06, and the sweep is well behaved from 0.9 down to 0.6 rather than one
lucky value. At 0 the body cannot learn it has been corrected and falls apart,
so the useful range is the top of the scale.

Still open: **`cg_ragdoll_solvegain` trades against twist.** Mean drawn twist is
3.0° at gain 1 and 4.1° at 0.7, while worst-case twist goes the other way, 17° →
15°. If twist starts reading badly, raise the gain before touching anything else.

**Sleeping is what arrests the last of it, and it must be left on.** The residual
that survives the velocity rewrite is not velocity at all: it is Gauss-Seidel
drift, the solver nudging a position a little the same way each step. The
settling pass zeroes the particle's velocity correctly and the drift continues
regardless, so no velocity fix reaches it. Sleeping does.

Confirmed in the game. With `cg_ragdoll_sleepvel 0` a corpse crept about ten
units over three seconds, walked its own arms off the step propping it up, lost
those two contact planes and collapsed — a jump of 11.06 units a step, nearly
five seconds after death. With the default 0.25 the same class of body sleeps at
1.8–4.4 s and holds; over five traced corpses the worst late movement fell from
11.06 to 1.41 and four of the five froze and stayed frozen for the rest of their
lives. The fifth was removed by the server while still legitimately sliding.

### 11. The gate now disagrees with what corpses are supposed to do

`lateMove < 0.5` requires a body to have stopped by frame 150. Since `24897ebf`
a corpse on a slope steeper than its friction angle correctly keeps sliding, so
it fails that term for doing the right thing — `slope 35 deg` travels 324 units
where it used to travel 11. Three of the gate's lost passes are this, not
regressions. Either exempt scenarios with a sloped floor, or measure late
movement as *acceleration* rather than speed.

### 12. Only 9 of 24 real animations pass their full quality gate

The gate is a composite of ~16 thresholds, so one bad number fails a scenario.
Treat it as a screen, not a score, and look at the individual metrics.

---

## Working agreements with the user

- **Twist bothers them more than self-intersection.** Stated explicitly. The
  previous session traded the wrong way once and it was the worst regression.
- They test in multiplayer against bots and send screenshots. Ask for one when
  stuck — it has been more informative than any sweep.
- They will tell you when the game is closed so the DLL can be replaced.
- `run-openmohaa.sh` in the repo root is their launcher, deliberately untracked.
- Do not chase "shootable/explodable corpses" — explicitly descoped.

## One honest note to carry forward

Progress in this session came almost entirely from three things: **ablation**
(disable one pass, see what changes), **tracing a single joint over time**, and
**the user's screenshots**. It came almost not at all from parameter sweeps, which
consumed most of the effort and produced most of the wrong turns.

If you find yourself sweeping a constant, stop and ask what the joint is
physically supposed to do instead.
