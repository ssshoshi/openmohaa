#!/usr/bin/env python3
"""The swinging lantern's cut-out card, without its speckled glass.

models/lights/swinging_lantern.tik draws a flat card through the lantern's
middle (glanterncull: glantern.tga, alphaFunc GE128, depthWrite), a side view
of it that carries its wire handle. The glass in that texture has an alpha of
about 120, just under the cut, so the game drops it; the 4x upscale
(tools/texupscale, the .dds GL2 always loads first) put noise on it, and a
third of the glass passed the cut: a speckled grey plane through the lantern
that the flame was clipped by. This writes the card's texture as the upscale
made it (colour, and its smooth alpha), the glass faded out of it, found in
the original by its half-clear alpha:

    tools/lamps/lantern_cull.py --out DIR   ->  DIR/textures/opm/lamps/glantern_cull.tga

from the player's own paks (content/opm-lamp-states runs it, lantern.generate).
MOHAA_GAME is the folder with main/.
"""

import argparse
import glob
import io
import os
import zipfile

import numpy as np
from PIL import Image

GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")
BASE = "textures/models/lights/glantern"
OUT = "textures/opm/lamps/glantern_cull.tga"


def game_files():
    files = {}
    for pak in sorted(glob.glob(os.path.join(GAME, "main", "*.pk3"))):
        if os.path.basename(pak).lower().startswith("zzzzzzzzz-opm-lamp-states"):
            continue
        with zipfile.ZipFile(pak) as z:
            for n in z.namelist():
                files[n.lower()] = (pak, n)
    return files


def read(files, path):
    pak, name = files[path]
    with zipfile.ZipFile(pak) as z:
        img = Image.open(io.BytesIO(z.read(name)))
        img.load()
    return img.convert("RGBA")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    files = game_files()
    cut = read(files, BASE + ".tga")
    colour = read(files, BASE + ".dds") if BASE + ".dds" in files else cut
    w, h = colour.size
    # the glass: the original's half-clear texels (its alpha about 120), not the
    # frame (255) nor the clear round it (0); faded out of the upscale's alpha,
    # whose edges elsewhere stay as smooth as it made them
    a = np.asarray(cut)[..., 3].astype(np.float32)
    glass = ((a >= 60) & (a <= 140)).astype(np.float32)
    glass = np.asarray(Image.fromarray((glass * 255).astype(np.uint8)).resize((w, h), Image.BILINEAR),
                       dtype=np.float32) / 255.0
    glass = np.clip((glass - 0.25) / 0.5, 0, 1)

    rgba = np.asarray(colour).copy()
    rgba[..., 3] = (rgba[..., 3].astype(np.float32) * (1 - glass)).astype(np.uint8)
    keep = rgba[..., 3] >= 128
    path = os.path.join(args.out, OUT)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    Image.fromarray(rgba, "RGBA").save(path)
    print("%s: %dx%d, %.1f%% kept" % (OUT, w, h, 100.0 * keep.mean()))


if __name__ == "__main__":
    main()
