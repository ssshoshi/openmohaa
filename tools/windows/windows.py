#!/usr/bin/env python3
"""Lit windows remade: sharper frames, glass lit from the room behind it.

The game's lit windows (textures/light/*lit) are a frame texture over the
lightmap and a "_blend" texture added on top for the lit glass, both small
(64x128 to 256x128) and soft, the glass a flat, blotchy wash. For each window
in WINDOWS this writes, from the player's own paks (never committed: they are
the game's):

    textures/opm/windows/<name>_frame.tga   the frame texture, 4x (Real-ESRGAN)
    textures/opm/windows/<name>_glow.tga    the glass lit from behind: the lit
                                            panes 4x, their blotches smoothed,
                                            brighter where the room's lamp would
                                            be (high in the middle) and darker in
                                            the pane corners, with a fine frost
    textures/opm/windows/<name>_mask.tga    the glass alone, for the sheen

and, all its own, textures/opm/windows/sheen.tga: a soft reflection of a night
sky for the glass (tcGen environment). The shaders are content/opm-windows'
patch of scripts/light.shader.

    tools/windows/windows.py --out DIR            every window
    tools/windows/windows.py --out DIR --sheet    and DIR/sheet.png, before and after

Without torch the 4x is Lanczos (softer). MOHAA_GAME is the folder with main/.
"""

import argparse
import glob
import io
import os
import sys
import zipfile

import numpy as np
from PIL import Image

GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")
HERE = os.path.dirname(os.path.abspath(__file__))

# shader name: (frame texture, lit glass texture, how bright its glass: it blooms,
# brighter than white, so a glass that was near white already is let down)
WINDOWS = {
    "jh_rustwin1lit": ("textures/algiers/jh_rustwin1.tga", "textures/light/jh_rustwin1_blend.tga", 0.85),
    "window4_exteriorlit": ("textures/light/window4_exteriorlit.tga", "textures/light/window4_exteriorlit_blend.tga", 0.85),
    "jh_window1pj_lit": ("textures/light/jh_window1pj_lit.tga", "textures/light/jh_window1pj_lit_blend.tga", 0.75),
    "ihwin1a_framelit": ("textures/light/ihwin1a_framelit.tga", "textures/light/ihwin1a_framelit_blend.tga", 0.55),
}

OUT_DIR = "textures/opm/windows"
MAX_SIDE = 1024
GLASS_THRESHOLD = 0.06  # a lit-glass texel brighter than this is glass


def game_files():
    """Every file the game loads, lowercase path -> (pak, name): later paks win, ours left out."""
    files = {}
    for pak in sorted(glob.glob(os.path.join(GAME, "main", "*.pk3"))):
        if os.path.basename(pak).lower().startswith("zzzzzzzzz-opm-windows"):
            continue
        with zipfile.ZipFile(pak) as z:
            for n in z.namelist():
                files[n.lower()] = (pak, n)
    return files


def read_rgb(files, path):
    pak, name = files[path.lower()]
    with zipfile.ZipFile(pak) as z:
        img = Image.open(io.BytesIO(z.read(name)))
        img.load()
    return np.asarray(img.convert("RGB"), dtype=np.float32) / 255.0


def make_upscaler():
    """4x: Real-ESRGAN when torch is there, else Lanczos."""
    try:
        sys.path.insert(0, os.path.join(HERE, "..", "texupscale"))
        import esrgan  # noqa: E402

        net = esrgan.Upscaler()

        def up(rgb):
            h, w = rgb.shape[:2]
            pad = min(16, h, w)
            p = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode="symmetric")
            return net.upscale(p)[pad * 4:(pad + h) * 4, pad * 4:(pad + w) * 4]

        print("upscaling with Real-ESRGAN on %s" % net.name())
        return up
    except Exception as e:  # no torch, no CUDA, no weights
        print("upscaling with Lanczos (%s)" % e)

        def up(rgb):
            h, w = rgb.shape[:2]
            return np.asarray(Image.fromarray((rgb * 255).astype(np.uint8)).resize((w * 4, h * 4), Image.LANCZOS),
                              dtype=np.float32) / 255.0

        return up


def fit(rgb, side=MAX_SIDE):
    h, w = rgb.shape[:2]
    k = min(1.0, side / max(h, w))
    if k >= 1.0:
        return rgb
    img = Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8))
    return np.asarray(img.resize((max(1, int(w * k)), max(1, int(h * k))), Image.LANCZOS), dtype=np.float32) / 255.0


def to_img(a, mode="RGB"):
    return Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8), mode if a.ndim == 3 else "L")


def blur(a, r):
    from scipy.ndimage import gaussian_filter

    sigma = (r, r, 0) if a.ndim == 3 else r
    return gaussian_filter(a.astype(np.float32), sigma=sigma, mode="reflect")


