"""Cracks in a generated map's ground: every ground prism's top triangle
from the .map, and any vertex lying on another triangle's edge (in plan)
but off it in height. 0 is what a build wants.

    python3 tools/techdemo/survey/cracks.py build/techdemo/m0_techdemo.map"""
import re, sys, collections, math
mp = sys.argv[1]
tris = []
brush = []; depth = 0
num = r"(-?[\d.e+-]+)"
pt = re.compile(r"\(\s*" + r"\s+".join([num]*3) + r"\s*\)")
for line in open(mp):
    s = line.strip()
    if s.startswith("( ") and not s.startswith("( (") and s.count(")") >= 3:
        ps = [tuple(map(float, m)) for m in pt.findall(s)[:3]]
        tex = s.split(")")[3].split()[0] if len(s.split(")")) > 3 else ""
        brush.append((ps, tex))
    elif s == "}":
        if len(brush) == 5:
            tops = [b for b in brush if len({round(p[2], 2) for p in b[0]}) > 1 and len({(round(p[0], 2), round(p[1], 2)) for p in b[0]}) == 3]
            if len(tops) == 1 and "caulk" not in tops[0][1]:
                tris.append(tops[0])
        brush = []
print(len(tris), "ground tris")
grid = collections.defaultdict(list)
for k, (ps, t) in enumerate(tris):
    for p in ps: grid[(int(p[0] // 64), int(p[1] // 64))].append((p, k))
bad = []
for k, (ps, t) in enumerate(tris):
    for a, b in ((ps[0], ps[1]), (ps[1], ps[2]), (ps[2], ps[0])):
        L = math.hypot(b[0]-a[0], b[1]-a[1])
        if L < 1: continue
        cells = set()
        for s in range(int(L // 32) + 2):
            f = min(1, s * 32 / L); x, y = a[0] + (b[0]-a[0])*f, a[1] + (b[1]-a[1])*f
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1): cells.add((int(x // 64) + dx, int(y // 64) + dy))
        for c in cells:
            for p, j in grid[c]:
                if j == k: continue
                f = ((p[0]-a[0])*(b[0]-a[0]) + (p[1]-a[1])*(b[1]-a[1])) / (L*L)
                if f <= 1e-4 or f >= 1 - 1e-4: continue
                d = abs((p[0]-a[0])*(b[1]-a[1]) - (p[1]-a[1])*(b[0]-a[0])) / L
                if d > 0.05: continue
                z = a[2] + (b[2]-a[2])*f
                if abs(z - p[2]) > 0.1: bad.append((round(abs(z-p[2]), 2), p, t))
bad = sorted(set(bad), reverse=True)
print(len(bad), "vertices off the edge they lie on")
for b in bad[:25]: print(b)
