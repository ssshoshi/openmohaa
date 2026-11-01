#!/usr/bin/env python3
"""Draws the rain runoff textures (content/opm-rain/textures/opm_rain/*.tga).

curtain.tga: the veil of water that leaves a roof's edge all along it, many
short streaks falling and the drops they break into (tiling both ways).
crown.tga, drop.tga, mist.tga: the splash where it lands, the spray thrown up
and out, the drops flying off, the fine mist.
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


def drop(rng):
    """A drop of water: round, soft at its rim, a highlight high on it."""
    n = 32
    y, x = np.mgrid[0:n, 0:n] / (n - 1) * 2 - 1
    r = np.sqrt(x * x + y * y)
    alpha = np.clip(1.0 - r, 0.0, 1.0) ** 0.8
    hi = np.exp(-((x + 0.3) ** 2 + (y + 0.35) ** 2) / 0.05)
    rgb = np.clip(190 + 65 * hi, 0, 255)[..., None].repeat(3, axis=2)
    return Image.fromarray(np.dstack([rgb, alpha * 255]).astype(np.uint8), "RGBA")


def value_noise2(rng, n, cells):
    knots = rng.random((cells + 1, cells + 1))
    knots[-1, :] = knots[0, :]
    knots[:, -1] = knots[:, 0]
    t = np.arange(n) * cells / n
    i = np.floor(t).astype(int)
    f = t - i
    f = f * f * (3 - 2 * f)
    a = knots[i][:, i] * (1 - f)[None, :] + knots[i][:, i + 1] * f[None, :]
    b = knots[i + 1][:, i] * (1 - f)[None, :] + knots[i + 1][:, i + 1] * f[None, :]
    return a * (1 - f)[:, None] + b * f[:, None]


def mist(rng):
    """A puff of the fine spray thrown up where water lands: soft, uneven."""
    n = 128
    y, x = np.mgrid[0:n, 0:n] / (n - 1) * 2 - 1
    r = np.sqrt(x * x + y * y)
    body = np.clip(1.0 - r, 0.0, 1.0) ** 1.6
    noise = 0.5 * value_noise2(rng, n, 4) + 0.3 * value_noise2(rng, n, 9) + 0.2 * value_noise2(rng, n, 21)
    alpha = np.clip(body * (0.35 + 0.9 * noise), 0.0, 1.0)
    rgb = np.full((n, n, 3), 225.0)
    return Image.fromarray(np.dstack([rgb, alpha * 255]).astype(np.uint8), "RGBA")


def crown(rng):
    """The crown of a splash: spray thrown up and out from where water lands
    (the bottom middle), in streaks fanning out, drops at their ends."""
    n = 128
    alpha = np.zeros((n, n))
    y, x = np.mgrid[0:n, 0:n].astype(float)
    ox, oy = n / 2, n - 4
    for _ in range(90):
        ang = np.radians(rng.normal(0.0, 40.0))
        reach = n * (0.12 + 0.55 * rng.random() ** 2)
        dx, dy = np.sin(ang), -np.cos(ang)
        px, py = x - ox, y - oy
        along = px * dx + py * dy
        across = np.abs(px * dy - py * dx)
        t = along / reach
        width = 0.7 + 1.6 * (1 - t)
        line = np.clip(1.0 - across / width, 0.0, 1.0) * (t > 0) * (t < 1) * (0.3 + 0.7 * t)
        np.maximum(alpha, line * (0.5 + 0.5 * rng.random()), out=alpha)
        # the drop it ends in
        ex, ey = ox + dx * reach, oy + dy * reach
        d = np.sqrt((x - ex) ** 2 + (y - ey) ** 2) / (1.2 + 1.5 * rng.random())
        np.maximum(alpha, np.clip(1.0 - d, 0.0, 1.0), out=alpha)
    # the foam at its foot
    foot = np.exp(-(((x - ox) / (n * 0.22)) ** 2 + ((y - oy) / (n * 0.08)) ** 2))
    alpha = np.clip(np.maximum(alpha, foot * 0.8), 0.0, 1.0)
    rgb = np.clip(200 + 55 * alpha, 0, 255)[..., None].repeat(3, axis=2)
    return Image.fromarray(np.dstack([rgb, alpha * 255]).astype(np.uint8), "RGBA")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=3)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    os.makedirs(OUT, exist_ok=True)
    for name, draw in (("stream", stream), ("curtain", curtain), ("drop", drop), ("mist", mist), ("crown", crown)):
        path = os.path.join(OUT, name + ".tga")
        draw(rng).save(path)
        print(path)


if __name__ == "__main__":
    main()
