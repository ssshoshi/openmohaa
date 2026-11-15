"""The flora's textures, from photographs: sprays of leaves and bark (tools/flora/flora.py).

The leaves are scans of real ones, from ambientCG's leaf sets, and the bark is
Poly Haven's photographed bark. Both are CC0 (public domain). They are
downloaded once into ~/.cache/openmohaa-content/flora.

A leaf texture is a square of four cells (2x2), each one spray: a twig growing
up from the middle of the cell's bottom edge, with the species' scanned leaves
set along it and its side shoots, the ones underneath in shade. The model's
leaf cards each show one cell. Beside the colour goes a normal map (`_n`): the
scan's own, each leaf's turned with it and tilted its own way, so a card is
not lit as one flat sheet.

Colours are albedo: the renderer lights them. Pixels the alpha test drops take
the leaves' colour, so filtering draws no dark rim. Seeded: the same files
every run.
"""

import io
import json
import math
import os
import urllib.request
import zipfile

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

CACHE = os.path.join(os.path.expanduser("~"), ".cache", "openmohaa-content", "flora")
AGENT = "content-build"


def _get(url, name):
    path = os.path.join(CACHE, name)
    if not os.path.exists(path):
        os.makedirs(CACHE, exist_ok=True)
        req = urllib.request.Request(url, headers={"User-Agent": AGENT})
        with urllib.request.urlopen(req, timeout=120) as r:
            data = r.read()
        with open(path + ".part", "wb") as f:
            f.write(data)
        os.replace(path + ".part", path)
    return path


def ambientcg(asset, res="2K"):
    """An ambientCG atlas: {map name: image} (Color, Opacity, NormalGL)."""
    z = zipfile.ZipFile(_get("https://ambientcg.com/get?file=%s_%s-JPG.zip" % (asset, res), "%s_%s.zip" % (asset, res)))
    out = {}
    for n in z.namelist():
        for key in ("Color", "Opacity", "NormalGL"):
            if n.endswith("_%s.jpg" % key):
                out[key] = Image.open(io.BytesIO(z.read(n)))
                out[key].load()
    return out


def polyhaven(asset, res="1k"):
    """A Poly Haven texture: (colour image, normal image)."""
    with open(_get("https://api.polyhaven.com/files/" + asset, asset + ".json")) as f:
        files = json.load(f)
    out = []
    for key in ("Diffuse", "nor_gl"):
        url = files[key][res]["jpg"]["url"]
        out.append(Image.open(_get(url, os.path.basename(url))).convert("RGB"))
    return out


# --------------------------------------------------------------------------- leaves

def cut_leaves(asset, upright=True):
    """The atlas's leaves, each cut out: (rgb, alpha, normal) float arrays, stalk at the bottom."""
    maps = ambientcg(asset)
    rgb = np.asarray(maps["Color"].convert("RGB"), np.float32)
    alpha = np.asarray(maps["Opacity"].convert("L"), np.float32)
    nrm = np.asarray(maps["NormalGL"].convert("RGB"), np.float32)
    labels, count = ndimage.label(alpha > 128)
    leaves = []
    for sl in ndimage.find_objects(labels):
        h, w = sl[0].stop - sl[0].start, sl[1].stop - sl[1].start
        if h * w < alpha.size / 400:  # a speck
            continue
        pad = (slice(max(0, sl[0].start - 2), sl[0].stop + 2), slice(max(0, sl[1].start - 2), sl[1].stop + 2))
        leaves.append((rgb[pad], alpha[pad], nrm[pad]))
    if not leaves:
        raise SystemExit("%s: no leaves found in its opacity map" % asset)
    return leaves


def _turned(leaf, length, angle, squash, tint, tilt):
    """A leaf scaled to length, narrowed by squash (seen edge on), turned by angle (0 = up, clockwise)."""
    rgb, alpha, nrm = leaf
    h, w = alpha.shape
    k = length / h
    size = (max(2, int(w * k * squash)), max(2, int(h * k)))
    deg = -math.degrees(angle)

    def tf(arr, mode, fill):
        im = Image.fromarray(arr.clip(0, 255).astype(np.uint8), mode).resize(size, Image.LANCZOS)
        return np.asarray(im.rotate(deg, Image.BICUBIC, expand=True, fillcolor=fill), np.float32)

    a = tf(alpha, "L", 0)
    c = tf(rgb, "RGB", (0, 0, 0)) * tint
    n = tf(nrm, "RGB", (128, 128, 255)) / 127.5 - 1.0
    # the normals turn with the picture (y is up in the map, down in the image), then tilt
    ca, sa = math.cos(angle), math.sin(angle)
    nx = n[..., 0] * ca + n[..., 1] * sa + tilt[0]
    ny = -n[..., 0] * sa + n[..., 1] * ca + tilt[1]
    nz = np.maximum(n[..., 2], 0.2)
    ln = np.sqrt(nx * nx + ny * ny + nz * nz)
    n = np.dstack([nx / ln, ny / ln, nz / ln]) * 127.5 + 127.5
    return c, a, n, size


