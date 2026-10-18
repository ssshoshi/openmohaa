# matgen

Generates normal (`_n`) and specular (`_s`) maps for the OpenGL 2 renderer from the game's own
textures, and packs them into one add-on pk3. The game's files are never modified.

```sh
py=build/deepbump/venv/bin/python                   # see below for setting this up
$py tools/matgen/matgen.py --dry-run                # what would be generated, and from which pk3
$py tools/matgen/matgen.py --only 'textures/normandy/*' --preview 12
$py tools/matgen/matgen.py --parallax --install     # build, then copy the pk3s into the game dir
```

Needs Python 3 with Pillow, numpy and scipy, plus [DeepBump](https://github.com/HugoTini/DeepBump)
and onnxruntime for the default relief estimation. DeepBump is GPL-3 and is only run as a build
tool, so it lives outside the tree:

```sh
git clone --depth 1 https://github.com/HugoTini/DeepBump.git build/deepbump/src
python3 -m venv --system-site-packages build/deepbump/venv
build/deepbump/venv/bin/pip install onnxruntime
build/deepbump/venv/bin/python tools/matgen/matgen.py --parallax --install
```

`--deepbump` (or `MATGEN_DEEPBUMP`) points at the checkout. `--normals luma` needs neither.
`--game` (or `MATGEN_GAME`) points at the directory holding the pk3s; it defaults to
`/mnt/d/Medal of Honor/main`.

- **Which textures.** World textures and model skins (`textures/`, `models/`), resolved the
  way the GL2 renderer resolves them: pk3s sorted by name, later ones win, a `.dds` of the name
  first, and a `.tga` request is served by a `.jpg` of the same name before the `.tga`. So
  installed texture packs (such as AA HD or HRRTM) and the upscale pk3 (`tools/texupscale`)
  are what the maps are made from; rebuild after installing or rebuilding either. Model skins
  don't tile, so their relief is found without wrapping around the edges.
- **Skipped.** UI, sky, sprite, effect and tool directories, textures only used by additive,
  filter, environment-mapped, sky or sprite stages, and textures that already ship their own
  `_n`, `_nh` or `_s` map.
- **Normal and height.** By default DeepBump's network estimates the normals from the texture
  (it tells stains from relief far better than brightness does), and the height is those
  normals integrated, band-passed to features up to 1/64 of the texture (mortar joints, plank
  edges, formwork lines) and softened, since texel-scale grain turns into spikes under
  parallax. `--normals luma` takes the relief from brightness instead: fast, but dark stains
  become dents. Either way, the slopes are scaled by the preset's bump factor and `--strength`.
  DeepBump takes a few seconds per texture on one core; a full build takes about an hour
  on 8.
- **Specular.** Reflectance and gloss come from a material preset (`PRESETS`), chosen from
  the shader's `surfaceparm` or, failing that, from keywords in the path. Crevices are made
  rougher and less reflective.
- **Output formats.** Normal maps are 4:4:4 JPEGs by default. `--parallax` writes `_nh` PNGs
  instead, with the height in alpha, but only for opaque textures with a preset that has a
  `depth` (stone, ground, wood, metal, snow, default). Everything else keeps a JPEG `_n`:
  skeletal models are never parallax mapped, and alpha-tested edges would swim. The height is
  anchored so the high points sit on the polygon and only the crevices recede. Specular maps
  are half-size grey+alpha PNGs. `--max-size` (1024 by default) caps the resolution.
- **Split pk3s.** The engine reads pk3s with 32-bit offsets and without ZIP64, so the output is
  split into parts under 2 GB (`zzzzzzzzzz-opm-materials.pk3`, `...-2.pk3`, ...). `--install`
  removes stale parts from the game directory.
- **Rebuilds are incremental.** `build/matgen/manifest.json` records each output's source
  and settings, and only changed textures are regenerated.
- **Axis check.** `--selftest` checks the normal map axes against the engine's own
  `RGBAtoNormal` (`code/renderergl2/tr_image.c`).

In game the maps need `r_normalMapping 1` and `r_specularMapping 1` (the defaults).
`r_parallaxMapping 1` (or `2` for relief mapping) needs a pk3 built with `--parallax`; it is
latched, so it takes a `vid_restart`. `r_baseParallax` (0.05) sets the overall depth. Use a
separate `--out` for the parallax build to keep both builds around.
