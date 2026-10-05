# MOHAA models in Blender

- `io_scene_mohaa/`: the Blender add-on (File > Import/Export > MOHAA): `.tik`, `.skd`/`.skb`
  and `.skc`, with the game's textures. Install the folder as an add-on, then set the game
  folders in its preferences.
- `mohaa_blender.py`: the same from the command line (`import`, `export`, `preview`, `info`,
  `extract`).
- `mohaa_remaster.py`: batch remastering, below.

## Remastering models

New geometry for the game's models, on their original skeletons, UVs and textures, so your
upscaled textures and normal maps keep fitting. Where the new geometry fails a check, a
surface is smoothed instead, or left as it was.

    tools/blender/mohaa_remaster.py run 'models/weapons/*.tik' models/human/allied_airborne_soldier.tik \
        --work ~/remaster --pk3 ~/remaster/zzz-remaster.pk3 \
        --trellis-python ~/miniconda3/envs/trellis/bin/python --trellis-home ~/TRELLIS

1. **prepare**: each model is imported with the game's textures (put your texture pk3 in
   `main/` first; keep an earlier `zzz-remaster.pk3` out of it), its arms are lifted away from
   the body, and it is rendered from four sides.
2. **generate**: [TRELLIS](https://github.com/microsoft/TRELLIS) turns the renders into one
   candidate mesh per seed (`--seeds 1,2,3`). Meshes of your own count as candidates too: put
   them in `--detail-dir` under the model's game path (`models/weapons/kar98.obj`, or a folder
   `models/weapons/kar98/` of them), or in `WORK/<model>/candidates/`.
3. **build**: the candidate that fits best is aligned to the model, decimated to a budget
   (`--factor`, 4x the original triangles by default, at most `--max-tris`), cut into the
   original surfaces with their UVs, bone weights and facial morphs, and checked surface by
   surface: its shape against the original (`--fit-tolerance`) and its stretching over a few of
   the model's animations. Surfaces matching `--keep` (e.g. `--keep 'head*'`) stay as they are.
4. **pack**: the changed `.skd` files go into the pk3. The game's `.tik` files and animations
   are not touched.

Review `WORK/<model>/review/before_after.png` (top: original, bottom: remastered) and
`report.json` (what happened to each surface, and why). `WORK/<model>/remastered.blend`
holds both versions for hand fixes. Re-running skips finished steps; `--redo build` (or
`prepare`, `generate`) runs them again. Then test in game before the pk3 goes in your install.

Engine limits kept: 1000 vertices and 2000 triangles per surface (bigger surfaces are split
under the same name, so the `.tik` still shades them, though a script hiding a split surface
by name hides only its first piece), and 32 surfaces per model (each entity's surface flags
are a fixed array of 32), so the budget shrinks until a model fits. New surfaces have no LOD
data. Skinning runs on the CPU, so keep the budget modest for models that appear many times
at once.

### TRELLIS

TRELLIS needs Linux (WSL works) and an NVIDIA GPU with 16 GB or more. Follow its README
(`./setup.sh --new-env --basic --xformers --flash-attn --diffoctreerast --spconv --mipgaussian
--kaolin --nvdiffrast`); the model downloads from Hugging Face on first use. Without
`--trellis-python` (or `$TRELLIS_PYTHON`) the generate step is skipped and only your own meshes,
or smoothing, are used.

### Tests

    python tests/test_remaster.py --work /tmp/remaster-test     # with the bpy module (pip install bpy)
    blender -b --factory-startup --python tests/test_remaster.py -- --work /tmp/remaster-test

It needs no game data and no TRELLIS: it makes a small rigged character, stands in for
TRELLIS with a fused, unrigged blob of it, and checks the build and the fallback.