def draw_spray(leaves, size, rng, tint, twig_rgb, leaf_len, density):
    """One cell: (rgb, alpha, normal) arrays, size x size."""
    S = size
    rgb = np.zeros((S, S, 3), np.float32)
    alpha = np.zeros((S, S), np.float32)
    nrm = np.zeros((S, S, 3), np.float32)
    nrm[...] = (127.5, 127.5, 255.0)

    # the twigs: a main stem and side shoots, as polylines of (x, y, heading)
    stems = []

    def stem(x, y, heading, length, bend, steps):
        pts = [(x, y, heading)]
        for _ in range(steps):
            heading += rng.normal(0, 0.10) + bend
            x += math.sin(heading) * length / steps
            y -= math.cos(heading) * length / steps
            pts.append((x, y, heading))
        stems.append(pts)
        return pts

    main = stem(S * 0.5, S * 0.99, rng.normal(0, 0.06), S * 0.72, rng.normal(0, 0.02), 12)
    for i in range(2, len(main) - 1, 2):
        x, y, h = main[i]
        side = 1 if (i // 2) % 2 else -1
        stem(x, y, h + side * rng.uniform(0.6, 1.0), S * rng.uniform(0.24, 0.40) * (1.0 - 0.35 * i / len(main)),
             -side * 0.04, 6)

    twig = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(twig)
    for k, pts in enumerate(stems):
        d.line([(p[0], p[1]) for p in pts], fill=255, width=max(2, int(S * (0.012 if k == 0 else 0.007))))
    t = np.asarray(twig, np.float32)
    rgb[t > 0] = twig_rgb
    alpha = np.maximum(alpha, t)

    spots = []
    for pts in stems:
        for i in range(1, len(pts)):
            for _ in range(density):
                x, y, h = pts[i]
                if i == len(pts) - 1:
                    ang = h + rng.normal(0, 0.5)  # a cluster at the shoot's tip
                else:
                    ang = h + rng.choice([-1, 1]) * rng.uniform(0.35, 1.25)
                spots.append((x, y, ang))
    rng.shuffle(spots)
    n = len(spots)
    for k, (x, y, ang) in enumerate(spots):
        depth = 0.55 + 0.45 * k / max(1, n - 1)  # pasted later = on top = in the light
        vary = np.array(tint, np.float32) * depth * rng.uniform(0.88, 1.12, 3) * rng.uniform(0.9, 1.1)
        c, a, nm, _ = _turned(leaves[rng.integers(len(leaves))], S * leaf_len * rng.uniform(0.75, 1.15), ang,
                              rng.uniform(0.6, 1.0), vary, (rng.normal(0, 0.25), rng.normal(0, 0.25)))
        h, w = a.shape
        # its stalk (the bottom middle of the upright leaf) goes on the twig
        half = S * leaf_len * 0.5
        cx, cy = x + math.sin(ang) * half, y - math.cos(ang) * half
        x0, y0 = int(cx - w / 2), int(cy - h / 2)
        if x0 < 2 or y0 < 2 or x0 + w > S - 2 or y0 + h > S - 2:
            continue  # it would be cut by the cell's edge
        sx0, sy0, sx1, sy1 = 0, 0, w, h
        dst = (slice(y0 + sy0, y0 + sy1), slice(x0 + sx0, x0 + sx1))
        src = (slice(sy0, sy1), slice(sx0, sx1))
        m = (a[src] / 255.0)[..., None]
        rgb[dst] = rgb[dst] * (1 - m) + c[src] * m
        nrm[dst] = nrm[dst] * (1 - m) + nm[src] * m
        alpha[dst] = np.maximum(alpha[dst], a[src])
    return rgb, alpha, nrm


def bleed(rgb, alpha, rounds=24):
    """Colour spread out into the pixels the alpha test drops."""
    rgb = rgb.copy()
    have = alpha > 96
    for _ in range(rounds):
        acc = np.zeros_like(rgb)
        cnt = np.zeros(alpha.shape, np.float32)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            sh = np.roll(have, (dy, dx), (0, 1))
            acc += np.roll(rgb, (dy, dx), (0, 1)) * sh[..., None]
            cnt += sh
        grow = (~have) & (cnt > 0)
        rgb[grow] = acc[grow] / cnt[grow][..., None]
        have = have | grow
    rgb[~have] = rgb[have].mean(axis=0) if have.any() else 0
    return rgb


def leaf_atlas(asset, tint, twig_rgb, seed, cell=512, leaf_len=0.2, density=2):
    """(RGBA image, normal image): four sprays, 2x2, of the leaf set's leaves."""
    rng = np.random.default_rng(seed)
    leaves = cut_leaves(asset)
    W = cell * 2
    rgb = np.zeros((W, W, 3), np.float32)
    alpha = np.zeros((W, W), np.float32)
    nrm = np.zeros((W, W, 3), np.float32)
    for i in range(4):
        r, a, n = draw_spray(leaves, cell, rng, tint, twig_rgb, leaf_len, density)
        y0, x0 = (i // 2) * cell, (i % 2) * cell
        rgb[y0:y0 + cell, x0:x0 + cell] = r
        alpha[y0:y0 + cell, x0:x0 + cell] = a
        nrm[y0:y0 + cell, x0:x0 + cell] = n
    rgb = bleed(rgb, alpha)
    out = np.dstack([rgb, alpha]).clip(0, 255).astype(np.uint8)
    return Image.fromarray(out, "RGBA"), Image.fromarray(nrm.clip(0, 255).astype(np.uint8), "RGB")


# --------------------------------------------------------------------------- bark

def bark(asset, tint=(1.0, 1.0, 1.0), size=(512, 1024)):
    """(RGB image, normal image), tiling: a Poly Haven bark, its colours times tint."""
    col, nrm = polyhaven(asset)
    col = np.asarray(col.resize(size, Image.LANCZOS), np.float32) * np.array(tint, np.float32)
    return Image.fromarray(col.clip(0, 255).astype(np.uint8), "RGB"), nrm.resize(size, Image.LANCZOS)
