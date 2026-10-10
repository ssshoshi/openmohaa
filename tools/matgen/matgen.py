#!/usr/bin/env python3
"""Generate GL2 normal/height (_nh) and specular (_s) maps from MOH:AA textures.

The OpenGL 2 renderer picks these up by name: for textures/foo/bar.tga it looks
for textures/foo/bar_nh (tangent space normal in RGB, height in alpha) and
textures/foo/bar_s (specular reflectance in RGB, gloss in alpha). See
R_CreateShader's material lookup in code/renderergl2/tr_shader.c.

The textures are read from the game's own pk3s, resolved in the same order the
engine uses (pk3 names sorted case-insensitively, later ones win), so the maps
match whatever texture pack is actually installed. Nothing in the game's files
is modified; the result is a single add-on pk3.
"""

import argparse
import fnmatch
import hashlib
import io
import json
import multiprocessing
import os
import re
import sys
import zipfile

import numpy as np
from PIL import Image
from scipy import ndimage

VERSION = 3

IMAGE_EXTS = ("jpg", "tga", "png", "dds")

# Average normal tilt, in degrees, that a texture's relief is scaled towards
# (before the preset's bump factor and --strength).
TARGET_TILT = 12.0

# Directories whose textures are never drawn by the lit lightall path (UI,
# sky, sprites, effects, tool textures), so maps for them would only take
# up space.
SKIP_DIRS = (
    "textures/common/", "textures/effects/", "textures/gametext/",
    "textures/hud/", "textures/light/", "textures/lights/",
    "textures/mohmenu/", "textures/objectives/", "textures/sky/",
    "textures/special/", "textures/sprites/", "textures/test/",
    "textures/mohtest/", "textures/weather/", "textures/tempsign/",
    "models/fx/",
)

# Where the images with maps are: world textures, and model skins (most of
# which are under textures/models/).
ROOTS = ("textures/", "models/")

# Model skins are cut into islands and don't tile, so their relief is
# integrated without wrapping, like a clampmap's.
NO_WRAP_DIRS = ("textures/models/", "models/")

# Blend modes that add to or filter what is already drawn: such a stage is
# never the lit diffuse stage.
UNLIT_BLENDS = {
    "add", "filter",
    "gl_one gl_one", "gl_dst_color gl_zero", "gl_zero gl_src_color",
    "gl_dst_color gl_one", "gl_one gl_one_minus_src_color",
}

# Material presets for the non-PBR specular map.
#   f0:     specular reflectance at normal incidence (also darkens the diffuse
#           by 1 - f0 in lightall, so keep dielectrics near 0.04)
#   gloss:  gloss range, crevices at the low end and exposed surfaces at the
#           high end (r_glossType 0: roughness = exp2(-3 * gloss))
#   bump:   normal strength multiplier
#   depth:  parallax depth, as a fraction of r_baseParallax; presets without
#           one get a plain normal map even with --parallax (see PARALLAX)
PRESETS = {
    "metal":   dict(f0=0.12, gloss=(0.35, 0.65), bump=0.8, depth=0.5),
    "painted": dict(f0=0.05, gloss=(0.30, 0.55), bump=0.7),
    # MOH:AA's "glass" textures are mostly whole windows, frames included
    "glass":   dict(f0=0.06, gloss=(0.45, 0.75), bump=0.5),
    "water":   dict(f0=0.06, gloss=(0.80, 0.95), bump=0.4),
    "wood":    dict(f0=0.04, gloss=(0.20, 0.40), bump=1.0, depth=0.7),
    "stone":   dict(f0=0.035, gloss=(0.10, 0.30), bump=1.3, depth=1.0),
    "ground":  dict(f0=0.03, gloss=(0.05, 0.20), bump=1.2, depth=0.9),
    "foliage": dict(f0=0.04, gloss=(0.20, 0.40), bump=0.8),
    "snow":    dict(f0=0.04, gloss=(0.30, 0.50), bump=0.8, depth=0.6),
    "fabric":  dict(f0=0.03, gloss=(0.05, 0.20), bump=0.8),
    "skin":    dict(f0=0.035, gloss=(0.30, 0.50), bump=0.4),
    "paper":   dict(f0=0.03, gloss=(0.10, 0.25), bump=0.4),
    "default": dict(f0=0.04, gloss=(0.15, 0.35), bump=1.0, depth=0.7),
}

# Parallax only pays on opaque world surfaces with real relief. Fabric and
# skin are mostly on skeletal models, which lightall never parallax maps;
# glass, water, paper and paint have no depth to show; and alpha-tested
# textures (foliage, fences) would have their cut-out edges swim.
PARALLAX = {name for name, p in PRESETS.items() if "depth" in p}

