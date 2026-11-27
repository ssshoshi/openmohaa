"""The bounds of the models the biomes place, read from the game's own models
(the SKD posed at the first frame of its TIKI's animation, at the TIKI's
scale) and cached in prop_bounds.json, so that the layout can stand furniture
against a wall or on a table without it going into either.

    tools/techdemo/propbounds.py static/bigbed.tik furniture/table.tik ...

Models face +x at yaw 0 (their front is their +x side)."""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "prop_bounds.json")
_cache = None


def load():
    global _cache
    if _cache is None:
        try:
            _cache = json.load(open(CACHE))
        except (OSError, ValueError):
            _cache = {}
    return _cache


def bounds(model):
    """(mins, maxs) of a model at yaw 0, or None when it isn't cached."""
    b = load().get(model.lower())
    return (tuple(b[0]), tuple(b[1])) if b else None


def measure(models):
    sys.path.insert(0, os.path.join(HERE, "..", "lamps"))
    sys.path.insert(0, os.path.join(HERE, ".."))
    import numpy as np
    import lamps
    c = load()
    for m in models:
        try:
            tris = lamps.mesh("models/" + m)
        except Exception as e:  # noqa: BLE001
            print("  %s: %s" % (m, e))
            continue
        if not tris:
            print("  %s: no triangles" % m)
            continue
        pts = np.array([p for t in tris for p in t[:3]])
        c[m.lower()] = [[round(float(v), 1) for v in pts.min(0)], [round(float(v), 1) for v in pts.max(0)]]
        print("  %-45s %s %s" % (m, c[m.lower()][0], c[m.lower()][1]))
    json.dump(c, open(CACHE, "w"), indent=0, sort_keys=True)


if __name__ == "__main__":
    measure(sys.argv[1:])
