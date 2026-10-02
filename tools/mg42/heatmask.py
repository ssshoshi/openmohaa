#!/usr/bin/env python3
"""Draw the MG42 barrel's heat mask: data/opm-mg42/textures/opm/mg42_heatmask.tga.

The MG42 (models/statweapons/mg42*.tik) is one mesh on one 256x256 texture,
textures/models/weapons/mg42/mg42.tga. The glow of a hot barrel (cg_mg42.cpp,
scripts/opm_mg42.shader) is the whole model drawn again in added light,
through this mask: white where the barrel is on that texture (the perforated
jacket, the muzzle booster and the barrel tube), black elsewhere and in the
jacket's cooling slot and holes, which the texture cuts out.

The shapes are measured off the texture's layout and drawn here; nothing of
the texture itself is used.

  tools/mg42/heatmask.py
"""

import os

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "data", "opm-mg42", "textures", "opm", "mg42_heatmask.tga")
SIZE = 256

# The barrel's parts on the texture: (left, top, right, bottom).
BARREL = [
    (48, 164, 255, 237),  # the perforated jacket
    (0, 178, 34, 237),    # the muzzle booster
    (0, 0, 16, 166),      # the barrel tube
]
# The jacket's cooling slot, which tapers from right to left, and its holes.
SLOT = [(49, 181), (120, 178), (219, 166), (219, 186), (49, 186)]
HOLES = [
    (81, 194, 99, 201), (109, 192, 127, 200), (136, 192, 154, 199), (163, 191, 180, 198),
    (188, 190, 206, 197), (213, 190, 220, 197),
    (81, 212, 99, 219), (109, 212, 126, 219), (136, 212, 154, 219), (163, 212, 180, 219),
    (188, 212, 206, 219), (213, 212, 220, 219),
]


def main():
    mask = Image.new("L", (SIZE, SIZE), 0)
    draw = ImageDraw.Draw(mask)
    for box in BARREL:
        draw.rectangle(box, fill=255)
    draw.polygon(SLOT, fill=0)
    for l, t, r, b in HOLES:
        draw.rounded_rectangle((l - 1, t - 1, r + 1, b + 1), radius=(b - t) // 2 + 1, fill=0)
    # soft edges, so the glow does not end in a hard line
    mask = mask.filter(ImageFilter.GaussianBlur(1.2))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    mask.convert("RGB").save(OUT)
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
