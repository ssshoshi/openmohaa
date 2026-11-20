#!/usr/bin/env python3
"""A bullet's hole in a pane of glass: the decal fgame g_glass.cpp puts where
each round goes through a truck's window.

    tools/vehicles/glass_crack.py   ->  data/opm-lamps/textures/opm/vehicles/glasscrack{0,1,2}.tga
                                        and glassshard.tga (a fallen pane's shards)

Drawn, not photographed: a small clean hole (dark, see-through at its very
middle), a crushed ring round it, cracks running out from it in a star, each
kinking a little as it goes and thinning out, and arcs of a spiderweb joining
some of them, broken off here and there. White where the cracks catch the
light, the alpha their strength; the rest clear. Three, seeded, so the hits on
one pane differ.
"""

import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "data", "opm-lamps", "textures", "opm", "vehicles")
SIZE = 256
SS = 2  # drawn this much bigger, then made smaller: smooth lines


def crack(seed):
    rng = np.random.default_rng(seed)
    n = SIZE * SS
    c = n / 2
    img = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(img)

    # the star: cracks out from the hole, kinked, thinning
    rays = []
    count = rng.integers(9, 14)
    for k in range(count):
        ang = 2 * math.pi * (k + rng.uniform(-0.3, 0.3)) / count
        length = n * rng.uniform(0.28, 0.48)
        pts = [(c + math.cos(ang) * n * 0.03, c + math.sin(ang) * n * 0.03)]
        r = n * 0.03
        a = ang
        while r < length:
            step = n * rng.uniform(0.03, 0.07)
            a += rng.normal(0, 0.045)
            r += step
            pts.append((c + math.cos(a) * r, c + math.sin(a) * r))
        rays.append(pts)
        for i in range(len(pts) - 1):
            t = i / max(1, len(pts) - 1)
            width = max(1, int(round((3.0 - 2.4 * t) * SS)))
            d.line([pts[i], pts[i + 1]], fill=int(255 * (1.0 - 0.55 * t)), width=width)
        # a branch or two off it
        for _ in range(rng.integers(0, 3)):
            i = rng.integers(1, max(2, len(pts)))
            bx, by = pts[min(i, len(pts) - 1)]
            ba = a + rng.choice([-1, 1]) * rng.uniform(0.4, 0.9)
            bl = n * rng.uniform(0.04, 0.1)
            d.line([(bx, by), (bx + math.cos(ba) * bl, by + math.sin(ba) * bl)], fill=150, width=max(1, SS))

    # the web: arcs between neighbouring rays at a few radii, some missing
    for ring in (0.09, 0.16, 0.25):
        for k in range(len(rays)):
            if rng.random() < 0.35:
                continue
            p, q = rays[k], rays[(k + 1) % len(rays)]
            i = min(len(p) - 1, int(ring / 0.06))
            j = min(len(q) - 1, int(ring / 0.06))
            mid = ((p[i][0] + q[j][0]) / 2, (p[i][1] + q[j][1]) / 2)
            # bowed out a little, as a web's threads are
            mx, my = mid[0] - c, mid[1] - c
            ml = math.hypot(mx, my) or 1
            bow = n * 0.012
            mid = (mid[0] + mx / ml * bow, mid[1] + my / ml * bow)
            d.line([p[i], mid, q[j]], fill=int(255 * (0.75 - ring)), width=max(1, int(2.2 * SS * (1.2 - ring * 2))))

    # the crushed ring round the hole: a pale frosted disc, broken up
    frost = Image.new("L", (n, n), 0)
    fd = ImageDraw.Draw(frost)
    rr = n * 0.07
    fd.ellipse([c - rr, c - rr, c + rr, c + rr], fill=140)
    noise = (rng.random((n, n)) * 255).astype(np.uint8)
    frost = Image.fromarray(np.minimum(np.asarray(frost), noise)).filter(ImageFilter.GaussianBlur(SS * 1.2))
    alpha = np.maximum(np.asarray(img, np.float32), np.asarray(frost, np.float32))

    # the hole itself: dark, and clear in its very middle
    yy, xx = np.mgrid[0:n, 0:n]
    dist = np.hypot(xx - c, yy - c) / n
    hole = dist < 0.028
    rim = (dist >= 0.028) & (dist < 0.04)
    rgb = np.full((n, n), 235, np.float32)
    rgb[rim] = 40
    rgb[hole] = 15
    alpha[rim] = 235
    alpha[hole] = np.clip((dist[hole] / 0.028) * 255, 0, 255) * 0.9

    out = np.stack([rgb, rgb, rgb, alpha], axis=2).clip(0, 255).astype(np.uint8)
    return Image.fromarray(out, "RGBA").resize((SIZE, SIZE), Image.LANCZOS)


def shard(seed=7):
    """A shard of a fallen pane, for opm_glassshard (the falling shards'
    sprite): a sliver of clear glass, faintly green-blue, darker across most
    of it and catching a reflection along one side, its broken edges thin and
    a little greener, as glass is seen edge on."""
    rng = np.random.default_rng(seed)
    n = 64 * SS
    pts = []
    corners = rng.integers(4, 6)
    for k in range(corners):
        a = 2 * math.pi * k / corners + rng.uniform(-0.3, 0.3)
        r = n * (0.45 if k % 2 == 0 else rng.uniform(0.12, 0.25))
        pts.append((n / 2 + math.cos(a) * r, n / 2 + math.sin(a) * r * 0.6))
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).polygon(pts, fill=255)
    m = np.asarray(mask, np.float32) / 255
    # the reflection: a soft band across the sliver, from one side
    yy, xx = np.mgrid[0:n, 0:n] / n
    band = np.clip(1 - abs((xx * 0.8 + yy * 0.6) - 0.62) / 0.16, 0, 1) ** 2
    rgb = np.zeros((n, n, 3), np.float32)
    rgb[...] = (150, 175, 170)
    rgb += band[..., None] * np.array((90, 75, 70), np.float32)
    alpha = m * (28 + 110 * band)
    edges = Image.new("L", (n, n), 0)
    ImageDraw.Draw(edges).line(pts + [pts[0]], fill=255, width=SS + 1)
    e = (np.asarray(edges, np.float32) / 255) * m
    rgb = rgb * (1 - e[..., None]) + e[..., None] * np.array((120, 165, 150), np.float32)
    alpha = np.maximum(alpha, e * 150)
    out = np.dstack([rgb, alpha]).clip(0, 255).astype(np.uint8)
    return Image.fromarray(out, "RGBA").resize((64, 64), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    for k in range(3):
        path = os.path.join(OUT, "glasscrack%d.tga" % k)
        crack(100 + k).save(path)
        print(os.path.relpath(path))
    path = os.path.join(OUT, "glassshard.tga")
    shard().save(path)
    print(os.path.relpath(path))


if __name__ == "__main__":
    main()

