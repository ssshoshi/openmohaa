"""The flora's textures, drawn: sprays of leaves and bark (tools/flora/flora.py).

A leaf texture is a square of four cells (2x2), each one spray: a twig growing
up from the middle of the cell's bottom edge with the species' leaves on it.
The model's leaf cards each show one cell. Beside the colour goes a normal map
(`_n`): every leaf tilted its own way and creased along its midrib, so a card
is not lit as one flat sheet.

Bark tiles both ways: noise made in the frequency domain, which wraps by
itself. Its height gives the normal map.

Colours are albedo: the renderer lights them. Pixels the alpha test drops take
the leaves' colour, so filtering draws no dark rim. Seeded: the same files
every run.
"""

import math

import numpy as np
from PIL import Image, ImageDraw

SS = 2  # supersampling


# --------------------------------------------------------------------------- leaves

def leaf_outline(kind, n=64):
    """A leaf's outline, base at (0, 0), tip at (0, 1): points down one side and back up the other."""

    def half(t):
        if kind == "oak":  # obovate, four or five round lobes a side, widest past the middle
            env = 0.34 * (t ** 0.75) * ((1.0 - t) ** 0.4) / 0.55
            return env * (0.70 + 0.30 * math.cos(2 * math.pi * 4.5 * t + 0.6)) + 0.012
        if kind == "beech":  # ovate, wavy edge
            env = 0.30 * math.sin(math.pi * t ** 0.85) ** 0.8
            return env * (1.0 + 0.04 * math.cos(2 * math.pi * 9 * t)) + 0.01
        if kind == "birch":  # triangular, toothed, pointed
            env = 0.36 * (t ** 0.45) * ((1.0 - t) ** 0.95) / 0.5
            return env * (1.0 + 0.07 * math.cos(2 * math.pi * 11 * t)) + 0.008
        if kind == "hawthorn":  # deeply cut, three to five lobes
            env = 0.42 * (t ** 0.6) * ((1.0 - t) ** 0.5) / 0.55
            return env * (0.62 + 0.38 * math.cos(2 * math.pi * 2.5 * t + 1.0)) + 0.012
        # plain oval
        return 0.26 * math.sin(math.pi * t ** 0.9) ** 0.85 + 0.01

    ts = [i / (n - 1) for i in range(n)]
    right = [(half(t), t) for t in ts]
    left = [(-half(t), t) for t in reversed(ts)]
    return right, left


def _xf(points, origin, angle, length, squash):
    """Leaf-space points onto the canvas: turned by angle (0 = up), squash narrows it (a leaf seen edge on)."""
    c, s = math.cos(angle), math.sin(angle)
    out = []
    for x, y in points:
        x *= length * squash
        y *= length
        out.append((origin[0] + x * c + y * s, origin[1] + x * s - y * c))
    return out