# The engine reads pk3s through 32-bit offsets (unzip.c's uLong, which is 32
# bits on Windows) and without ZIP64, so each pk3 has to stay under 2 GB.
PK3_MAX_BYTES = 1900 * 1024 * 1024

# MOH:AA surfaceparm material names -> preset.
SURFACEPARM_PRESETS = {
    "metal": "metal", "grill": "metal",
    "wood": "wood",
    "stone": "stone", "gravel": "ground",
    "dirt": "ground", "mud": "ground", "sand": "ground",
    "grass": "foliage", "foliage": "foliage",
    "snow": "snow",
    "glass": "glass",
    "puddle": "water", "water": "water",
    "carpet": "fabric", "paper": "paper",
}

# Path keywords -> preset, tried in order when no shader names a material.
KEYWORD_PRESETS = (
    (("faces/", "face", "skin", "hand"), "skin"),
    (("models/human/", "uniform", "tunic", "cloth", "fabric", "flag",
      "carpet", "rug", "curtain", "sandbag", "tarp", "canvas"), "fabric"),
    (("glass", "window"), "glass"),
    (("water", "ocean", "sea", "puddle"), "water"),
    (("models/vehicles/", "models/statweapons/"), "painted"),
    (("metal", "steel", "iron", "rust", "pipe", "grate", "grill", "tin",
      "models/weapons/", "copper", "brass"), "metal"),
    (("wood", "plank", "board", "crate", "barrel", "beam", "door",
      "furniture", "log"), "wood"),
    (("stone", "brick", "conc", "rock", "plaster", "cobble", "wall",
      "tile", "adobe", "bunker", "marble", "cement", "roof"), "stone"),
    (("dirt", "mud", "sand", "gravel", "ground", "road", "path", "earth"),
     "ground"),
    (("grass", "leaf", "leaves", "bush", "hedge", "tree", "foliage",
      "ivy", "vine", "natural/"), "foliage"),
    (("snow", "ice", "winter"), "snow"),
    (("paper", "poster", "sign", "map", "book"), "paper"),
)


def path_key(name):
    """Sort key matching the engine's FS_PathCmp."""
    return name.lower().replace("\\", "/").replace(":", "/")


class Vfs:
    """The game's pk3 contents, resolved the way the engine resolves them."""

    def __init__(self, game_dir, exclude):
        self.game_dir = game_dir
        paks = [p for p in os.listdir(game_dir) if p.lower().endswith(".pk3")]
        paks = [p for p in paks if not exclude(p)]
        self.paks = sorted(paks, key=path_key)
        # lowercase path -> (pak, member, size, crc); later paks win
        self.files = {}
        for pak in self.paks:
            with zipfile.ZipFile(os.path.join(game_dir, pak)) as z:
                for info in z.infolist():
                    if info.is_dir():
                        continue
                    self.files[info.filename.lower()] = (
                        pak, info.filename, info.file_size, info.CRC)

    def read(self, entry):
        pak, member = entry[0], entry[1]
        with zipfile.ZipFile(os.path.join(self.game_dir, pak)) as z:
            return z.read(member)


def tokenize(text):
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    for line in text.splitlines():
        for tok in re.findall(r'"[^"]*"|\{|\}|[^\s{}]+', line):
            yield tok.strip('"')
        yield "\n"


def parse_shaders(text):
    """Yield (name, shader_words, stages) for every shader in a script.

    shader_words holds the lowercase shader-level lines, and each stage is a
    list of lowercase lines (each a list of tokens) with image names kept in
    their original case.
    """
    toks = list(tokenize(text))
    i = 0
    n = len(toks)

    def read_line(i):
        line = []
        while i < n and toks[i] not in ("\n", "{", "}"):
            line.append(toks[i])
            i += 1
        return line, i

    while i < n:
        if toks[i] in ("\n", "}"):
            i += 1
            continue
        if toks[i] == "{":
            # stray block: skip it
            depth = 0
            while i < n:
                if toks[i] == "{":
                    depth += 1
                elif toks[i] == "}":
                    depth -= 1
                    if depth == 0:
                        i += 1
                        break
                i += 1
            continue
        name = toks[i]
        i += 1
        while i < n and toks[i] == "\n":
            i += 1
        if i >= n or toks[i] != "{":
            continue
        i += 1
        shader_lines, stages = [], []
        while i < n and toks[i] != "}":
            if toks[i] == "\n":
                i += 1
            elif toks[i] == "{":
                i += 1
                stage = []
                while i < n and toks[i] != "}":
                    if toks[i] in ("\n", "{"):
                        i += 1
                        continue
                    line, i = read_line(i)
                    if line:
                        stage.append(line)
                i += 1
                stages.append(stage)
            else:
                line, i = read_line(i)
                if line:
                    shader_lines.append(line)
        i += 1
        yield name, shader_lines, stages


