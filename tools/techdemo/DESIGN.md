# Designing levels that look like the retail ones

What the retail campaign is built like, measured and looked at, and what
that means for the generator (biomes.py, kit.py, ground.py). This is the brief
to read before generating anything new from a description or a picture.

## How it was learned

- **Looking:** `survey/views.py` puts 12 viewpoints in each retail map. It
  clusters the AI path nodes (the walkable places) and stands at each
  cluster's middle, eye height. It faces the longest clear sightline,
  traced through the map's brushes and LOD terrain with `survey/bsp.py`, so a
  shot shows a street or a yard, not the wall beside it.
- **Shooting:** `survey/sweep.py` holds the orch-test camera at each view
  and makes one contact sheet per map. The pictures are retail content, so
  they stay outside the repository, in `D:\Medal of Honor\design-ref`
  (`retail/`, `td_v1/` for the techdemo before this pass).
  `views_techdemo.py` does the same for the techdemo's six zones, so the two
  can be put side by side.
- **Measuring:** `survey/grammar.py` reads every campaign BSP. It records:
  - which wall texture stands on which, and how tall the lower one is;
  - which static models stand within 160 units of each other;
  - how far each model is from its nearest neighbour.
- **Checking a build:**
  - `survey/cracks.py` reports any ground vertex that lies on another
    triangle's edge but off it in height.
  - Clip checks: props against walls, as before.

The multiplayer maps hung the game while loading (2026-11-28), so the
sweep covers the 34 campaign maps. They use the same art.

## What retail does

**Spaces are enclosed.**
- The player is almost never in an open field with things dotted about.
  - Streets are corridors: terraced fronts wall to wall (m5l1a, m6l3*).
  - Lanes run between hedgerows 350–450 units tall (m3l2), or between
    garden walls with pillars (m3l3, m4l3).
  - Compounds are yards closed by long sheds and fences (m2l1, m2l3, m1l2b).
- Open ground, where it exists, is bounded on every side by a tree line, a
  wall or a hill. The bounds are what you look at.

**Hills are never straight.**
- The retail edge of the world is one of these:
  - rolling terrain topped by a tree line;
  - big rounded rock masses (`cliffset`, desert/Algiers);
  - forest so dense it closes (m6l1a: nearest tree 75–120 units);
  - fog.
- A crest never runs dead level along a straight line.

**Rhythm.**
- Things repeat at a steady pitch:
  - pillars along garden walls (about every 256);
  - lamps along a street;
  - poles carrying wires;
  - fence posts;
  - rows of lit windows on long sheds (m2l1);
  - banners in pairs flanking doors.
- The eye reads a spacing as designed.

**Walls are layered, bottom to top.** The measured stacks (count of faces, median
height of the lower one):

| upper | on | lower height |
|---|---|---|
| central_europe_winter/trim_set1fltw | stonewall1snow | 136 |
| mohtest/stonewall1grim | stonewall2grim | 160 |
| norway/penwall1 | penwall1a | 128 |
| algiers/afrik_wall1a | afrik_wall1b | 224 |
| norway/resrchnorway_wall1 | resrchnorway_wall1low | 216 |
| tudor / nrmndywlset walls | joist_w (a band between storeys) | 16 |
| joist_w | stonebricks1drk_wntr (the ground storey) | 200 |
| church_exterior_flat | church_exterior_low | 136 |

So a base course is tall: 128–224, a storey's worth, not a 40-unit kerb. A
16-unit joist band marks each floor. Every wall texture set has its own
`…low`, `…b` or `…base` partner for the base.

**Retail paints windows.**
- Buildings that can't be entered carry window textures (`…exwall1win`,
  `jh_concwallwin2b`, `light/jh_window2pj_lit`, lit at night) in even rows.
- Real openings are kept for the buildings you go into.

**Props come in groups** (pairs within 160 units, most common first):
- Every lamp model has a corona beside it: `corona_orange` with
  `lightbulb_caged`, `covered_nowire`, `desertcage`, wall sconces.
- Chairs come with tables, a typewriter, a bucket or a helmet on a
  chair, and stools.
- `mg42_bipod` comes with `mg42ammoboxwbelt`; `30cal_crate` with
  `50cal_crate`; `gasmask` with `bunkershelves`.
- Sandbag links come with their end caps; barbed wire with its tetra posts.
- Trees come with bushes and rocks at their feet: pines with
  `thintrunk` / `thicktrunk`; oak and `commontree` with `bush_regularbush`
  and `bush_full`; palms with squat palms.

**Density** (median distance to the nearest neighbour):

| what | units |
|---|---|
| winter forest trunks and pines | 75–120 |
| bushes | 230–290 |
| oaks, common trees | 330–450 |
| side-mounted lamp posts | ~230 |
| chairs, tables, desks, cabinets | 30–50 (they cluster) |

**Ground:**
- Roads are winding tyre-track decals following the terrain, never ruled
  lines, with verges that fade.
- Snow banks up against walls; grass grows to the foot of walls.
- Craters, rubble mounds and ploughed furrows tell what happened there.

**Storytelling:**
- Shelled houses: a broken wall, the roof rafters bare.
- Rubble spilling into the street, a burnt car, a wagon, hay.
- A landmark visible from far off, such as a church spire or a tower, to
  steer by.

## Where the techdemo fell short (td_v1)

1. Every ground decal (splats, road strips) drew at **full bright**:
   - The decals were `rgbGen vertex`, but MOHlight leaves patch vertex
     colours white.
   - The road strips showed as glowing bands.
   - **Fixed:** the decals take a lightmap (`nextbundle` + `$lightmap`,
     the retail decal pattern), `surfacedensity 32`.
2. Zone ridges were **dead-straight wedges** of one height, rock on the
   slope: tents.
   - **Fixed:** each crest wanders up to 120 off its line and rises and
     falls between 70% and 110%. Both zones a ridge parts read it from the
     same function of the line, so they meet.
   - The caulk vis walls inside follow the lower crest.
   - Each zone's ridges carry a tree line of its own species (`crest` on
     the zone).
3. Bocage hedgerows were low stone banks with small bushes.
   - **Fixed:** retail hedge curtains (`wilderness/m3l3hedgerowsml`
     patches) are about 380 tall on the bank, two a little apart, bowed and
     wandering, with oaks along them.
4. The town's houses stood **apart in a field**.
   - **Fixed:** they are terraced wall to wall, 2 and 3 storeys, different
     storey heights and roof pitches. The street is now a corridor.
5. Still to do, in order of what the shots show:
   - long sheds and compound fences in Norway, to make yards;
   - painted lit windows on the buildings you can't enter;
   - tall base courses (128–224) and joist bands;
   - lamp poles at a steady pitch along roads;
   - a landmark per zone (the bocage chapel could get a spire);
   - rubble and craters;
   - snow drifts against walls;
   - one sky per zone (not possible in one map; sky and sun are global).

## Rules for the generator

- Enclose every space the player stands in. Put the bounds first (fronts,
  hedges, walls, tree lines), then what is inside.
- Give every long edge two frequencies of change: a slow wander and a quicker
  wobble. Nothing natural runs dead level or dead straight.
- Place props as groups from the pair table, not one by one. Lamps always
  come with their light and corona.
- Repeat structural details at a fixed pitch: posts, pillars, lamps,
  windows.
- Use each texture set's own base and band partners at the measured heights.
- Check before compiling:
  - `survey/cracks.py`;
  - clip checks;
  - the brush-side, lightmap and shader-name limits in README.md.
- After compiling, sweep the result (`views_techdemo.py` + `sweep.py`) and
  look at it beside the retail sheets of the same setting.
