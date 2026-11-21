#!/usr/bin/env python3
"""The glass of every map's windows (func_window), for breaking a piece at a time.

cgame (cg_glass.cpp) draws a window once it is hit, cut into small pieces. For
each texture a func_window in any map is drawn with, this writes, from the
player's own paks (never committed: they are the game's):

    textures/opm/glass/<texture>.tga   the texture's frame alone: its colour,
                                       opaque where the window's bars are
                                       (alpha FRAME and up in the game's) and
                                       clear elsewhere. The game's glass has
                                       broken shards painted into it, half
                                       see-through, which drawn blended made a
                                       whole window look broken already; cgame
                                       reads this for which pieces are frame
    scripts/opm_glass.shader           for a texture with a frame, <texture>
                                       itself in place of the game's (a sheet
                                       of reflection a fifth opaque over the
                                       whole window, the frame alpha tested): a
                                       faint tint, the frame over it, a sheen;
                                       and for every texture <texture>_tiles,
                                       what cgame draws the pieces with, lit by
                                       their colour (sorting after the game's
                                       general_structure.shader, it wins)

    tools/glass/window_glass.py --out DIR

content/opm-glass runs it (glass.generate). MOHAA_GAME is the folder with main/.
"""

import argparse
import io
import os
import re
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import paks  # noqa: E402

FRAME = 240        # a window texture's alpha from this up is its frame
OWN_PAK = "opm-glass"
LUMP_SHADERS, LUMP_SURFACES, LUMP_MODELS, LUMP_ENTITIES = 0, 3, 13, 14
SURF_NODRAW = 0x80
SURFACE_SIZE = 108
# The glass, whole or in pieces the same: a fixed tint (not lit: the pieces
# have no lightmap, and lit by the grid they went near clear indoors while the
# whole pane lit by its lightmap did not), between clear and a sheet
TINT = "0.20 0.22 0.23"
TINT_ALPHA = "0.28"
SHEEN = "0.16 0.16 0.16"


def lump(data, version, n):
    if version <= 18 and n > 12:
        n += 1
    return struct.unpack_from("<ii", data, 12 + 8 * n)


def window_textures(data):
    """The textures a map's func_windows are drawn with."""
    if data[:4] != b"2015":
        return set()
    version = struct.unpack_from("<i", data, 4)[0]
    so, sl = lump(data, version, LUMP_SHADERS)
    su, sul = lump(data, version, LUMP_SURFACES)
    mo, _ = lump(data, version, LUMP_MODELS)
    eo, el = lump(data, version, LUMP_ENTITIES)
    if sul % SURFACE_SIZE:
        return set()
    shaders = []
    for i in range(sl // 140):
        name = data[so + 140 * i:so + 140 * i + 64].split(b"\0")[0].decode("latin1")
        flags = struct.unpack_from("<i", data, so + 140 * i + 64)[0]
        shaders.append((name, flags))
    out = set()
    text = data[eo:eo + el].decode("latin1", "replace")
    for block in re.findall(r"\{[^{}]*\}", text):
        if not re.search(r'"classname"\s+"func_window"', block, re.I):
            continue
        debris = re.search(r'"debristype"\s+"(\d+)"', block)
        if debris and debris.group(1) != "0":
            continue  # not glass
        m = re.search(r'"model"\s+"\*(\d+)"', block)
        if not m:
            continue
        k = int(m.group(1))
        first, count = struct.unpack_from("<ii", data, mo + 40 * k + 24)
        for s in range(first, first + count):
            shader = struct.unpack_from("<i", data, su + SURFACE_SIZE * s)[0]
            if 0 <= shader < len(shaders):
                name, flags = shaders[shader]
                if not flags & SURF_NODRAW and "common/" not in name.lower():
                    out.add(name)
    return out


def winner(idx, path):
    have = [p for p in paks.holders(idx, path) if OWN_PAK not in p.lower()]
    return have[-1] if have else None


def mangled(texture):
    return texture.replace("/", "_")


def own_shader(texture, frame):
    return f"""{texture}
{{
	qer_editorimage {texture}.tga
	surfaceparm glass
	surfaceparm trans
	{{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const {TINT}
		alphaGen const {TINT_ALPHA}
	}}
	{{
		map {frame}
		blendFunc blend
		rgbGen identity
	nextbundle
		map $lightmap
	}}
	{{
		map textures/opm/windows/sheen.tga
		tcGen environment
		blendFunc add
		rgbGen const {SHEEN}
	}}
}}
"""


def tiles_shader(texture, frame):
    return f"""{texture}_tiles
{{
	cull none
	surfaceparm nolightmap
	surfaceparm nomarks
	{{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const {TINT}
		alphaGen const {TINT_ALPHA}
	}}
	{{
		map {frame}
		blendFunc blend
		rgbGen vertex
	}}
	{{
		map textures/opm/windows/sheen.tga
		tcGen environment
		blendFunc add
		rgbGen const {SHEEN}
	}}
}}
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    idx = paks.index()
    bsps = {}
    for pak, files in idx.items():
        for f in files:
            if f.lower().startswith("maps/") and f.lower().endswith(".bsp"):
                bsps[f.lower()] = (pak, f)  # the later pak wins
    textures = {}
    for low, (pak, f) in sorted(bsps.items()):
        for t in window_textures(paks.read(pak, f)):
            textures.setdefault(t, []).append(os.path.basename(f)[:-4])

    out_shaders = [
        "// The glass of the maps' windows (func_window), broken a piece at a time by\n"
        "// cgame cg_glass.cpp: written by tools/glass/window_glass.py from the game's\n"
        "// own textures (content/opm-glass).\n"
    ]
    os.makedirs(os.path.join(args.out, "textures", "opm", "glass"), exist_ok=True)
    os.makedirs(os.path.join(args.out, "scripts"), exist_ok=True)

    for texture in sorted(textures):
        pak = winner(idx, texture + ".tga")
        frame = None
        if pak:
            img = Image.open(io.BytesIO(paks.read(pak, texture + ".tga")))
            if img.mode == "RGBA":
                a = np.array(img)
                a[..., 3] = np.where(a[..., 3] >= FRAME, 255, 0).astype(np.uint8)
                frame = f"textures/opm/glass/{mangled(texture)}.tga"
                Image.fromarray(a, "RGBA").save(os.path.join(args.out, frame))
        print(f"{texture}: {', '.join(textures[texture][:6])}{' ...' if len(textures[texture]) > 6 else ''}"
              f"{'' if frame else ' (no alpha: no frame)'}")
        image = frame or texture + (".tga" if pak else ".jpg")
        if frame:
            # in place of the game's: its glass was a 0.2 sheet of reflection
            # over the frame (alpha tested), and drawn blended its painted
            # shards showed, a window broken before it was hit
            out_shaders.append(own_shader(texture, frame))
        out_shaders.append(tiles_shader(texture, image))

    with open(os.path.join(args.out, "scripts", "opm_glass.shader"), "w", newline="\n") as f:
        f.write("\n".join(out_shaders))


if __name__ == "__main__":
    main()