def image_base(name):
    name = name.replace("\\", "/").lower()
    root, ext = os.path.splitext(name)
    if ext[1:] in IMAGE_EXTS:
        return root, ext[1:]
    return name, ""


class ImageUse:
    def __init__(self):
        self.lit = False
        self.unlit = False
        self.clamp = False
        self.materials = set()
        self.request_ext = ""


def collect_uses(vfs):
    """Map image base name -> ImageUse, from every shader script."""
    uses = {}
    scripts = sorted(p for p in vfs.files
                     if p.startswith("scripts/") and p.endswith(".shader"))
    for path in scripts:
        text = vfs.read(vfs.files[path]).decode("latin-1")
        for name, shader_lines, stages in parse_shaders(text):
            low = [[t.lower() for t in l] for l in shader_lines]
            materials = {l[1] for l in low
                         if len(l) > 1 and l[0] == "surfaceparm"}
            sprite = any(l[0] == "spritegen" or
                         (l[0] == "deformvertexes" and len(l) > 1 and
                          l[1].startswith("autosprite"))
                         for l in low)
            sky = any(l[0] == "skyparms" for l in low) or "sky" in materials
            for st in stages:
                stl = [[t.lower() for t in l] for l in st]
                images = []
                clamp = False
                for l, raw in zip(stl, st):
                    if l[0] in ("map", "clampmap", "clampmapx", "clampmapy") \
                            and len(l) > 1:
                        images.append(raw[1])
                        clamp |= l[0] != "map"
                    elif l[0] == "animmap" and len(l) > 2:
                        images.extend(raw[2:])
                blend = next((" ".join(l[1:]) for l in stl
                              if l[0] == "blendfunc"), "")
                env = any(l[0] == "tcgen" and len(l) > 1 and
                          l[1] == "environment" for l in stl)
                unlit = sprite or sky or env or blend in UNLIT_BLENDS
                for img in images:
                    if img.startswith("$") or img.startswith("*"):
                        continue
                    base, ext = image_base(img)
                    use = uses.setdefault(base, ImageUse())
                    if unlit:
                        use.unlit = True
                    else:
                        use.lit = True
                        use.materials |= materials
                    use.clamp |= clamp
                    if ext and not use.request_ext:
                        use.request_ext = ext
    return uses


def resolve_image(vfs, base, request_ext):
    """The file R_LoadImage would load for base.

    GL2 tries a .dds first, from any pk3, so an installed upscale pk3
    (tools/texupscale) is what the maps are made from.
    """
    ext = request_ext or "tga"
    order = ["dds"]
    if ext == "tga":
        order.append("jpg")     # MOH:AA tries the JPEG first for a .tga
    order.append(ext)
    order += [e for e in ("png", "tga", "jpg") if e not in order]
    for e in order:
        entry = vfs.files.get(base + "." + e)
        if entry:
            return entry
    return None


def pick_preset(base, use):
    mats = [SURFACEPARM_PRESETS[m] for m in sorted(use.materials)
            if m in SURFACEPARM_PRESETS] if use else []
    if mats:
        preset = mats[0]
        # painted metal on vehicles and props reads better than bare metal
        if preset == "metal" and any(k in base for k in
                                     ("models/vehicles/", "statweapons/")):
            preset = "painted"
        return preset
    for keys, preset in KEYWORD_PRESETS:
        if any(k in base for k in keys):
            return preset
    return "default"


# ---------------------------------------------------------------------------
# Map generation


def load_rgba(data):
    img = Image.open(io.BytesIO(data))
    img.load()
    return img.convert("RGBA")


# How relief is found: "deepbump" (a neural net that estimates normals from
# the texture, see load_deepbump) or "luma" (brightness as height).
NORMALS = "deepbump"
DEEPBUMP_DIR = os.environ.get("MATGEN_DEEPBUMP", "build/deepbump/src")

# DeepBump's normals are gentle; this scales their slopes before the preset's
# bump factor and --strength.
DEEPBUMP_GAIN = 1.5
# Raw DeepBump tilt, in degrees, at which a texture gets its preset's full
# parallax depth; flatter textures get proportionally less.
DEEPBUMP_FULL_TILT = 6.0

_deepbump = None