def draw_spray(kind, size, rng, palette, twig_rgb, leaf_len, density):
    """One cell: (rgb, alpha, normal) arrays, size x size."""
    S = size * SS
    rgb = Image.new("RGB", (S, S), palette[0])
    alpha = Image.new("L", (S, S), 0)
    nrm = Image.new("RGB", (S, S), (128, 128, 255))
    d_rgb, d_a, d_n = ImageDraw.Draw(rgb), ImageDraw.Draw(alpha), ImageDraw.Draw(nrm)
    right, left = leaf_outline(kind)

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

    main = stem(S * 0.5, S * 0.99, rng.normal(0, 0.06), S * 0.74, rng.normal(0, 0.02), 14)
    for i in range(3, len(main) - 2, 2):
        x, y, h = main[i]
        side = 1 if (i // 2) % 2 else -1
        stem(x, y, h + side * rng.uniform(0.6, 1.0), S * rng.uniform(0.22, 0.40) * (1.0 - 0.4 * i / len(main)),
             -side * 0.04, 7)

    for pts in stems:
        w = max(1, int(S * 0.006))
        d_rgb.line([(p[0], p[1]) for p in pts], fill=twig_rgb, width=w + 1)
        d_a.line([(p[0], p[1]) for p in pts], fill=255, width=w + 1)

    # the leaves, the ones underneath first and darker
    leaves = []
    for pts in stems:
        for i in range(1, len(pts)):
            for _ in range(density):
                x, y, h = pts[i]
                side = rng.choice([-1, 1])
                ang = h + side * rng.uniform(0.35, 1.25)
                if i == len(pts) - 1:
                    ang = h + rng.normal(0, 0.5)  # a cluster at the shoot's tip
                leaves.append((x, y, ang))
    rng.shuffle(leaves)
    n = len(leaves)
    for k, (x, y, ang) in enumerate(leaves):
        depth = 0.62 + 0.38 * k / max(1, n - 1)  # painted later = on top = lighter
        length = S * leaf_len * rng.uniform(0.75, 1.15)
        squash = rng.uniform(0.55, 1.0)
        base = palette[rng.integers(len(palette))]
        vary = rng.uniform(0.85, 1.15) * depth
        tilt = (rng.normal(0, 0.28), rng.normal(0, 0.28))
        c, s = math.cos(ang), math.sin(ang)
        for half, shade, crease in ((right, 1.06, 0.30), (left, 0.90, -0.30)):
            poly = _xf(half + [(0.0, 1.0), (0.0, 0.0)], (x, y), ang, length, squash)
            col = tuple(int(min(255, ch * vary * shade)) for ch in base)
            d_rgb.polygon(poly, fill=col)
            d_a.polygon(poly, fill=255)
            # normal: the leaf's own tilt, and each half turned away from the midrib
            nx = tilt[0] + crease * c
            ny = tilt[1] + crease * s
            ln = math.sqrt(nx * nx + ny * ny + 1.0)
            d_n.polygon(poly, fill=(int(127.5 + 127.5 * nx / ln), int(127.5 - 127.5 * ny / ln),
                                    int(127.5 + 127.5 / ln)))
        rib = tuple(int(min(255, ch * vary * 1.35 + 12)) for ch in base)
        mid = _xf([(0.0, -0.08), (0.0, 0.97)], (x, y), ang, length, squash)
        d_rgb.line(mid, fill=rib, width=max(1, SS))
        d_a.line(mid[:1] + _xf([(0.0, 0.1)], (x, y), ang, length, squash), fill=255, width=max(1, SS))
        for j in range(1, 6):  # side veins
            t = j / 6.5
            for sgn in (-1, 1):
                v = _xf([(0.0, t), (sgn * 0.16, t + 0.13)], (x, y), ang, length, squash)
                d_rgb.line(v, fill=tuple(int(min(255, ch * vary * 1.15 + 5)) for ch in base), width=1)

    small = lambda im: im.resize((size, size), Image.LANCZOS)  # noqa: E731
    return np.asarray(small(rgb), np.float32), np.asarray(small(alpha), np.float32), np.asarray(small(nrm), np.float32)


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


def leaf_atlas(kind, palette, twig_rgb, seed, cell=512, leaf_len=0.17, density=2):
    """(RGBA image, normal image): four sprays, 2x2."""
    rng = np.random.default_rng(seed)
    W = cell * 2
    rgb = np.zeros((W, W, 3), np.float32)
    alpha = np.zeros((W, W), np.float32)
    nrm = np.zeros((W, W, 3), np.float32)
    for i in range(4):
        r, a, n = draw_spray(kind, cell, rng, palette, twig_rgb, leaf_len, density)
        y0, x0 = (i // 2) * cell, (i % 2) * cell
        rgb[y0:y0 + cell, x0:x0 + cell] = r
        alpha[y0:y0 + cell, x0:x0 + cell] = a
        nrm[y0:y0 + cell, x0:x0 + cell] = n
    rgb = bleed(rgb, alpha)
    out = np.dstack([rgb, alpha]).clip(0, 255).astype(np.uint8)
    return Image.fromarray(out, "RGBA"), Image.fromarray(nrm.clip(0, 255).astype(np.uint8), "RGB")


# --------------------------------------------------------------------------- bark

def _noise(rng, h, w, fx, fy, falloff=1.6):
    """Wrapping noise, 0 mean, unit deviation: about fx waves across and fy up."""
    spec = np.fft.fft2(rng.normal(size=(h, w)))
    ky = np.fft.fftfreq(h)[:, None] * h / max(fy, 1e-3)
    kx = np.fft.fftfreq(w)[None, :] * w / max(fx, 1e-3)
    k = np.sqrt(kx * kx + ky * ky)
    filt = np.exp(-((k - 1.0) ** 2) * falloff) + 0.35 * np.exp(-((k - 2.2) ** 2) * falloff)
    out = np.real(np.fft.ifft2(spec * filt))
    return (out - out.mean()) / (out.std() + 1e-9)


def bark(kind, plate_rgb, fissure_rgb, moss_rgb, seed, w=512, h=1024):
    """(RGB image, normal image), tiling: plates of bark with fissures between them."""
    rng = np.random.default_rng(seed)
    if kind == "furrowed":  # oak, old pine: deep fissures running up, ridges that fork and join
        a = _noise(rng, h, w, 9, 2.2) + 0.45 * _noise(rng, h, w, 22, 6)
        height = np.clip(np.abs(a) / 0.55, 0, 1) ** 0.7
        height = height * (0.82 + 0.18 * np.clip(_noise(rng, h, w, 40, 30) * 0.5 + 0.5, 0, 1))
    elif kind == "smooth":  # beech, young trees: smooth grey, faint rings and scars
        height = 0.75 + 0.10 * _noise(rng, h, w, 3, 14) + 0.05 * _noise(rng, h, w, 30, 40)
        height = np.clip(height, 0, 1)
    elif kind == "birch":  # white, with dark horizontal lenticels and black scars
        height = np.clip(0.8 - 0.5 * np.clip(_noise(rng, h, w, 4, 40) - 1.1, 0, 1)
                         - 0.9 * np.clip(_noise(rng, h, w, 2.5, 5) - 1.5, 0, 1), 0, 1)
    else:  # scaly: spruce, palm
        a = _noise(rng, h, w, 7, 9) + 0.5 * _noise(rng, h, w, 16, 20)
        height = np.clip(np.abs(a) / 0.5, 0, 1) ** 0.6
    fine = np.clip(_noise(rng, h, w, 60, 25) * 0.5 + 0.5, 0, 1)
    moss = np.clip(_noise(rng, h, w, 2, 1.5) - 0.7, 0, 1)[..., None] * 0.55
    plate = np.array(plate_rgb, np.float32)
    fiss = np.array(fissure_rgb, np.float32)
    col = fiss + (plate - fiss) * height[..., None]
    col = col * (0.82 + 0.36 * fine[..., None])
    col = col * (1 - moss) + np.array(moss_rgb, np.float32) * moss
    # the normal from the height, wrapping
    k = 3.0
    dx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * k
    dy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * k
    ln = np.sqrt(dx * dx + dy * dy + 1.0)
    nrm = np.dstack([127.5 - 127.5 * dx / ln, 127.5 + 127.5 * dy / ln, 127.5 + 127.5 / ln])
    return (Image.fromarray(col.clip(0, 255).astype(np.uint8), "RGB"),
            Image.fromarray(nrm.clip(0, 255).astype(np.uint8), "RGB"))
