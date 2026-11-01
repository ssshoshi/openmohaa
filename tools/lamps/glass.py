"""The remodelled lamps' glass (data/opm-lamps/textures/opm/lamps/<lamp>_glass.tga).

A lamp's glass shared its texture with its frame: a few dozen texels of it,
smeared with the frame's brown and the shade's painted rim. The remodelled
glass (remodel_blender.py) is wrapped round each shade instead, u round it,
v from its neck (0) down to its rim (1), and takes one of these: frosted
glass lit from inside, in the colour lamps.txt gives it (`glass r g b`),
brightest at the bulb, a little darker at the neck, a faint lit rim, the
frosting barely there. Even round the shade, so the seam where u wraps does
not show. Seeded: the same file every run.
"""

import os

import numpy as np
from PIL import Image

W, H = 64, 128
OUT = os.path.join(os.path.dirname(__file__), "..", "..", "data", "opm-lamps", "textures", "opm", "lamps")


def glass_image(tint, seed=0):
    rng = np.random.default_rng(seed)
    v = (np.arange(H) + 0.5) / H
    # brightest at the bulb (a little below the middle), darker at the neck
    glow = 0.72 + 0.28 * np.exp(-((v - 0.55) / 0.32) ** 2)
    glow *= 0.86 + 0.14 * np.clip(v / 0.25, 0, 1)
    # the rim: the glass's edge catches the light
    glow += 0.06 * np.exp(-((v - 0.95) / 0.03) ** 2)
    lum = np.repeat(glow[:, None], W, axis=1)
    # frosting: soft noise, round the shade it repeats (the seam)
    noise = rng.normal(0, 1, (H // 4, W // 4))
    noise = np.array(Image.fromarray(((noise * 0.5 + 0.5) * 255).clip(0, 255).astype(np.uint8))
                     .resize((W, H), Image.BICUBIC)) / 255.0 - 0.5
    lum = lum * (1 + 0.05 * noise)
    rgb = np.clip(lum[:, :, None] * np.array(tint)[None, None, :], 0, 1)
    alpha = np.ones((H, W, 1))
    return Image.fromarray((np.concatenate([rgb, alpha], axis=2) * 255).round().astype(np.uint8), "RGBA")


def write(name, tint):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + "_glass.tga")
    glass_image(tint, seed=sum(map(ord, name))).save(path)
    return path
