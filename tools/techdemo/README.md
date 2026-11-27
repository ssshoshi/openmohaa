# m0_techdemo

A map to look at and measure the fork's features on, generated from Python
and compiled with MOH:AA's own tools.

| Zone | Where | For |
|---|---|---|
| Lighting hall | west | 16 coloured lamps and two spots among pillars: realtime lights and their shadows; three lights moving among them (`techdemo_movers 0` stops them) |
| Physics room | north | crates, chairs, tables, bottles, buckets and a ramp: physics props and impact sounds |
| Firing range | north-east | 8 passive soldiers and gas barrels (`func_barrel`) behind sandbags: gore, blasts, ragdolls |
| Gallery | east | 12 wall textures that have material maps, lamps raking down them: normal and parallax maps |
| Yard | middle | one of every human model in the stock paks (54; `techdemo_crowd N` for N of them): the cost of a populated scene |
| Field and pond | south | grass for the ground cover, trees, and a pond with dead men floating in it: water, ragdoll buoyancy |

West and east of those, behind hill ridges with a pass through each, are six
biomes, one for each kind of place the game's levels are set in. Their
buildings, textures and models are the ones those levels use most (counted
from the retail BSPs). Every building can be walked into and has a lamp lit.
`biomes.py` makes them.

| Biome | Where | Like | Buildings |
|---|---|---|---|
| Norway | north-west | m2l1, m2l3 | two-storey concrete research block, bunker, corrugated shed, wooden hut; pines, sandbags, wire |
| Ardennes | west | m6l1, m6l2 | two snowed half-timbered houses (one roof shelled open) on a village street, bunker; dense winter pine forest, rocks |
| Bocage | south-west | m3l2, m3l3, m4 | stone farmhouse, plank barn, stone chapel with pews; hedgerows on earth banks, oaks, a dirt lane |
| North Africa | north-east | m1l1 to m1l3 | flat-roofed plaster houses with parapets and mosaic bands (one two-storey), corrugated motor pool shed; palms |
| Omaha | east | m3l1 | casemate and two pillboxes on a grassy bluff over sand and sea; hedgehogs, mined poles, ramps, wire |
| Town | south-east | m5l1 to m5l3, m4l0 | two rows of two-storey houses on a cobbled street (one shelled), a larger plastered house, field stone walls |

The passes from the middle are at the plaza's and field's west and east edges:
`tele` to these to start in a biome (just inside each pass).

| Biome | tele |
|---|---|
| Norway | `tele -4544 2880 48` |
| Ardennes | `tele -4544 250 97` |
| Bocage | `tele -4544 -2400 50` |
| North Africa | `tele 4544 2300 43` |
| Omaha | `tele 4544 250 188` |
| Town | `tele 4544 -2400 46` |

The sun, sky and water settings are the map's, so they are the same in every
biome: the snow zones are snow on the ground, not a snowfall, and the sea is
drawn with the pond's water settings.

## Building

    tools/techdemo/build.py --fast                 # about 45 min: rough vis and light
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

The biomes are drawn at the size the retail levels draw each texture
(`texscale.py`, `tex_scales.json`: measured from the retail BSPs), lined up
with what they're on (tiles start at a building's corners and floors). Their
ground is blended by decals laid on it (`ground.py`): soft-edged patches of
a second ground, the ground over the foot of its cliffs, faded road strips.
Their pictures are made by the build from the game's own textures, so
nothing retail is in the repository; they are most of the pak's 40 MB.
Buildings (`kit.py`) have working doors (`func_rotatingdoor`), glazed
windows that break (`func_window`), and rooms furnished to each model's own
bounds (`propbounds.py`, `prop_bounds.json`), so nothing clips a wall, a
window, a door's swing or anything else.

Fitting the biomes in took raising three of the compiler's limits:
- brush sides (131072): the ground merges flat stretches into bigger cells
  and the decals are patches, not brushes;
- faces it merged and then dropped (holes in flat ground): `-nomerge`, and a
  unit or so of noise on the ground's heights;
- MOHlight's lighting (8 MB, 170 lightmaps): the build makes a copy of
  MOHlight.exe with room for 256 (`mohlight_patch.py`: the array moved to a
  new section, never the MOHTools original), and the ground and buildings'
  outsides take coarser lightmap texels (`surfaceDensity`) than rooms.

The compilers don't get the trees pak (`zzzzzzzzz-opm-trees.pk3`): its trees
are many times denser than retail's and MOHlight lights static models vertex
by vertex, so the biomes' pines alone made the light pass an hour longer.
They are lit as the retail trees, like every retail map, and the game draws
the new ones over them. The light pass is still most of the build: the map's
lit area is over twice what it was before the biomes.

- `mapgen.py` writes .map files: brushes from corners (the winding is worked
  out), heightfields as triangular prisms, entities.
- `techdemo.py` is the layout of the middle (plaza, rooms, field), `biomes.py`
  the six biomes round it and the kit their buildings are made with.
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
