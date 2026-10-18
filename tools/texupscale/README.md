# texupscale

Upscales the game's textures 4x with [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) and
packs them as mipmapped DDS files into one add-on pk3. The game's files are never modified.

```sh
python3 -m venv --system-site-packages build/texupscale-venv   # torch with CUDA from the system
build/texupscale-venv/bin/pip install etcpak
py=build/texupscale-venv/bin/python
$py tools/texupscale/texupscale.py --dry-run          # what would be upscaled, from which pk3
$py tools/texupscale/texupscale.py --preview 10       # before/after sheet of 10 random textures
$py tools/texupscale/texupscale.py --install          # build, then copy the pk3s into the game dir
```

Needs Python 3 with torch (CUDA strongly preferred), Pillow, numpy, scipy (for matgen, whose pak
and shader code this uses) and etcpak. The RealESRGAN_x4plus weights (BSD-3) are downloaded to
`~/.cache/openmohaa-texupscale` on first use and checked against their sha256. `--model` takes
another 4x RRDBNet checkpoint. A full build is about 1,800 textures and two hours on a GTX 1080.

- **In game.** GL2 only: `R_LoadImage` tries `<name>.dds` before the `.jpg` or `.tga` a shader
  asks for, from any pk3, so pak order doesn't matter. The GL1 renderer ignores the pk3.
  `r_ext_compressed_textures` only sets how the other textures are compressed on upload: 0 (the
  sharpest) leaves them uncompressed, which on an m3l2 save at 1080p took the GPU memory in
  use from 2.7 GB to 3.6 GB.
- **Which textures.** Everything under `textures/`, `env/` and `models/`, resolved the way the
  engine resolves it: pk3s sorted by name, later ones win. So installed texture packs (AA HD,
  HRRTM) are what gets upscaled. When a stock `.dds` would hide a bigger `.jpg`/`.tga` from a
  texture pack (the engine prefers the DDS), the bigger one is used, and a DDS of it is written
  even if it isn't upscaled, so the pack's texture shows at last.
- **Skipped.** UI, tool, effect, sprite and weather directories, `_n`/`_nh`/`_s` material maps,
  textures under 64 pixels (`--min-size`), and textures already 1024 or more (`--threshold`).
- **Sizes.** Output sides are powers of two, 4x the source, at most 2048 (`--max-size`).
- **Edges.** Tiling textures are padded by wrapping before upscaling, so their seams stay
  seamless. Model skins, sky boxes and `clampmap` textures are padded by mirroring instead.
- **Alpha.** The alpha channel goes through the network too. Opaque textures become DXT1,
  textures with alpha DXT5 (`--format bc7` uses BC7 for both: better colour, and twice the
  memory of DXT1). For textures a shader alpha tests (foliage, fences), each mip level's alpha
  is scaled so as many texels pass the test as at full size; otherwise they thin out and
  vanish with distance.
- **Mipmaps.** Colour is averaged in linear light. The chain goes down to a single 4x4 block,
  which is what the renderer needs to keep a DDS's mipmaps.
- **Split pk3s.** Parts stay under 2 GB, as the engine reads pk3s without ZIP64
  (`zzzzzzzzz-opm-upscale.pk3`, `...-2.pk3`). `--install` removes stale parts.
- **Rebuilds are incremental.** `build/texupscale/manifest.json` records each output's source
  and settings; only changed textures are redone, and an interrupted run picks up where it
  stopped. `--limit N` stops after N.
- **Check.** `--selftest` round-trips each format through Pillow's DDS reader and checks the
  alpha coverage.

matgen resolves textures the same way, so once this pk3 is installed, rebuild the material
maps (`tools/matgen`) to make them from the upscaled textures.
