"""How big the retail levels draw each texture, so the tech demo draws its
textures at the same size: a scale per texture for the .map (the texture's
repeat in units over its image's width in pixels).

The repeats were measured from the retail BSPs' faces (the median over every
face that has the texture); tex_scales.json holds the scales that came out,
numbers only. `scale(name)` gives a texture's, or 1 (what the retail maps use
for most textures) for one that wasn't measured.

    tools/techdemo/texscale.py <retail_repeats.json>   # remake tex_scales.json
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, "tex_scales.json")
_t = None


def table():
    global _t
    if _t is None:
        try:
            _t = json.load(open(TABLE))
        except (OSError, ValueError):
            _t = {}
    return _t


def scale(name, default=1.0):
    """(xscale, yscale, image w, image h) for textures/<name>."""
    e = table().get(name.lower())
    return (e[0], e[1], e[2], e[3]) if e else (default, default, 256, 256)


def remake(repeats_json):
    import io
    import re
    sys.path.insert(0, os.path.join(HERE, ".."))
    import paks
    from PIL import Image
    idx = paks.index()
    files = {}
    for pak, fs in idx.items():
        if pak.lower().startswith("pak"):
            for f in fs:
                files[f.lower()] = (pak, f)
    rep = json.load(open(repeats_json))
    out = {}
    for shader, (su, tu, n) in rep.items():
        name = shader[len("textures/"):]
        img = None
        for ext in (".tga", ".jpg"):
            if "textures/" + name + ext in files:
                img = files["textures/" + name + ext]
                break
        if not img:
            continue
        try:
            w, h = Image.open(io.BytesIO(paks.read(*img))).size
        except Exception:  # noqa: BLE001
            continue
        out[name] = [round(su / w, 3), round(tu / h, 3), w, h, n]
    json.dump(out, open(TABLE, "w"), indent=0, sort_keys=True)
    print(len(out), "textures")


if __name__ == "__main__":
    remake(sys.argv[1])
