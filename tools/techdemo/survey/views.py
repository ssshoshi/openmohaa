"""Viewpoints for a survey sweep of the retail maps (views.json): in each
map, the AI path nodes clustered into 12 groups, the node nearest each
group's middle, the eye 56 over it, facing the way with the longest clear
sight (traced through the map's brushes and terrain, bsp.py), so a shot
shows a street or a yard rather than the wall beside it."""
import os, zipfile, re, math, json, random, sys
from bsp import BSP
z = zipfile.ZipFile(os.path.join(os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor"), "main", "Pak5.pk3"))
res = {}
for name in sorted(n for n in z.namelist() if re.match(r"maps/(m\d|dm/|obj/).*\.bsp$", n, re.I)):
    if len(sys.argv) > 1 and not any(a in name for a in sys.argv[1:]): continue
    b = BSP(z.read(name))
    pts = [tuple(map(float, e["origin"].split())) for e in b.ents
           if e.get("classname") in ("info_pathnode", "info_player_start", "info_player_deathmatch", "info_player_allied", "info_player_axis") and "origin" in e]
    pts = [p for p in pts if not b.solid((p[0], p[1], p[2] + 56))]
    random.seed(1); K = min(12, len(pts))
    cen = random.sample(pts, K)
    for _ in range(12):
        groups = [[] for _ in cen]
        for p in pts:
            groups[min(range(K), key=lambda k: (p[0]-cen[k][0])**2 + (p[1]-cen[k][1])**2 + ((p[2]-cen[k][2])*2)**2)].append(p)
        cen = [tuple(sum(c)/len(g) for c in zip(*g)) if g else cen[k] for k, g in enumerate(groups)]
    views = []
    for k, c in enumerate(cen):
        g = groups[k] or pts
        p = min(g, key=lambda q: (q[0]-c[0])**2 + (q[1]-c[1])**2 + (q[2]-c[2])**2)
        eye = (p[0], p[1], p[2] + 56)
        ds = [min(b.depth(eye, a * 15), 2500) for a in range(24)]
        score = lambda i: sum(ds[(i + o) % 24] for o in (-2, -1, 0, 1, 2)) + ds[i]
        i = max(range(24), key=score)
        views.append([round(eye[0]), round(eye[1]), round(eye[2]), 3, i * 15, round(ds[i])])
    m = re.sub(r"^maps/|\.bsp$", "", name)
    res[m] = views
    print(m, len(pts), [v[5] for v in views], flush=True)
order = sorted(res, key=lambda k: ("/" in k, k))
json.dump({k: res[k] for k in order}, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "views.json"), "w"), indent=0)
