#!/usr/bin/env python3
"""Draws the gore textures (content/opm-gore/textures/opm_gore/*.tga).

They are made here rather than painted so that the fork carries nothing it
cannot publish. Each one fades to nothing well inside its border: the shaders
clamp them, and the cgame lays them over whole triangles of the body, which
reach past the wound.

    tools/content/gore_textures.py          # rewrite every texture
    tools/content/gore_textures.py --seed 7 # another draw of the same
"""

import argparse
import os

import numpy as np
from PIL import Image

OUT = os.path.join(os.path.dirname(__file__), "..", "..", "content", "opm-gore", "textures", "opm_gore")

HOLE = np.array([10, 1, 1], float)
DRIED = np.array([46, 4, 3], float)
FRESH = np.array([96, 7, 5], float)
FLESH = np.array([120, 36, 30], float)
SOOT = np.array([16, 13, 11], float)


def noise(rng, size, cells, octaves=4):
    """Fractal value noise in [0, 1], size (h, w)."""
    h, w = size
    out = np.zeros(size)
    amp, total = 1.0, 0.0
    for o in range(octaves):
        c = cells * 2 ** o
        grid = rng.random((c + 1, c + 1))
        img = Image.fromarray((grid * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC)
        out += amp * np.asarray(img, float) / 255.0
        total += amp
        amp *= 0.5
    return out / total


def polar(size, centre=(0.5, 0.5)):
    """Distance from centre (1 at the half width) and angle, per pixel."""
    h, w = size
    y, x = np.mgrid[0:h, 0:w]
    dx = (x + 0.5) / w - centre[0]
    dy = (y + 0.5) / h - centre[1]
    return np.hypot(dx, dy) * 2.0, np.arctan2(dy, dx)


def ragged(rng, ang, lobes, amount):
    """A radius multiplier that wanders with the angle, closing on itself."""
    r = np.ones_like(ang)
    for k in range(2, lobes + 8):
        r += amount * rng.random() / k ** 1.3 * np.sin(k * ang + rng.random() * 6.283)
    return np.maximum(r, 0.35)


def smooth(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def dots(rng, size, n, rmin, rmax, spread, centre=(0.5, 0.5)):
    """Alpha of n round droplets scattered about the centre."""
    h, w = size
    y, x = np.mgrid[0:h, 0:w]
    a = np.zeros(size)
    for _ in range(n):
        ang = rng.random() * 6.283
        dist = spread[0] + rng.random() * (spread[1] - spread[0])
        cx = (centre[0] + np.cos(ang) * dist / 2) * w
        cy = (centre[1] + np.sin(ang) * dist / 2) * h
        r = (rmin + rng.random() * (rmax - rmin)) * w / 2
        d = np.hypot(x + 0.5 - cx, y + 0.5 - cy)
        a = np.maximum(a, smooth(r, r * 0.6, d))
    return a


def save(name, rgb, alpha, round_=True):
    h, w = alpha.shape
    # Nothing at the border, however the noise fell: the shaders clamp, and the
    # cgame cuts the decal to the square, which would show.
    y, x = np.mgrid[0:h, 0:w]
    border = np.minimum.reduce([x, y, w - 1 - x, h - 1 - y])
    alpha = alpha * smooth(0, 2, border)
    if round_:
        alpha = alpha * smooth(1.0, 0.8, polar((h, w))[0])
    img = np.dstack([np.clip(rgb, 0, 255), np.clip(alpha * 255, 0, 255)]).astype(np.uint8)
    os.makedirs(OUT, exist_ok=True)
    Image.fromarray(img, "RGBA").save(os.path.join(OUT, name), compression="tga_rle")


def mix(a, b, t):
    t = np.clip(t, 0, 1)[..., None]
    return a * (1 - t) + b * t


def wound(rng, size, hole, rim, stain, jag, spatter, flesh):
    """A bullet wound: a dark hole, a raw rim and the cloth soaked round it."""
    r, ang = polar((size, size))
    n = noise(rng, (size, size), 4)
    rr = r / ragged(rng, ang, 9, jag)
    soak = rr / (stain * (0.75 + 0.5 * n))

    alpha = smooth(1.0, 0.55, soak) * 0.92
    alpha = np.maximum(alpha, smooth(rim * 1.1, rim * 0.9, rr))
    alpha = np.maximum(alpha, dots(rng, (size, size), spatter, 0.02, 0.06, (stain * 0.9, 0.9)) * 0.9)

    rgb = mix(FRESH[None, None], DRIED[None, None], soak * 1.1 - 0.15 + (n - 0.5) * 0.6)
    rgb = mix(rgb, FLESH[None, None], smooth(rim, hole, rr) * flesh * (0.6 + 0.6 * n))
    rgb = mix(rgb, HOLE[None, None], smooth(hole * 1.15, hole * 0.8, rr))
    return rgb, alpha


def run(rng, w, h):
    """Blood running down from a wound at the top centre, in a few streaks."""
    y, x = np.mgrid[0:h, 0:w]
    u = (x + 0.5) / w
    v = (y + 0.5) / h
    n = noise(rng, (h, w), 3)
    alpha = np.zeros((h, w))
    for i in range(rng.integers(2, 5)):
        x0 = 0.5 + (rng.random() - 0.5) * 0.5
        length = 0.35 + rng.random() * 0.55
        width = 0.09 + rng.random() * 0.08
        wobble = (noise(rng, (h, 1), 3, 2)[:, 0] - 0.5) * 0.18
        cx = x0 + wobble[:, None] * v
        # thins as it runs, ends in a drop
        half = width * (1.0 - 0.6 * v / length)
        line = smooth(half, half * 0.5, np.abs(u - cx)) * smooth(length, length - 0.04, v)
        bulb = smooth(width * 0.9, width * 0.5, np.hypot((u - cx) , (v - length) * h / w))
        alpha = np.maximum(alpha, np.maximum(line, bulb))
    # the soaked patch it starts from
    alpha = np.maximum(alpha, smooth(0.25, 0.05, np.hypot(u - 0.5, (v - 0.02) * h / w * 0.5)))
    alpha *= 0.75 + 0.25 * n
    rgb = mix(FRESH[None, None], DRIED[None, None], v * 0.8 + (n - 0.5) * 0.5)
    return rgb, alpha


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=1944)
    rng = np.random.default_rng(ap.parse_args().seed)

    # entry: a neat hole, little torn
    save("wound_entry.tga", *wound(rng, 128, hole=0.17, rim=0.27, stain=0.62, jag=0.18, spatter=5, flesh=0.5))
    # exit: torn open, flesh showing, and more thrown about it
    save("wound_exit.tga", *wound(rng, 128, hole=0.24, rim=0.4, stain=0.8, jag=0.55, spatter=12, flesh=1.0))
    # shrapnel: small, ragged
    save("wound_frag.tga", *wound(rng, 64, hole=0.16, rim=0.3, stain=0.7, jag=0.6, spatter=3, flesh=0.8))
    save("wound_run.tga", *run(rng, 64, 256), round_=False)

    # soot from a blast close by
    r, ang = polar((128, 128))
    n = noise(rng, (128, 128), 3)
    save("burn.tga", np.broadcast_to(SOOT, (128, 128, 3)) * (0.7 + 0.6 * n[..., None]),
         np.clip(1.0 - r / ragged(rng, ang, 7, 0.35) / (0.55 + 0.5 * n), 0, 1) ** 0.9 * 0.6)

    # a drop in the air
    r, _ = polar((32, 32))
    save("drop.tga", np.broadcast_to(FRESH, (32, 32, 3)) * (1.2 - 0.6 * r[..., None]), smooth(0.9, 0.4, r))

    # where a drop lands
    r, ang = polar((64, 64))
    n = noise(rng, (64, 64), 3)
    core = smooth(0.45, 0.3, r / ragged(rng, ang, 12, 0.6))
    a = np.maximum(core, dots(rng, (64, 64), 7, 0.04, 0.1, (0.5, 0.95)))
    save("splat.tga", mix(FRESH[None, None], DRIED[None, None], 0.4 + n * 0.5), a * 0.95)

    # the pool under a body: fills out to its edge as it grows
    r, ang = polar((256, 256))
    n = noise(rng, (256, 256), 3)
    rr = r / ragged(rng, ang, 10, 0.35) / (0.85 + 0.25 * n)
    rgb = mix(np.array([62, 5, 4], float)[None, None], np.array([26, 2, 2], float)[None, None], smooth(0.9, 0.3, rr) + (n - 0.5) * 0.6)
    save("pool.tga", rgb, smooth(0.9, 0.78, rr) * 0.94)


if __name__ == "__main__":
    main()
