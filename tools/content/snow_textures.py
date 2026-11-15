#!/usr/bin/env python3
"""Draws the snow textures (content/opm-snow/textures/opm_snow/*.tga).

footprint.tga: a boot's print in snow (cg_snow.cpp, CG_SnowFootprint): the
sole and the heel pressed in, the snow in them in shadow, with the cleats'
bars across the sole and a faint pale rim of the snow pushed up round it.
The colour is the shade of it (the cgame lights it with the vertex colour);
the alpha is how much of the snow under it it hides. The toe is up.

Made here rather than painted, so the fork carries nothing it cannot publish.

    tools/content/snow_textures.py
"""

import os

import numpy as np
from PIL import Image

OUT = os.path.join(os.path.dirname(__file__), "..", "..", "content", "opm-snow", "textures", "opm_snow")

W, H = 64, 128


def ellipse(x, y, cx, cy, rx, ry):
    """How far inside an ellipse a point is: over 0 inside, 1 at its middle."""
    return 1.0 - np.sqrt(((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2)


def smooth(v, lo, hi):
    t = np.clip((v - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def footprint():
    y, x = np.mgrid[0:H, 0:W].astype(float)
    x = (x + 0.5) / W
    y = (y + 0.5) / H

    # the sole: wide at the ball of the foot, narrower to the waist; the heel
    sole = np.maximum(ellipse(x, y, 0.50, 0.30, 0.30, 0.24), ellipse(x, y, 0.49, 0.50, 0.22, 0.20))
    heel = ellipse(x, y, 0.50, 0.80, 0.24, 0.14)
    inside = np.maximum(sole, heel)

    pressed = smooth(inside, 0.0, 0.18)
    # bars across the sole and the heel, not pressed quite as deep
    bars = 0.5 + 0.5 * np.cos(y * H * 2 * np.pi / 9.0)
    depth = pressed * (0.72 + 0.28 * smooth(bars, 0.35, 0.65))
    # the snow pushed up round it: a pale rim just outside
    rim = smooth(inside, -0.22, -0.04) * (1.0 - smooth(inside, -0.04, 0.06))

    alpha = np.clip(depth * 0.62 + rim * 0.22, 0.0, 1.0)
    shade = np.array([0.40, 0.45, 0.56])  # the print's shadow, a little blue
    pale = np.array([1.0, 1.0, 1.0])
    mix = np.clip(rim * 0.22 / np.maximum(alpha, 1e-4), 0.0, 1.0)[..., None] * (1.0 - pressed[..., None])
    rgb = shade[None, None, :] * (1.0 - mix) + pale[None, None, :] * mix

    img = np.dstack([rgb, alpha[..., None]])
    return Image.fromarray((np.clip(img, 0.0, 1.0) * 255 + 0.5).astype(np.uint8), "RGBA")


def main():
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "footprint.tga")
    footprint().save(path)
    print(path)


if __name__ == "__main__":
    main()