def load_deepbump():
    """DeepBump (github.com/HugoTini/DeepBump, GPL-3) run as a build tool:
    its ONNX model and tiling helpers are loaded from DEEPBUMP_DIR, a
    checkout of that repository, with onnxruntime."""
    global _deepbump
    if _deepbump is None:
        sys.path.insert(0, DEEPBUMP_DIR)
        import onnxruntime as ort
        import utils_inference
        import module_normals_to_height
        ort.disable_telemetry_events()
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = 1    # one worker per core already
        session = ort.InferenceSession(
            os.path.join(DEEPBUMP_DIR, "deepbump256.onnx"), opts,
            providers=["CPUExecutionProvider"])
        _deepbump = (session, utils_inference, module_normals_to_height)
    return _deepbump


def luma_relief(lum, mode, size, p, strength):
    """Brightness as height. Returns (normal, height, parallax height,
    depth factor); heights are in [-0.5, 0.5]."""
    # Remove low-frequency albedo (paint, stains, lighting baked into the
    # texture) so only surface detail becomes relief.
    low = ndimage.gaussian_filter(lum, sigma=max(2.0, size / 24.0), mode=mode)
    detail = lum - low

    spread = np.percentile(np.abs(detail), 98) + 1e-6
    height = np.clip(detail / (2.0 * spread), -0.5, 0.5)

    # Multi-scale gradients: fine grain plus broader shapes. Gradients are
    # per texel, so scale by resolution to keep HD and stock textures alike.
    gx = np.zeros_like(height)
    gy = np.zeros_like(height)
    for sigma, weight in ((0.0, 0.5), (1.5, 0.3), (4.0, 0.2)):
        hs = ndimage.gaussian_filter(height, sigma, mode=mode) if sigma \
            else height
        gx += weight * ndimage.sobel(hs, axis=1, mode=mode) / 8.0
        gy += weight * ndimage.sobel(hs, axis=0, mode=mode) / 8.0
    # Aim every texture at a similar average tilt rather than a fixed scale,
    # which would leave fine-grained textures flat and coarse ones craggy.
    # The cap keeps nearly featureless textures from amplifying noise.
    mean_grad = np.sqrt(gx * gx + gy * gy).mean() + 1e-6
    k_aim = np.tan(np.radians(TARGET_TILT)) / mean_grad
    k = min(k_aim, size / 256.0 * 12.0)
    capped = k / k_aim
    k *= strength * p["bump"]
    # Same axes as RGBAtoNormal in tr_image.c: +x right along s, +y down the
    # image along t, so a raised texel's normal leans away from the slope.
    nx, ny = -gx * k, -gy * k
    nz = np.ones_like(nx)
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    normal = np.stack((nx * inv, ny * inv, nz * inv), axis=-1)

    # Parallax needs broad shapes: texel-scale grain in the height becomes
    # thin pillars that lightall's 16-step search renders as spikes. So the
    # height keeps only bands a few texels wide and up (the grain stays in
    # the normal map), relative to a 1024 texture.
    res = size / 1024.0
    ph = np.zeros_like(height)
    for sigma, weight in ((3.0, 0.4), (6.0, 0.35), (12.0, 0.25)):
        ph += weight * ndimage.gaussian_filter(height, max(1.0, sigma * res),
                                               mode=mode)
    ph = np.clip(ph / (2.0 * (np.percentile(np.abs(ph), 98) + 1e-6)),
                 -0.5, 0.5)
    return normal, height, ph, capped


def deepbump_relief(grey, clamp, mode, size, p, strength):
    """DeepBump's normals, integrated into a height. Same returns as
    luma_relief."""
    session, ui, to_height = load_deepbump()
    img = grey[None].astype(np.float32)
    tiles, paddings = ui.tiles_split(img, (256, 256), (128, 128))
    pred = ui.tiles_infer(tiles, session)
    db = ui.normalize(ui.tiles_merge(pred, (128, 128), (3,) + grey.shape,
                                     paddings))
    raw = db.transpose(1, 2, 0) * 2.0 - 1.0

    # DeepBump's +y is up the image; RGBAtoNormal's is down it.
    k = DEEPBUMP_GAIN * strength * p["bump"]
    nx, ny, nz = raw[..., 0] * k, -raw[..., 1] * k, np.maximum(raw[..., 2],
                                                               1e-3)
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    normal = np.stack((nx * inv, ny * inv, nz * inv), axis=-1)
    tilt = np.degrees(np.arccos(np.clip(raw[..., 2], -1.0, 1.0))).mean()
    capped = min(1.0, tilt / DEEPBUMP_FULL_TILT)

    # Integrating the normals leaves broad swells that are not relief, so
    # keep features up to 1/64 of the texture (mortar joints, formwork
    # lines, plank edges), then soften texel grain, which parallax turns
    # into spikes.
    z = to_height.apply(db, not clamp, None)[0]
    z = z - ndimage.gaussian_filter(z, max(2.0, size / 64.0), mode=mode)
    lo, hi = np.percentile(z, 1), np.percentile(z, 99)
    height = np.clip((z - lo) / (hi - lo + 1e-9), 0.0, 1.0) - 0.5
    ph = ndimage.gaussian_filter(height, max(0.7, 1.5 * size / 1024.0),
                                 mode=mode)
    return normal, height, ph, capped


