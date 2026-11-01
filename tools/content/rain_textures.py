#!/usr/bin/env python3
"""Draws the rain runoff textures (content/opm-rain/textures/opm_rain/*.tga).

curtain.tga: the veil of water that leaves a roof's edge all along it, many
short streaks falling and the drops they break into (tiling both ways).
stream.tga, below:

Water pouring off an eave (tr_water.c, R_RainRunoff): a few thin threads of
it side by side, each wandering a little, thinning and breaking into drops
here and there, on a clear ground. White, with how much water there is in the
alpha: the renderer tints and fades it with the vertex colour. It tiles
top to bottom, as the shader scrolls it down.

Made here rather than painted, so the fork carries nothing it cannot publish.

    tools/content/rain_textures.py          # rewrite it
    tools/content/rain_textures.py --seed 7 # another draw of the same
"""

import argparse
import os

import numpy as np
from PIL import Image

OUT = os.path.join(os.path.dirname(__file__), "..", "..", "content", "opm-rain", "textures", "opm_rain")

W, H = 64, 512


def periodic_noise(rng, n, cells):
    """1D value noise over n samples that wraps at n, in [0, 1]."""
    knots = rng.random(cells)
    x = np.arange(n) * cells / n
    i = np.floor(x).astype(int)
    f = x - i
    f = f * f * (3 - 2 * f)
    return knots[i % cells] * (1 - f) + knots[(i + 1) % cells] * f


def stream(rng):
    alpha = np.zeros((H, W))
    xs = np.arange(W)[None, :]
    ys = np.arange(H)

    threads = 4
    for t in range(threads):
        centre = (t + 0.5) * W / threads + (rng.random() - 0.5) * W / threads * 0.5
        wander = (periodic_noise(rng, H, 6) - 0.5) * 6.0 + (periodic_noise(rng, H, 23) - 0.5) * 1.5
        width = 1.0 + 1.8 * periodic_noise(rng, H, 9)
        # where it runs whole and where it breaks into drops
        body = np.clip((periodic_noise(rng, H, 7) - 0.25) * 2.5, 0.0, 1.0)
        x = centre + wander
        d = np.abs(xs - x[:, None]) / width[:, None]
        core = np.clip(1.0 - d, 0.0, 1.0) ** 1.5
        alpha = np.maximum(alpha, core * (0.35 + 0.65 * body[:, None]))

        # drops, stretched by their fall, where it breaks up
        for _ in range(26):
            y0 = rng.integers(0, H)
            if body[y0] > 0.7 and rng.random() < 0.7:
                continue
            length = rng.integers(6, 22)
            r = 1.0 + 1.4 * rng.random()
            dy = (ys - y0 + H // 2) % H - H // 2  # wraps
            along = np.clip(1.0 - np.abs(dy + length / 2) / (length / 2 + 1), 0.0, 1.0)
            dx = np.abs(xs - x[y0] - (rng.random() - 0.5) * 3) / r
            drop = np.clip(1.0 - dx, 0.0, 1.0) * along[:, None]
            alpha = np.maximum(alpha, drop * (0.8 + 0.2 * rng.random()))

    # a fine mist about the threads
    mist = periodic_noise(rng, H, 40)[:, None] * 0.0
    alpha = np.clip(alpha + mist, 0.0, 1.0)

    # brighter down the middle of each thread, as light runs through water
    rgb = np.empty((H, W, 3))
    rgb[...] = (200 + 55 * alpha)[..., None]
    img = np.dstack([rgb, alpha * 255]).astype(np.uint8)
    return Image.fromarray(img, "RGBA")


def curtain(rng):
    """A veil of water off a roof edge: many short streaks of it falling, and
    the drops it breaks into, densely, tiling both ways (W2 x H2)."""
    w2, h2 = 256, 512
    alpha = np.zeros((h2, w2))
    xs = np.arange(w2)
    ys = np.arange(h2)

    def stamp(x0, y0, length, width, strength):
        # a streak from (x0, y0) down, wrapping at the edges
        dx = (xs - x0 + w2 / 2) % w2 - w2 / 2
        dy = (ys - y0 + h2 / 2) % h2 - h2 / 2
        across = np.clip(1.0 - np.abs(dx) / width, 0.0, 1.0)
        along = np.clip(1.0 - np.abs(dy - length / 2) / (length / 2 + 0.5), 0.0, 1.0) ** 0.6
        np.maximum(alpha, np.outer(along, across) * strength, out=alpha)

    # streaks: long and faint, short and bright
    for _ in range(520):
        stamp(rng.random() * w2, rng.random() * h2, 8 + rng.random() * 70, 0.6 + rng.random() * 1.1,
              0.25 + 0.6 * rng.random())
    # drops, a little drawn out by their fall
    for _ in range(900):
        stamp(rng.random() * w2, rng.random() * h2, 2 + rng.random() * 6, 0.7 + rng.random() * 0.9,
              0.5 + 0.5 * rng.random())

    rgb = np.empty((h2, w2, 3))
    rgb[...] = (205 + 50 * alpha)[..., None]
    img = np.dstack([rgb, alpha * 255]).astype(np.uint8)
    return Image.fromarray(img, "RGBA")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=3)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    os.makedirs(OUT, exist_ok=True)
    for name, draw in (("stream", stream), ("curtain", curtain)):
        path = os.path.join(OUT, name + ".tga")
        draw(rng).save(path)
        print(path)


if __name__ == "__main__":
    main()
