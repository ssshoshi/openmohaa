#!/usr/bin/env python3
"""Draw the ground cover's grass texture (data/opm-groundcover).

    tools/groundcover/grass.py              # writes the texture into data/
    tools/groundcover/grass.py --preview    # and a preview on grey beside it

Two clumps of grass blades side by side, 512x256 RGBA: each half is one
square tuft quad (code/renderergl2/tr_groundcover.c). Tapered, curved blades
from a common base, darker at the root. The colour is a mid olive and the
renderer multiplies it by the ground's light, so it is the grass's albedo,
not how bright it looks. Pixels the alpha test drops take the blades' colour,
so filtering does not draw a dark rim. Seeded: the same file every run.
"""

import argparse
import math
import os
import random

import numpy as np
from PIL import Image

W, H = 512, 256
SS = 3  # supersampling
OUT = os.path.join(os.path.dirname(__file__), "..", "..", "data", "opm-groundcover",
                   "textures", "opm", "groundcover_grass.tga")

PALETTE = [  # blade colours, albedo
    (92, 112, 52), (104, 122, 58), (82, 104, 46), (118, 128, 64),
    (98, 116, 50), (128, 126, 70), (140, 132, 78),  # the last two dry
]


def blade(rgb, alpha, x0, base_w, height, bend, color, rng):
    """One blade from (x0, bottom) up, bent sideways by bend at the tip."""
    h = rgb.shape[0]
    steps = int(height * 1.5) + 2
    for i in range(steps):
        t = i / (steps - 1)
        y = h - 1 - t * height
        x = x0 + bend * t * t
        half = base_w * 0.5 * (1.0 - t) ** 0.8 + 0.6 * SS
        shade = 0.55 + 0.45 * t ** 0.7  # darker at the root
        c = np.array(color) * shade * rng.uniform(0.95, 1.05)
        xa, xb = int(math.floor(x - half)), int(math.ceil(x + half))
        yi = int(y)
        if yi < 0:
            break
        for xi in range(max(xa, 0), min(xb + 1, rgb.shape[1])):
            # a little lighter down the middle
            mid = 1.0 - abs(xi - x) / max(half, 1e-3)
            rgb[yi, xi] = c * (0.94 + 0.08 * max(mid, 0))
            alpha[yi, xi] = 1.0


def clump(seed):
    rng = random.Random(seed)
    w, h = (W // 2) * SS, H * SS
    rgb = np.zeros((h, w, 3), np.float32)
    alpha = np.zeros((h, w), np.float32)
    n = 80
    blades = []
    for _ in range(n):
        # the base spread over the middle, the tall ones near the centre
        x0 = w * 0.5 + rng.gauss(0, w * 0.16)
        x0 = min(max(x0, w * 0.08), w * 0.92)
        centre = 1.0 - abs(x0 - w * 0.5) / (w * 0.5)
        height = h * rng.uniform(0.45, 0.97) * (0.65 + 0.35 * centre)
        lean = (x0 - w * 0.5) / (w * 0.5)
        bend = (lean * rng.uniform(0.3, 0.9) + rng.uniform(-0.3, 0.3)) * height * 0.45
        # the tip stays on the quad
        bend = min(max(bend, w * 0.04 - x0), w * 0.96 - x0)
        base_w = rng.uniform(2.5, 5.5) * SS
        blades.append((height, x0, base_w, bend, rng.choice(PALETTE)))
    # the short ones in front
    for height, x0, base_w, bend, color in sorted(blades, key=lambda b: -b[0]):
        blade(rgb, alpha, x0, base_w, height, bend, color, rng)
    return rgb, alpha


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--preview", action="store_true")
    args = ap.parse_args()

    halves = [clump(1), clump(2)]
    rgb = np.concatenate([h[0] for h in halves], axis=1)
    alpha = np.concatenate([h[1] for h in halves], axis=1)

    # down from the supersampled size
    img_rgb = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).resize((W, H), Image.LANCZOS)
    img_a = Image.fromarray((alpha * 255).astype(np.uint8)).resize((W, H), Image.LANCZOS)
    # premultiplied colour, then divided back: the edges keep the blade colour
    prem = np.asarray(Image.fromarray(np.clip(rgb * alpha[..., None], 0, 255).astype(np.uint8))
                      .resize((W, H), Image.LANCZOS), np.float32)
    a = np.asarray(img_a, np.float32) / 255.0
    colour = prem / np.maximum(a[..., None], 1e-3)
    mean = (prem.reshape(-1, 3).sum(0) / max(a.sum(), 1)).astype(np.float32)
    colour = np.where(a[..., None] > 0.02, colour, mean)
    out = np.dstack([np.clip(colour, 0, 255), a * 255]).astype(np.uint8)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    im = Image.fromarray(out, "RGBA")
    im.save(OUT)
    print(os.path.normpath(OUT))

    if args.preview:
        bg = Image.new("RGBA", (W, H), (128, 128, 128, 255))
        bg.alpha_composite(im)
        p = os.path.splitext(OUT)[0] + "_preview.png"
        p = os.path.join(os.environ.get("TMPDIR", "/tmp"), os.path.basename(p))
        bg.save(p)
        print(p)


if __name__ == "__main__":
    main()