def gen_maps(img, preset, clamp, strength, max_size, spec_scale):
    """Return (nh RGBA image, s LA image, preview planes)."""
    w, h = img.size
    scale = min(1.0, max_size / max(w, h))
    if scale < 1.0:
        w, h = max(1, round(w * scale)), max(1, round(h * scale))
        img = img.resize((w, h), Image.LANCZOS)

    px = np.asarray(img, dtype=np.float32) / 255.0
    rgb, alpha = px[..., :3], px[..., 3]
    if NORMALS == "deepbump":
        grey = rgb.mean(axis=-1)    # what DeepBump was trained on
    else:
        grey = rgb[..., 0] * 0.25 + rgb[..., 1] * 0.5 + rgb[..., 2] * 0.25
        grey = grey ** 2.2

    # Fill alpha-tested holes with the mean so their edges don't become cliffs.
    opaque = alpha >= 0.5
    if opaque.any() and not opaque.all():
        grey = np.where(opaque, grey, grey[opaque].mean())

    mode = "nearest" if clamp else "wrap"
    size = max(w, h)
    p = PRESETS[preset]
    if NORMALS == "deepbump":
        normal, height, ph, capped = deepbump_relief(grey, clamp, mode, size,
                                                     p, strength)
    else:
        normal, height, ph, capped = luma_relief(grey, mode, size, p,
                                                 strength)

    nh = np.empty((h, w, 4), dtype=np.uint8)
    nh[..., :3] = np.clip((normal * 0.5 + 0.5) * 255.0 + 0.5, 0, 255)
    # lightall reads depth as 1 - alpha, and r_parallaxMapOffset 0 puts
    # alpha 1 at the polygon. So the high points sit on the polygon and only
    # the crevices recede, and a texture with little real relief gets
    # correspondingly shallow.
    depth = p.get("depth", 1.0) * min(1.0, strength) * capped
    nh[..., 3] = np.clip((1.0 - depth * (0.5 - ph)) * 255.0 + 0.5, 0, 255)

    # Specular: reflectance from the preset, occluded in crevices; gloss
    # rougher in crevices and smoother on exposed surfaces.
    cavity = ndimage.gaussian_filter(height, 1.0, mode=mode) + 0.5
    lo, hi = p["gloss"]
    gloss = lo + (hi - lo) * np.clip(cavity, 0.0, 1.0)
    f0 = p["f0"] * (0.6 + 0.4 * np.clip(cavity * 2.0, 0.0, 1.0))
    spec = np.empty((h, w, 2), dtype=np.uint8)
    spec[..., 0] = np.clip(f0 * 255.0 + 0.5, 0, 255)
    spec[..., 1] = np.clip(gloss * 255.0 + 0.5, 0, 255)
    s_img = Image.fromarray(spec, "LA")
    if spec_scale < 1.0:
        sw = max(1, round(w * spec_scale))
        sh = max(1, round(h * spec_scale))
        s_img = s_img.resize((sw, sh), Image.BILINEAR)

    return Image.fromarray(nh, "RGBA"), s_img, (img, nh, spec)


def output_names(base, parallax):
    """A plain normal map is a JPEG, a sixth the size of an RGBA PNG; the
    height channel for parallax needs alpha, so _nh is a PNG."""
    return (base + ("_nh.png" if parallax else "_n.jpg")), base + "_s.png"


def pk3_part_name(pk3, i):
    """The i-th pk3 of a split set: foo.pk3, foo-2.pk3, foo-3.pk3, ..."""
    return pk3 if i == 0 else "%s-%d.pk3" % (pk3[:-4], i + 1)


def is_pk3_part(name, pk3):
    stem = pk3[:-4].lower()
    name = name.lower()
    return name == pk3.lower() or re.fullmatch(
        re.escape(stem) + r"-\d+\.pk3", name) is not None


