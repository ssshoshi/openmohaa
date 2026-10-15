# m0_techdemo

A map to look at and measure the fork's features on, generated from Python
and compiled with MOH:AA's own tools.

| Zone | Where | For |
|---|---|---|
| Lighting hall | west | 16 coloured lamps and two spots among pillars: realtime lights and their shadows |
| Physics room | north | crates, chairs, tables, bottles, buckets and a ramp: physics props and impact sounds |
| Firing range | north-east | 8 passive soldiers and gas barrels (`func_barrel`) behind sandbags: gore, blasts, ragdolls |
| Gallery | east | 12 wall textures that have material maps, lamps raking down them: normal and parallax maps |
| Yard | middle | a fixed crowd of soldiers (`techdemo_crowd`, default 24): the cost of a populated scene |
| Field and pond | south | grass for the ground cover, trees, and a pond with dead men floating in it: water, ragdoll buoyancy |

## Building

    tools/techdemo/build.py --fast                 # about 25 s: rough vis and light
    tools/techdemo/build.py                        # full vis and light
    tools/techdemo/build.py --install "/mnt/d/Medal of Honor/orch-test/home/main"

It needs the MOHTools release (`MOHTOOLS`, default `/mnt/d/MOHTools-master`:
`Q3map.exe`, `MOHlight.exe`) and the game (`MOHAA_GAME`, default
`/mnt/d/Medal of Honor`). The map is compiled against every pak in the game's
`main`, mods included, so their shaders and textures are the ones it is lit and
flagged with. The compilers stop at a shader file the game would read past
(HRRTM's `scripts/new_opelgray.shader` starts with a stray `}`), so they run in
`<game>/techdemo-build`: hard links to the game's paks, and a pak sorting last
with a repaired copy of each such file. That pak also holds the retail image of
each of the map's textures a mod replaces: the compilers work texture
coordinates out from the image's size, and the HD pack's images are four times
retail's, which made every texture four times too big. Compiled against the
retail sizes, as the retail maps were, the HD images draw at the retail scale. The result is
`build/techdemo/zzzzzzzzz-opm-techdemo.pk3`: the bsp, the map's script,
precache and water file, and nothing retail or from a mod.

- `mapgen.py` writes .map files: brushes from corners (the winding is worked
  out), heightfields as triangular prisms, entities.
- `techdemo.py` is the layout. Change the map there.
- `pak/` holds the files that go into the pak as they are.

The ground is brushes, not patches or LOD terrain, because the ground cover
reads brush triangles and terrain only. The map's name starts with `m` so the
single player sound aliases load (they match on the map name's start).
Retail light values (20 to 40) suit rooms a few hundred units across. These
rooms are bigger, so `LIGHT_SCALE` turns every lamp up.

## Playing and benchmarking

    devmap m0_techdemo

With `techdemo_bench 1` set before the map loads, a camera flies the same path
round every zone at 256 units a second, lap after lap (113 s a lap). The
console prints `techdemo: bench lap start` at the start of each lap. With
win-bench:

    OMBENCH_MAP=m0_techdemo OMBENCH_CVARS="techdemo_bench 1" tools/win-bench/bench.sh

The pk3 must be in the bench install's `main` (or its home's).

The pond's water is the water program's (`water/m0_techdemo.txt`), so it needs
a build with it. Without one the pond is the retail water shader.
