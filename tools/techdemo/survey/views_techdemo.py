"""Viewpoints for a sweep of m0_techdemo's biomes (views_techdemo.json), the
same way as views.py: no path nodes there, so ground found under 500 random
points in each zone, clustered into 10 groups, each facing its longest
clear sight. Reads the built pk3 (or the one given)."""
import os, zipfile, math, json, random, sys
from bsp import BSP
pk = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "build", "techdemo", "zzzzzzzzz-opm-techdemo.pk3")
b = BSP(zipfile.ZipFile(pk).read("maps/m0_techdemo.bsp"))
ROWS = (-3584, -1152, 1152, 3584); INNER, OUTER = 3584, 8064
names = {(-1, 2): "norway", (-1, 1): "ardennes", (-1, 0): "bocage", (1, 2): "desert", (1, 1): "beach", (1, 0): "town"}
def floor(x, y):
    z = 1400
    if b.solid((x, y, z)): return None
    while z > -600:
        z -= 8
        if b.solid((x, y, z)): return z + 8
    return None
res = {}
random.seed(2)
for (band, row), nm in names.items():
    xa, xb = sorted((band * (INNER + 300), band * (OUTER - 500)))
    pts = []
    for _ in range(500):
        x, y = random.uniform(xa, xb), random.uniform(ROWS[row] + 200, ROWS[row + 1] - 200)
        f = floor(x, y)
        if f is not None and not b.solid((x, y, f + 56)) and f > -60: pts.append((x, y, f))
    K = 10; cen = random.sample(pts, K)
    for _ in range(10):
        groups = [[] for _ in cen]
        for p in pts: groups[min(range(K), key=lambda k: (p[0]-cen[k][0])**2 + (p[1]-cen[k][1])**2)].append(p)
        cen = [tuple(sum(c)/len(g) for c in zip(*g)) if g else cen[k] for k, g in enumerate(groups)]
    views = []
    for k, c in enumerate(cen):
        p = min(groups[k] or pts, key=lambda q: (q[0]-c[0])**2 + (q[1]-c[1])**2)
        eye = (p[0], p[1], p[2] + 56)
        ds = [min(b.depth(eye, a * 15), 2500) for a in range(24)]
        i = max(range(24), key=lambda i: sum(ds[(i + o) % 24] for o in (-2, -1, 0, 1, 2)) + ds[i])
        views.append([round(eye[0]), round(eye[1]), round(eye[2]), 3, i * 15, round(ds[i])])
    res["td_" + nm] = views; print(nm, len(pts), [v[5] for v in views])
json.dump(res, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "views_techdemo.json"), "w"), indent=0)
