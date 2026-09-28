# ragdoll-harness

A headless test bench for the client ragdoll (`code/cgame/cg_ragdoll.cpp`). It
compiles the solver as-is against stubs for the engine, drops a body in each of
about fifty scenarios (flat floor, slopes, ledges, walls, beams, 17 real death
animations) and prints what the body did: bone stretch, self-overlap, knee and
elbow ranges, whips, stretch, spring off impacts, how deep it sank into the
floor, how far it slid, whether it passed.

```sh
cd tools/ragdoll-harness
./build.sh                               # builds ./rdsim from this tree's cg_ragdoll.cpp
./rdsim                                  # every scenario
RD_ONLY="anim death_run03" ./rdsim       # one
./build.sh ../../some_variant.cpp && cp rdsim rdsim-variant   # A/B a change
```

The first build generates `rd_anims.h` from the retail `Pak0.pk3` (set
`MOHAA_PAK0`; the default is `/mnt/d/Medal of Honor/main/Pak0.pk3`). The
animations are the game's own and are not part of the tree. Don't run two builds
at once in this directory: they share `solver.o`.

## The Jolt ragdoll

`./build.sh --jolt` builds `./rdsim_jolt`, which runs the same scenarios with the
Jolt ragdoll (`code/cgame/cg_physics_ragdoll.cpp`, `cg_ragdoll_solver 1`):
the particle solver carries the body through the blend out of the death
animation, as in the game, and Jolt carries it from there, in a world built
from each scenario's floor and ledge or wall (`rdjolt.cpp`). It links the Jolt
library of a CMake build of this tree (`JOLT_LIB`, default
`build/linux/libJolt.a`, so build the tree for Linux first).

`RD_SOLVER=0 ./rdsim_jolt` runs the particles in the same binary, and matches
`./rdsim` line for line. `scripts/compare.py` runs both and sets them side by
side: how many scenarios each fails and on which check, and the median of each
measure (`-v` for every scenario). `RD_CVAR` passes through, for tuning.

```sh
./build.sh --jolt
scripts/compare.py
RD_ONLY="anim death_run03" ./rdsim_jolt
```

## Knobs

Environment variables, all optional:

| Variable | What it does |
|---|---|
| `RD_ONLY="name"` | run one scenario |
| `RD_CVAR="name=value;..."` | override any ragdoll cvar for the run |
| `RD_DUMP=1` | write `ragdoll_dump_N.txt`, the same trace `cg_ragdoll_dump` writes in game |
| `RD_STARTZ`, `RD_STARTXY="x y"` | where the body starts |
| `RD_BOX="minx miny minz maxx maxy maxz"` | reshape the solid box of a ledge scenario (beams, slabs, overhangs) |
| `RD_HANG="ms lift joint"` | grab a joint from above at ms, lift it and hold; `RD_HANGSWING="amp period"` swings it, `RD_HANGDROP=ms` lets go |
| `RD_DRAG="ms joint dx dy dz dur"` | grab and drag: level first, then down |
| `RD_PATH="ms joint dx dy dz ..."` | grab and move through waypoints, a second each, then let go |
| `RD_PUNT="ms pitch"`, `RD_WALLDRAG="ms joint"` | punt, or drag into a wall and release |
| `RD_PILE=height` (+ `RD_PILEXY`, `RD_PILEPUNT`, `RD_PILEDELAY`) | drop a second body on the first |
| `RD_SHOVE`, `RD_SHOT`, `RD_BLAST` | a shove, a bullet, an explosion |
| `RD_HALFFRAME`, `RD_STEPDBG`, `RD_STDBG`, `RD_DRAGDBG` | sub-frame updates and per-step debug prints |

Sleep is off unless asked for (`RD_CVAR="cg_ragdoll_sleepvel=0.25"`), since a
frozen body cannot show what is being measured. The log (`cg_ragdoll_log`) goes
to stderr.

## Scripts

`scripts/` holds the batteries used while tuning. Each takes the binary to run
(default `./rdsim`) and prints totals, so two builds compare line for line:

- `drag.sh`: 18 bodies dragged off a ledge by the chest; leg jumps and jerk.
- `hook.sh`: bodies lifted out from under an overhang; worst bone stretch.
- `beamdrop.sh [binary] [height]`: 45 drops onto beams; spring, whips, bodies left hanging.
- `embed.sh`: bodies starting partly inside a beam; the kick out of it.
- `burycount.sh [binary] [height]`: frames with a joint or limb middle buried in the beam drops.

And readers for dump files, the harness's or the game's:

- `whip.py dump`: steps where a joint moves more than 6 units off the body's motion.
- `ev.py dump threshold t0 t1`: big joint moves, with buried joints and limbs.
- `cv.py dump t0 t1`: the body's velocity per step, and the fastest joint.
- `drops.py dump`: landings, and how much the body springs off them.
- `bone.py dump a b t0 t1`, `jv.py dump joint t0 t1`: one bone or one joint over time.
- `../rdtrace.py dump`: bones that turn while their particles stay still (reconstruction faults).

Always measure the noise first. Nudge a constant by a hundredth of a percent and
rerun; many single-scenario numbers swing that much on their own. Compare sums
over a battery, not one run.
