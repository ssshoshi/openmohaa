# model-smooth

Smoother MOH:AA skeletal meshes (`.skd`): one level of Loop subdivision, with the skinning exact. After the RTCW
Castle Operations remaster's `tools/model-smooth` (RTCW `.mds`).

Linear blend skinning is linear in each bone's weight and weight × offset, so every vertex is carried as those and
Loop's masks combine them: a new vertex skins, in every pose, to exactly that combination of the skinned vertexes.
Only dropping a new vertex's weakest weights (`--min-weight`, past the most the surface had) changes anything.

- Welded where position, weights and normal agree (UV seams are not edges); an edge whose normals differ past
  `--crease` degrees, or the end of a surface, keeps its line (boundary rules).
- Pieces apart from the rest under `--decal-tris` triangles are cut, not smoothed, so they stay on what they sit on.
- Texture coordinates are cut, not smoothed. Morph targets (faces) are carried like positions.
- Only vertexes whose strongest bone matches `--bones` are smoothed (default: shoulders, arms, hands, neck, head, so
  helmets too); the rest keep their place and edges touching them are only cut in half, and a surface with nothing to
  smooth is left alone. `--rule REGEX=BONES` picks other bones for the models it matches: the run below smooths only
  what the head carries in gear and equipment (every helmet and cap, whatever its file is called), all of the
  coxswain's life vest and the fly-off helmets, and bodies by the default. From the techdemo test: the torso (spine: belts and button lines went wavy), the clavicles
  (front of the collar), hips, legs and feet (boots looked worse) stay as they were, and equipment other than helmets
  and caps is not run at all.
- LOD stays: original vertexes keep their order and collapse data; new ones go last and collapse to an end of their
  edge at the lowest threshold, so distant models drop them first.
- A surface past 4095 vertexes or 8192 triangles once subdivided is left as it is (the engine's per-surface limits,
  `TIKI_MAX_VERTEXES` / `TIKI_MAX_TRIANGLES`, raised for this from 1000 / 2000, and `SHADER_MAX_VERTEXES` from 2048
  to 4096 so such a surface still fits one batch: Manon's head with her hair is 4368 triangles).

```bash
python3 -I tools/model-smooth/model_smooth.py --roundtrip '.*'                 # all 815 SKDs must be byte identical
python3 -I tools/model-smooth/model_smooth.py --list 'human/german'            # surfaces and their sizes
python3 -I tools/model-smooth/model_smooth.py --smooth '^models/(human|gear|equipment)/' \
    --rule 'lifevest=.' --rule 'flyoff=.' --rule '^models/(gear|equipment)/=head' --pk3 zzzzzzzzz-opm-smooth.pk3
```

Reads the game's paks through `tools/paks.py` (`MOHAA_GAME`). The pk3 it writes is made from the game's models:
never commit or share it (the `.gitignore` here keeps it out). Test it in an orch install's `home/main`, not the
shared `main`.
