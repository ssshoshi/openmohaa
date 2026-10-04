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

# Dark, as blood is: near HRRTM's blood (about 21 2 0), a little lighter so a
# wound still reads.
DRIED = np.array([30, 3, 2], float)
FRESH = np.array([58, 5, 4], float)
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


# Blood and flesh, a few ways: fresh and bright, dark, clotted brown, torn pink.
# (wet blood, dried blood, raw flesh, pale tissue)
PALETTES = [
    ([66, 6, 5], [34, 3, 2], [84, 22, 18], [112, 62, 54]),
    ([46, 4, 4], [24, 2, 2], [66, 15, 13], [92, 50, 44]),
    ([54, 12, 7], [30, 8, 4], [78, 30, 22], [108, 76, 60]),
    ([60, 7, 8], [32, 4, 5], [92, 38, 34], [120, 82, 74]),
]


def wound(rng, size, palette, meat, stain, jag, spatter, tissue):
    """A bullet wound: torn, wet flesh in the middle, no hole to see into, and
    the cloth soaked round it."""
    wet, dried, flesh, pale = (np.array(c, float)[None, None] for c in palette)
    r, ang = polar((size, size))
    n = noise(rng, (size, size), 4)
    fine = noise(rng, (size, size), 12, 3)
    rr = r / ragged(rng, ang, 9, jag)
    soak = rr / (stain * (0.75 + 0.5 * n))

    alpha = smooth(1.0, 0.55, soak) * 0.92
    alpha = np.maximum(alpha, smooth(meat * 1.15, meat * 0.85, rr))
    alpha = np.maximum(alpha, dots(rng, (size, size), spatter, 0.02, 0.06, (stain * 0.9, 0.9)) * 0.9)

    rgb = mix(wet, dried, soak * 1.1 - 0.15 + (n - 0.5) * 0.6)
    # the torn middle: mottled flesh, wet blood pooled in its folds, and a
    # little pale tissue showing
    inside = smooth(meat, meat * 0.6, rr)
    torn = mix(flesh, wet * 0.8, smooth(0.45, 0.65, fine))
    torn = mix(torn, pale, smooth(0.78, 0.9, fine) * tissue)
    rgb = mix(rgb, torn, inside)
    # the edge of the tear, darker and glistening by turns
    rgb = mix(rgb, wet * 0.6, smooth(meat * 0.7, meat, rr) * smooth(meat * 1.3, meat, rr) * 0.7)
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


VARIANTS = 4


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=1944)
    rng = np.random.default_rng(ap.parse_args().seed)

    # Several of each, the cgame picks one at random (cg_gore.cpp GORE_VARIANTS).
    for v in range(VARIANTS):
        pal = PALETTES[v % len(PALETTES)]
        # entry: small and fairly neat
        save(f"wound_entry{v + 1}.tga", *wound(rng, 128, pal, meat=0.2 + 0.05 * rng.random(), stain=0.55 + 0.15 * rng.random(),
                                              jag=0.15 + 0.2 * rng.random(), spatter=int(3 + 5 * rng.random()), tissue=0.3))
        # exit: torn open, more flesh, more thrown about it
        save(f"wound_exit{v + 1}.tga", *wound(rng, 128, pal, meat=0.32 + 0.1 * rng.random(), stain=0.75 + 0.15 * rng.random(),
                                             jag=0.45 + 0.25 * rng.random(), spatter=int(8 + 8 * rng.random()), tissue=1.0))
        # shrapnel: small, ragged
        save(f"wound_frag{v + 1}.tga", *wound(rng, 64, pal, meat=0.25, stain=0.7, jag=0.6, spatter=3, tissue=0.6))
    save("wound_run.tga", *run(rng, 64, 256), round_=False)

    # where a limb came off: torn flesh about a pale bone, wet blood round it
    r, ang = polar((128, 128))
    n = noise(rng, (128, 128), 4)
    fine = noise(rng, (128, 128), 14, 3)
    rr = r / ragged(rng, ang, 11, 0.35)
    rgb = mix(np.array([84, 22, 18], float)[None, None], np.array([46, 5, 4], float)[None, None], smooth(0.45, 0.62, fine))
    rgb = mix(rgb, np.array([30, 3, 2], float)[None, None], smooth(0.55, 0.95, rr))
    bone = smooth(0.2, 0.14, polar((128, 128), (0.5 + 0.04 * rng.random(), 0.5))[0] / (0.9 + 0.2 * n))
    rgb = mix(rgb, np.array([150, 132, 104], float)[None, None] * (0.8 + 0.3 * fine[..., None]), bone)
    rgb = mix(rgb, np.array([40, 30, 22], float)[None, None], bone * smooth(0.12, 0.06, r))  # the marrow
    save("stump.tga", rgb, smooth(1.0, 0.75, rr) * 0.97)

    # the inside of a head: grey-pink matter in lumps, dark blood in between,
    # and the broken edge of the skull about the rim
    r, ang = polar((128, 128))
    lumps = noise(rng, (128, 128), 9, 3)
    n = noise(rng, (128, 128), 4)
    rr = r / ragged(rng, ang, 12, 0.45)
    rgb = mix(np.array([92, 58, 56], float)[None, None], np.array([110, 74, 70], float)[None, None], smooth(0.4, 0.7, lumps))
    rgb = mix(rgb, np.array([40, 4, 3], float)[None, None], smooth(0.5, 0.35, lumps) * 0.9)
    rgb = mix(rgb, np.array([36, 3, 2], float)[None, None], smooth(0.4, 0.8, n) * 0.5)
    rim = smooth(0.62, 0.72, rr) * smooth(0.86, 0.76, rr) * smooth(0.45, 0.6, noise(rng, (128, 128), 12, 2))
    rgb = mix(rgb, np.array([150, 136, 112], float)[None, None], rim)
    rgb = mix(rgb, np.array([40, 4, 3], float)[None, None], smooth(0.78, 0.95, rr))
    save("brain.tga", rgb, smooth(1.0, 0.82, rr) * 0.97)

    # a bit of a head in the air
    r, ang = polar((32, 32))
    n = noise(rng, (32, 32), 4)
    rr = r / ragged(rng, ang, 6, 0.5)
    rgb = mix(np.array([80, 20, 16], float)[None, None], np.array([110, 72, 64], float)[None, None], smooth(0.5, 0.8, n))
    save("chunk.tga", rgb, smooth(0.95, 0.7, rr))

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