def job_key(entry, preset, clamp, args):
    h = hashlib.sha1()
    h.update(json.dumps([VERSION, TARGET_TILT, entry[0], entry[1],
                         entry[2], entry[3],
                         preset, clamp, PRESETS[preset], args.strength,
                         args.max_size, args.spec_scale,
                         args.parallax, NORMALS] + (
                             [DEEPBUMP_GAIN, DEEPBUMP_FULL_TILT]
                             if NORMALS == "deepbump" else [])).encode())
    return h.hexdigest()


_worker_game_dir = None
_worker_paks = {}


def _init_worker(game_dir, normals, deepbump_dir):
    global _worker_game_dir, NORMALS, DEEPBUMP_DIR
    _worker_game_dir = game_dir
    NORMALS, DEEPBUMP_DIR = normals, deepbump_dir


def _worker_read(entry):
    # Opening a pk3 reads its whole directory, which is slow over /mnt, so
    # each worker keeps its paks open.
    z = _worker_paks.get(entry[0])
    if z is None:
        z = zipfile.ZipFile(os.path.join(_worker_game_dir, entry[0]))
        _worker_paks[entry[0]] = z
    return z.read(entry[1])


def _run_job(job):
    base, entry, preset, clamp, want_n, want_s, out_dir, strength, \
        max_size, spec_scale, parallax = job
    try:
        img = load_rgba(_worker_read(entry))
        # alpha-tested textures get a plain normal map; see PARALLAX
        if parallax and (preset not in PARALLAX or
                         img.getextrema()[3][0] < 128):
            parallax = False
        nh, s, _ = gen_maps(img, preset, clamp, strength, max_size, spec_scale)
        nh_name, s_name = output_names(base, parallax)
        written = []
        if want_n:
            path = os.path.join(out_dir, nh_name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            if parallax:
                nh.save(path, compress_level=6)
            else:
                # 4:4:4, as chroma subsampling would smear the X and Y axes
                nh.convert("RGB").save(path, quality=95, subsampling=0)
            written.append(nh_name)
        if want_s:
            path = os.path.join(out_dir, s_name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            s.save(path, compress_level=6)
            written.append(s_name)
        return base, written, None
    except Exception as e:  # keep going; report at the end
        return base, [], "%s: %s" % (entry[1], e)


# ---------------------------------------------------------------------------


def plan_jobs(vfs, args):
    uses = collect_uses(vfs)

    bases = {}
    for path in vfs.files:
        if not path.startswith(ROOTS):
            continue
        base, ext = image_base(path)
        if ext:
            bases.setdefault(base, set()).add(ext)

    stats = {"skipped_dir": 0, "skipped_unlit": 0, "authored": 0,
             "companion": 0, "filtered": 0}
    jobs = []
    for base in sorted(bases):
        # our own suffixes, or maps a texture pack already ships
        stem = re.sub(r"_(n|nh|s)$", "", base)
        if stem != base and stem in bases:
            stats["companion"] += 1
            continue
        if base.startswith(SKIP_DIRS):
            stats["skipped_dir"] += 1
            continue
        if args.only and not any(fnmatch.fnmatch(base, g) for g in args.only):
            stats["filtered"] += 1
            continue
        use = uses.get(base)
        if use and use.unlit and not use.lit:
            stats["skipped_unlit"] += 1
            continue
        want_n = base + "_n" not in bases and base + "_nh" not in bases
        want_s = base + "_s" not in bases
        if not (want_n or want_s):
            stats["authored"] += 1
            continue
        entry = resolve_image(vfs, base, use.request_ext if use else "")
        if not entry:
            continue
        preset = pick_preset(base, use)
        clamp = bool(use and use.clamp) or base.startswith(NO_WRAP_DIRS)
        jobs.append((base, entry, preset, clamp, want_n, want_s))
    return jobs, stats


def write_preview(vfs, jobs, args, path):
    import random
    rng = random.Random(1)
    pick = rng.sample(jobs, min(args.preview, len(jobs)))
    cell = 192
    sheet = Image.new("RGB", (cell * 4, cell * len(pick)), (32, 32, 32))
    for row, (base, entry, preset, clamp, _, _) in enumerate(pick):
        img = load_rgba(vfs.read(entry))
        _, _, (diffuse, nh, spec) = gen_maps(
            img, preset, clamp, args.strength, args.max_size, 1.0)
        planes = [
            diffuse.convert("RGB"),
            Image.fromarray(nh[..., :3], "RGB"),
            Image.fromarray(nh[..., 3], "L").convert("RGB"),
            Image.fromarray(spec[..., 1], "L").convert("RGB"),
        ]
        for col, plane in enumerate(planes):
            sheet.paste(plane.resize((cell, cell), Image.BILINEAR),
                        (col * cell, row * cell))
        print("  preview %2d: %s [%s]" % (row, base, preset))
    sheet.save(path)
    print("wrote preview %s (diffuse | normal | height | gloss)" % path)


def selftest():
    """Check our normal axes against the engine's RGBAtoNormal on a bump."""
    # a small raised disc, well inside the high-pass radius
    size = 256
    yy, xx = np.mgrid[0:size, 0:size]
    disc = ((xx - 128) ** 2 + (yy - 128) ** 2 < 6 ** 2).astype(np.float32)
    disc = ndimage.gaussian_filter(disc, 2.0)
    grey = (np.full((size, size), 0.3) + 0.5 * disc) * 255.0
    rgba = np.dstack([grey, grey, grey, np.full_like(grey, 255.0)])
    img = Image.fromarray(rgba.astype(np.uint8), "RGBA")
    nh, _, _ = gen_maps(img, "default", False, 1.0, 1024, 1.0)
    ours = np.asarray(nh, dtype=np.float32)[..., :2] / 127.5 - 1.0

    # tr_image.c RGBAtoNormal: x = left - right, y = row above - row below
    hgt = np.asarray(img, dtype=np.float32)[..., 0]
    left, right = np.roll(hgt, 1, 1), np.roll(hgt, -1, 1)
    up, down = np.roll(hgt, 1, 0), np.roll(hgt, -1, 0)
    engine = np.dstack([left - right, up - down])

    # the slope directions must agree wherever the engine sees a slope
    mask = np.hypot(engine[..., 0], engine[..., 1]) > 4.0
    ratio = ((ours[mask] * engine[mask]).sum(-1) > 0.0).mean()
    print("selftest: %.1f%% of slope texels lean the same way as "
          "RGBAtoNormal's"
          % (ratio * 100.0))
    return 0 if ratio > 0.98 else 1


def main():
    global NORMALS, DEEPBUMP_DIR
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=os.environ.get(
        "MATGEN_GAME", "/mnt/d/Medal of Honor/main"),
        help="game directory holding the pk3s (default: %(default)s)")
    ap.add_argument("--out", default="build/matgen",
                    help="work directory (default: %(default)s)")
    ap.add_argument("--pk3", default="zzzzzzzzzz-opm-materials.pk3",
                    help="output pk3 name (default: %(default)s)")
    ap.add_argument("--only", action="append",
                    help="only texture bases matching this glob, e.g. "
                         "'textures/normandy/*' (repeatable)")
    ap.add_argument("--normals", choices=("deepbump", "luma"),
                    default=NORMALS,
                    help="estimate relief with DeepBump's neural net, or "
                         "from brightness (default: %(default)s)")
    ap.add_argument("--deepbump", default=DEEPBUMP_DIR,
                    help="DeepBump checkout (default: %(default)s)")
    ap.add_argument("--strength", type=float, default=1.0,
                    help="normal map strength (default: %(default)s)")
    ap.add_argument("--max-size", type=int, default=1024,
                    help="largest output dimension (default: %(default)s)")
    ap.add_argument("--spec-scale", type=float, default=0.5,
                    help="specular map size relative to the normal map "
                         "(default: %(default)s)")
    ap.add_argument("--parallax", action="store_true",
                    help="write _nh PNGs with height in alpha for "
                         "r_parallaxMapping, instead of _n JPEGs, for opaque "
                         "stone, ground, wood, metal, snow and default "
                         "textures (those are about 5x larger)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count(),
                    help="worker processes (default: %(default)s)")
    ap.add_argument("--dry-run", action="store_true",
                    help="list what would be generated and stop")
    ap.add_argument("--preview", type=int, default=0, metavar="N",
                    help="write a contact sheet of N random textures and stop")
    ap.add_argument("--preview-out", default="matgen-preview.png",
                    help="contact sheet path (default: %(default)s)")
    ap.add_argument("--install", action="store_true",
                    help="copy the finished pk3 into the game directory")
    ap.add_argument("--selftest", action="store_true",
                    help="check the normal map axes against the engine's")
    args = ap.parse_args()

    NORMALS, DEEPBUMP_DIR = args.normals, args.deepbump
    if NORMALS == "deepbump":
        try:
            load_deepbump()
        except (ImportError, OSError) as e:
            ap.error("DeepBump unavailable (%s); see tools/matgen/README.md, "
                     "or use --normals luma" % e)

    if args.selftest:
        return selftest()

    if not args.pk3.lower().endswith(".pk3"):
        ap.error("--pk3 must end in .pk3")
    vfs = Vfs(args.game, exclude=lambda p: is_pk3_part(p, args.pk3))
    print("%d pk3s, %d files" % (len(vfs.paks), len(vfs.files)))
    jobs, stats = plan_jobs(vfs, args)

    counts = {}
    for base, entry, preset, *_ in jobs:
        counts[preset] = counts.get(preset, 0) + 1
    print("%d textures to process; skipped: %s" % (len(jobs), ", ".join(
        "%s %d" % kv for kv in stats.items() if kv[1])))
    print("presets: " + ", ".join("%s %d" % kv for kv in sorted(
        counts.items(), key=lambda kv: -kv[1])))

    if args.dry_run:
        for base, entry, preset, clamp, want_n, want_s in jobs:
            print("  %-60s %-8s %s%s%s  <- %s" % (
                base, preset, "n" if want_n else "-", "s" if want_s else "-",
                " clamp" if clamp else "", entry[0]))
        return 0

    if args.preview:
        write_preview(vfs, jobs, args, args.preview_out)
        return 0

    out_dir = os.path.join(args.out, "out")
    manifest_path = os.path.join(args.out, "manifest.json")
    try:
        with open(manifest_path) as f:
            manifest = json.load(f)
    except (OSError, ValueError):
        manifest = {}

    todo = []
    new_manifest = {}
    for base, entry, preset, clamp, want_n, want_s in jobs:
        key = job_key(entry, preset, clamp, args)
        prev = manifest.get(base)
        if prev and prev["key"] == key and all(
                os.path.exists(os.path.join(out_dir, n))
                for n in prev["files"]):
            new_manifest[base] = prev
            continue
        # the worker decides between _n and _nh, so the files come later
        new_manifest[base] = {"key": key, "files": []}
        todo.append((base, entry, preset, clamp, want_n, want_s, out_dir,
                     args.strength, args.max_size, args.spec_scale,
                     args.parallax))

    # drop outputs that are no longer wanted or are about to be replaced
    for base, prev in manifest.items():
        if new_manifest.get(base) is not prev:
            for n in prev["files"]:
                try:
                    os.remove(os.path.join(out_dir, n))
                except OSError:
                    pass

    print("%d up to date, %d to generate" % (len(jobs) - len(todo), len(todo)))
    errors = []
    if todo:
        with multiprocessing.Pool(args.jobs, _init_worker,
                                  (args.game, NORMALS, DEEPBUMP_DIR)) as pool:
            for i, (base, written, err) in enumerate(
                    pool.imap_unordered(_run_job, todo, chunksize=4), 1):
                if err:
                    errors.append(err)
                    new_manifest.pop(base, None)
                else:
                    new_manifest[base]["files"] = written
                if i % 100 == 0 or i == len(todo):
                    print("  %d/%d" % (i, len(todo)), flush=True)

    os.makedirs(args.out, exist_ok=True)
    with open(manifest_path, "w") as f:
        json.dump(new_manifest, f, indent=0, sort_keys=True)

    # Split into parts under PK3_MAX_BYTES. A texture's maps go in the same
    # part so a part on its own is never half a material.
    parts = [[]]
    part_bytes = 0
    for base in sorted(new_manifest):
        files = new_manifest[base]["files"]
        size = sum(os.path.getsize(os.path.join(out_dir, n)) for n in files)
        if parts[-1] and part_bytes + size > PK3_MAX_BYTES:
            parts.append([])
            part_bytes = 0
        parts[-1] += files
        part_bytes += size

    pk3_names = [pk3_part_name(args.pk3, i) for i in range(len(parts))]
    for name in os.listdir(args.out):
        if is_pk3_part(name, args.pk3) and name not in pk3_names:
            os.remove(os.path.join(args.out, name))
    for name, files in zip(pk3_names, parts):
        pk3_path = os.path.join(args.out, name)
        tmp = pk3_path + ".tmp"
        total = 0
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_STORED,
                             allowZip64=False) as z:
            for n in files:
                src = os.path.join(out_dir, n)
                z.write(src, n)
                total += os.path.getsize(src)
        os.replace(tmp, pk3_path)
        print("wrote %s: %d files, %.1f MB" % (
            pk3_path, len(files), total / 1048576.0))
    n_nh = sum(f.endswith("_nh.png") for part in parts for f in part)
    print("%d textures with parallax height" % n_nh)

    for e in errors:
        print("error: " + e, file=sys.stderr)

    if args.install:
        import shutil
        for name in os.listdir(args.game):
            if is_pk3_part(name, args.pk3) and name not in pk3_names:
                os.remove(os.path.join(args.game, name))
                print("removed stale %s" % name)
        for name in pk3_names:
            dest = os.path.join(args.game, name)
            shutil.copyfile(os.path.join(args.out, name), dest)
            print("installed %s" % dest)

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
