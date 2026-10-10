# MOHAA models in Blender

- `io_scene_mohaa/`: the Blender add-on (File > Import/Export > MOHAA): `.tik`, `.skd`/`.skb`
  and `.skc`, with the game's textures. Install the folder as an add-on, then set the game
  folders in its preferences.
- `mohaa_blender.py`: the same from the command line (`import`, `export`, `preview`, `info`,
  `extract`).
- `mohaa_remaster.py`: batch remastering, below.

## Remastering models

Two modes, both keeping the models' original skeletons, UVs and textures, so your upscaled
textures and normal maps keep fitting.

### Props: subdivide (the default)

    tools/blender/mohaa_remaster.py run 'models/static/*.tik' 'models/furniture/*.tik' \
        --work ~/remaster --pk3 ~/remaster/zzz-remaster.pk3

Props have the right shapes, only coarse ones: an eight-sided stove, a boxy chair. Each
surface is rounded by subdivision (Catmull-Clark, `--levels 1` by default, each level about
four times the triangles):

- the surface is welded back into one mesh first (the game format splits vertices at every
  UV seam and normal break, which would leave every edge an open border);
- edges stay creased where the faces meet at more than 60 degrees, where the original's
  normals break, and along long faces (a straight-sided cylinder would bulge into a barrel
  otherwise: these act as a modeller's holding edges). Shading follows the original: an edge
  is shaded sharp where its normals broke, or where a thin panel folds back on itself;
- vertices move only along the surface normal (from a linear subdivision to the rounded one),
  so the texture stays where it was instead of sliding with them.

A surface that strays from the original by more than `--fit-tolerance`, stretches in
animation, has facial morphs, or would put the model over the engine's limits stays as it
was. Steps: **prepare** (import, renders), **build**, **pack**, as below; nothing is
generated.

### Characters and weapons: TRELLIS (`--mode trellis`)

New geometry, where the shape itself should gain detail. Where it fails a check a surface
is smoothed as above instead, or left as it was.

    tools/blender/mohaa_remaster.py run 'models/weapons/*.tik' models/human/allied_airborne_soldier.tik \
        --mode trellis --work ~/remaster --pk3 ~/remaster/zzz-remaster.pk3 \
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

TRELLIS rebuilds a model from pictures of it, so it reproduces a coarse model's flat facets
rather than rounding them, and its 64-cube grid loses thin parts: it does not suit props.

### Both

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

TRELLIS needs Linux (WSL works) and an NVIDIA GPU. Follow its README
(`./setup.sh --new-env --basic --xformers --spconv --kaolin`; nvdiffrast, flash-attention and
the Gaussian-splat extras aren't used); the model downloads from Hugging Face on first use.
Without `--trellis-python` (or `$TRELLIS_PYTHON`) the generate step is skipped and only your
own meshes, or smoothing, are used.

TRELLIS is written for Ampere-class GPUs with 16 GB. On older ones (anything before the RTX
30 series) `remaster/trellis_compat.py` steps in: xformers instead of flash-attention, fp32
instead of half precision, one model on the GPU at a time, and its sparse convolutions in
plain PyTorch where spconv has no kernels for the card. On a GTX 1080 (8 GB) a candidate
takes about 1.5 minutes plus 40 s to load the models, using up to 6 GB of GPU memory and
about 5 GB of system memory; it stops by itself when system memory runs low (WSL's default
limit is half the PC's memory, set in `%UserProfile%\.wslconfig`).

### Tests

    python tests/test_remaster.py --work /tmp/remaster-test     # with the bpy module (pip install bpy)
    blender -b --factory-startup --python tests/test_remaster.py -- --work /tmp/remaster-test

It needs no game data and no TRELLIS: it makes a small rigged character, stands in for
TRELLIS with a fused, unrigged blob of it, and checks the build and the fallback.
