# Simulated explosions (tools/pyro)

An explosion is simulated in Blender (Mantaflow fire and smoke, run headless from WSL on
the Windows Blender) and played in the game as a 3D volume, drawn by the GL2 renderer
(`code/renderergl2/tr_pyro.c`, `glsl/pyro_fp.glsl`).

    tools/pyro/pyro.py sim truck_hit --preview --set modes=[]   # quick 48^3 bake, no renders
    tools/pyro/gridsheet.py WORK/truck_hit_preview out.png      # side view of the grids over time
    tools/pyro/pyro.py sim truck_hit --set modes=[]             # the real bake (15-20 min)
    tools/pyro/pyro.py pack truck_hit --pk3 zzzzzzzzz-opm-pyro.pk3

The work folder is `$PYRO_WORK`, by default `opm-pyro` in the Windows temp folder (the bake
cache is large). Nothing it makes is committed: the pak is built from the bake.

## A preset (`presets/NAME.json`)

- The domain (metres; the ground is its floor), the grid (`res`), frames at `fps`.
- Fuel `blobs` that burst (`burst` outward, `lift` up) for `emit_frames`, lumpy (`lumps`);
  Mantaflow's fire settings (`fuel`, `burning_rate`, `flame_smoke`, `beta` heat lift,
  `alpha` density lift, `vorticity`).
- `smoke_source`: smoke only, from the base, for its `frames`: what keeps pouring off it.
  Give it upward `lift` and heat, not an outward `burst` (that blows up a balloon).
- `density_scale`, `vol_density`, `vol_fire`: how thick and bright it is in the game.
- `replaces`: the game effect model it takes the place of (its server side is kept: an
  Animate whose `start` animation the map plays), its `scale`, the flash, and `debris`
  groups of metal pieces thrown out (up to six models a group, picked at random).

Traps learned:
- `alpha` (density lift) above 0 or `vorticity` near 1 or more made the smoke smear out
  into a dome at this resolution. `alpha 0`, `vorticity` 0.3-0.5 keep its shape.
- `models/fx/bp_debris1-3.tik` crash the game when thrown as temp models: not used.
- Judge a sim from `gridsheet.py`, not the preview in the game (48^3 smears it).

## The game's side

`pack` writes `models/fx/opm_pyro_NAME.vol`: each frame's density (square root) and flame,
the frames side by side in one RG8 3D texture at load. cgame spawns it as a temp model
(`originspawn ( model models/fx/opm_pyro_NAME.vol ... )`), and the renderer marches it after
the opaque world, at half size (`r_pyroHalf`), stopping at the depth of what is behind:
smoke shadowed toward the map's light (light grid), lit by the fire in it, the flame
glowing; it thins away over its last quarter. Cvars `r_pyro*`. GL1 does not draw it.

The flipbook path (`pack --flipbook`, renders of `pos`/`neg`/`fire`) is the earlier sprite
version, kept for reference.
