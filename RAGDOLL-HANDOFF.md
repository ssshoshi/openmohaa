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

### Arms in the chest: what worked and what did not

Three numbers decide it. `limbRadius` (was 0.85 of the joint), `RD_TRUNK_DEEP_RATIO`
(was 0.90) and `RD_SEGMENT_RATE` (was 0.35). The first two were smaller than the
body they stand for; the third has to come *down* when they go up, or the
clearance is paid for in particle-cloud distortion. Together (`a73de289`): deepest
limb 18.0% → 16.8%, worst 68 → 61, selfX 22.1 → 20.7, and one more real death
passing its whole gate.

Measured and rejected:

| tried | result |
|---|---|
| Letting the seeded trunk clearance recover as the shape memory fades | **catastrophic.** Every scenario fails: the solver shoves limbs out of a body that has already settled. arm 32 → 42, particle distortion 14 → 44, twist 5.6 → 17. The concession is not only about the first frame |
| Raising `RD_SEGMENT_MIN_FRACTION` 0.70 → 0.80, or the 0.85 in the clearance seeding → 0.95 | **no effect whatever, byte for byte.** Arms are not touching the chest at the instant of death, so the seeded fraction is already 1.0 and neither path is reached. The penetration is *acquired* during the fall, at full asked clearance |
| `RD_SEGMENT_RATE` 0.5 and 0.7 | worse on clearance *and* on gate passes |
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
