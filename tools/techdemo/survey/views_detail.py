"""Fixed viewpoints on what a pass of work changed (views_detail.json), the
eye 56 over the floor the built BSP has there, for a sweep beside the
zone views. Each: name, x, y, yaw, pitch."""
import os, zipfile, json, sys
from bsp import BSP
HERE = os.path.dirname(os.path.abspath(__file__))
pk = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "..", "build", "techdemo", "zzzzzzzzz-opm-techdemo.pk3")
b = BSP(zipfile.ZipFile(pk).read("maps/m0_techdemo.bsp"))
SPOTS = [
    ("norway yard from the gate", -6000, 2320, 180, 3),
    ("norway yard, the north block", -6700, 2100, 90, 3),
    ("norway yard, the south block", -6600, 2500, 270, 3),
    ("norway road, wire poles", -4684, 2720, 211, 3),
    ("bocage spire from the lane", -5084, -2100, 193, -4),
    ("town, the shelled house's rubble", 5500, -2550, 40, 5),
    ("town, past the pass's ridge ends", 4622, -2960, 135, 3),
    ("beach, a crater on the bluff", 4300, -150, 290, 8),
]
def floor(x, y):
    z = 1400
    while z > -600:
        z -= 8
        if b.solid((x, y, z)): return z + 8
    return 0
views = [[x, y, floor(x, y) + 56, p, yaw, 0] for _, x, y, yaw, p in SPOTS]
json.dump({"td_detail": views}, open(os.path.join(HERE, "views_detail.json"), "w"), indent=0)
for s, v in zip(SPOTS, views): print(s[0], v)