def glass(lit, gain):
    """The lit glass texture -> (glow, mask), at its own size."""
    h, w = lit.shape[:2]
    luma = lit @ np.array([0.299, 0.587, 0.114], np.float32)

    # the glass: lit texels, soft-edged a texel or two
    mask = np.clip((blur(luma, 1.0) - GLASS_THRESHOLD) / GLASS_THRESHOLD, 0, 1)
    mask = blur(mask, 0.8)

    # the room's light through it: the panes' own colour and what they show
    # (bars, leading), the blotches smoothed out of it, brighter high in the
    # middle where a lamp hangs, and falling off into each pane's corners
    smooth = blur(lit, max(2.0, min(h, w) / 96.0))
    detail = lit / np.maximum(smooth, 1e-3)
    shown = smooth * (0.55 + 0.45 * np.clip(detail, 0.0, 1.6))

    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    u, v = xx / max(1, w - 1), yy / max(1, h - 1)
    room = 0.85 + 0.25 * np.exp(-(((u - 0.5) / 0.55) ** 2 + ((v - 0.32) / 0.6) ** 2))
    # each pane darker into its corners: distance into the glass, from the mask
    inside = blur(mask, max(3.0, min(h, w) / 40.0))
    edge = 0.72 + 0.28 * np.clip(inside * 1.6, 0, 1)

    # frost: fine noise the glass scatters the light with
    rng = np.random.default_rng(1234)
    frost = blur(rng.standard_normal((h, w)).astype(np.float32), 0.9)
    frost = 1.0 + 0.06 * frost / max(1e-6, frost.std())

    glow = shown * (room * edge * frost)[..., None]
    # a little warmer and cleaner than the game's yellowed wash
    glow = glow * np.array([1.04, 0.98, 0.88], np.float32)
    glow = glow * mask[..., None] * gain
    return np.clip(glow, 0, 1), np.clip(mask, 0, 1)


def sheen(size=256):
    """What glass shows of the night outside, tiling: a faint sky, and the soft
    diagonal streaks of light a pane catches, sliding over it as one moves."""
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32) / size
    sky = 0.10 + 0.10 * (0.5 + 0.5 * np.sin(2 * np.pi * yy))
    # streaks along a 35 degree diagonal, two widths, wrapping so it tiles
    d = (xx * 1.0 + yy * 0.7) % 1.0
    wide = np.exp(-((d - 0.30) / 0.07) ** 2)
    thin = np.exp(-((d - 0.47) / 0.018) ** 2)
    rng = np.random.default_rng(99)
    vary = blur(rng.standard_normal((size, size)).astype(np.float32), 10.0)
    vary = 0.8 + 0.4 * (vary - vary.min()) / max(1e-6, vary.max() - vary.min())
    s = (sky + 0.3 * wide + 0.35 * thin) * vary
    return np.clip(np.stack([s * 0.86, s * 0.92, s * 1.0], axis=2), 0, 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--sheet", action="store_true")
    args = ap.parse_args()

    files = game_files()
    up = make_upscaler()
    out = os.path.join(args.out, OUT_DIR)
    os.makedirs(out, exist_ok=True)
    rows = []

    for name, (frame_path, lit_path, gain) in WINDOWS.items():
        if frame_path.lower() not in files or lit_path.lower() not in files:
            print("%s: not in the game's paks, left out" % name)
            continue
        frame = read_rgb(files, frame_path)
        lit = read_rgb(files, lit_path)

        frame4 = fit(up(frame) if max(frame.shape[:2]) < MAX_SIDE else frame)
        lit4 = fit(up(lit))
        glow, mask = glass(lit4, gain)

        to_img(frame4).save(os.path.join(out, name + "_frame.tga"))
        to_img(glow).save(os.path.join(out, name + "_glow.tga"))
        to_img(mask).save(os.path.join(out, name + "_mask.tga"))
        print("%s: frame %dx%d, glass %dx%d" % (name, frame4.shape[1], frame4.shape[0], glow.shape[1], glow.shape[0]))
        if args.sheet:
            rows.append([lit, lit4, glow])

    to_img(sheen()).save(os.path.join(out, "sheen.tga"))

    if args.sheet and rows:
        cell = 320
        sheet = Image.new("RGB", (cell * 3, cell * len(rows)))
        for r, row in enumerate(rows):
            for c, a in enumerate(row):
                img = to_img(a).resize((cell, cell), Image.NEAREST if c == 0 else Image.LANCZOS)
                sheet.paste(img, (c * cell, r * cell))
        sheet.save(os.path.join(args.out, "sheet.png"))


if __name__ == "__main__":
    main()
