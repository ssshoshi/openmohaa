#!/usr/bin/env python3
"""A candle's flame, drawn: the frames of a loop the lamps' flames play.

    tools/lamps/flame.py                 # data/opm-lamps/textures/opm/lamps/flame/*.tga
    tools/lamps/flame.py --sheet out.png # the frames side by side, to look at

A flame is a glow, not a solid: each frame is the light of a teardrop of
burning gas, added up through its depth (a volume, like a ray marched
through a density). Blue and faint at its foot round the wick, yellow-white
where it is thick, orange at its edges and tip. It sways and stretches with
slow noise that grows with height (the foot stays on the wick), and now and
then flutters. The loop's noise is sines with whole cycles in it, so the
last frame runs into the first.

The frames are drawn additively (opm_candleflame, opm_lamps.shader) on a
quad that turns round its upright axis to the viewer (autosprite2), each
flame of the remodelled lamps (remodel_blender.py).
"""

import argparse
import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "data", "opm-lamps", "textures", "opm", "lamps", "flame")

FRAMES = 40       # the loop, at FPS: 2.7 s
FPS = 15
W, H = 48, 96     # each frame; the quad is twice as tall as wide
FOOT = 0.14       # where the flame's foot sits in the frame, from its bottom
TIP = 0.86        # its tip, at rest
WIDTH = 0.19      # its widest half-width, of the frame's width


def loop_noise(rng, cycles, n):
    """Smooth noise over the loop: sines of whole cycles, weaker as they quicken."""
    t = np.arange(n) / n
    out = np.zeros(n)
    for k in cycles:
        out += np.sin(2 * np.pi * (k * t + rng.random())) / k ** 0.8
    return out / np.abs(out).max()


def frame(sway, lean, stretch, flutter, depth=24):
    """One frame, RGB in 0..1: the flame's light through its depth."""
    ys = (np.arange(H)[::-1] + 0.5) / H          # up the frame
    xs = (np.arange(W) + 0.5) / W - 0.5          # across it
    zs = (np.arange(depth) + 0.5) / depth - 0.5  # through it
    y, x, z = np.meshgrid(ys, xs * 1.0, zs * 0.5, indexing="ij")

    tip = TIP + stretch
    h = (y - FOOT) / (tip - FOOT)                # 0 its foot, 1 its tip
    hc = np.clip(h, 0, 1)
    # the higher, the further it sways (its foot on the wick)
    bend = hc ** 1.6
    xx = x - (sway * 0.10 + flutter * 0.05 * np.sin(9 * hc)) * bend - lean * 0.05 * hc
    # a teardrop: round below, drawn to a point above
    shape = np.clip(h + 0.05, 0, None) ** 0.45 * (1 - hc) ** 1.2
    radius = WIDTH * shape / 0.52               # at its widest, WIDTH
    radius = np.maximum(radius, 1e-4)
    r = np.sqrt(xx ** 2 + (z * 2.0) ** 2) / radius
    density = np.clip(1 - r, 0, 1) ** 1.5 * (h > -0.04) * (h < 1)

    # its colours: blue round the foot, yellow-white where thick, orange at the
    # edge and towards the tip; a dark core just over the wick
    blue = np.array([0.10, 0.16, 0.85])
    core = np.array([1.30, 0.95, 0.45])
    edge = np.array([1.20, 0.45, 0.08])
    foot = np.clip(1 - h / 0.28, 0, 1)[..., None] * (r[..., None] > 0.35)
    body = np.clip((h + 0.05) / 0.3, 0, 1)[..., None]
    rim = np.clip((r - 0.45) / 0.5, 0, 1)[..., None] ** 0.7
    colour = body * (core * (1 - rim) + edge * rim)
    colour = colour * (1 - foot) + blue * foot
    hole = 1 - 0.75 * np.exp(-((xx / 0.03) ** 2 + ((h - 0.12) / 0.12) ** 2))[..., None]
    light = (density[..., None] * colour * hole).sum(axis=2) * (9.0 / depth)
    # the brightest saturates to white, as a flame looks
    light = 1 - np.exp(-light * 2.4)
    return np.clip(light, 0, 1)


def frames():
    rng = np.random.default_rng(7)
    sway = loop_noise(rng, (1, 2, 3, 5), FRAMES)
    lean = loop_noise(rng, (1, 2, 4), FRAMES)
    stretch = 0.035 * loop_noise(rng, (2, 3, 5, 7), FRAMES)
    flutter = loop_noise(rng, (5, 7, 11), FRAMES) * np.clip(loop_noise(rng, (1, 2), FRAMES), 0, 1)
    return [frame(sway[i], lean[i], stretch[i], flutter[i]) for i in range(FRAMES)]


def to_image(rgb):
    return Image.fromarray((rgb * 255 + 0.5).astype(np.uint8), "RGB")


def names():
    return ["textures/opm/lamps/flame/flame%02d.tga" % i for i in range(FRAMES)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sheet", help="the frames side by side, to look at, not written for the game")
    args = ap.parse_args()
    imgs = [to_image(f) for f in frames()]
    if args.sheet:
        cols = 10
        sheet = Image.new("RGB", (W * 3 * cols, H * 3 * ((len(imgs) + cols - 1) // cols)), (20, 16, 12))
        for i, im in enumerate(imgs):
            sheet.paste(im.resize((W * 3, H * 3), Image.BILINEAR), ((i % cols) * W * 3, (i // cols) * H * 3))
        sheet.save(args.sheet)
        print("wrote", args.sheet)
        return
    os.makedirs(OUT, exist_ok=True)
    for name, im in zip(names(), imgs):
        im.save(os.path.join(OUT, os.path.basename(name)), compression="tga_rle")
    print("wrote %d frames to %s" % (len(imgs), os.path.relpath(OUT)))
    print("animMap %d %s" % (FPS, " ".join(names())))


if __name__ == "__main__":
    main()
