"""What the retail maps are built of, measured (grammar.txt): which wall
texture stands on which (a base course, a joist band between storeys) and
how tall the lower one is; which static models stand within 160 units of
each other (what goes with what); how far each model's nearest neighbour is
(how dense trees, lamps, furniture are)."""
import zipfile, re, math, json, struct, collections
from bsp import BSP
import os
z = zipfile.ZipFile(os.path.join(os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor"), "main", "Pak5.pk3"))
stack = collections.Counter(); stack_h = collections.defaultdict(list)
wall_h = collections.defaultdict(list)
pairs = collections.Counter(); model_n = collections.Counter()
spacing = collections.defaultdict(list)
per_map = {}
for name in sorted(n for n in z.namelist() if re.match(r"maps/(m\d|dm/|obj/).*\.bsp$", n, re.I)):
    b = BSP(z.read(name)); d = b.d
    vo, vl = b.L(4)
    so, sl = b.L(3); surfs = [struct.unpack_from("<12i", d, so + k * 108) for k in range(sl // 108)]
    faces = []
    for f in surfs:
        sh, st, fv, nv = f[0], f[2], f[3], f[4]
        if st != 1 or nv < 3: continue
        s = b.shaders[sh][0].lower()
        if any(k in s for k in ("caulk", "nodraw", "sky", "clip", "trigger", "common/")): continue
        P = [struct.unpack_from("<3f", d, vo + (fv + k) * 44) for k in range(nv)]
        n = struct.unpack_from("<3f", d, vo + fv * 44 + 28)
        if abs(n[2]) > 0.05: continue  # vertical only
        zs = [p[2] for p in P]
        # horizontal extent along the face
        ax = (-n[1], n[0])
        us = [p[0]*ax[0] + p[1]*ax[1] for p in P]
        dist = P[0][0]*n[0] + P[0][1]*n[1]
        faces.append((s, round(n[0], 2), round(n[1], 2), dist, min(us), max(us), min(zs), max(zs)))
    # stacked faces: same plane, a's bottom == b's top
    byplane = collections.defaultdict(list)
    for f in faces: byplane[(f[1], f[2], round(f[3]))].append(f)
    for fl in byplane.values():
        for a in fl:
            for c in fl:
                if a is c or a[0] == c[0]: continue
                if abs(a[6] - c[7]) < 1 and min(a[5], c[5]) - max(a[4], c[4]) > 16:
                    stack[(a[0], c[0])] += 1; stack_h[(a[0], c[0])].append(round(c[7]-c[6]))
    for f in faces: wall_h[f[0]].append(f[7]-f[6])
    # static models: neighbours
    mo, ml = b.L(25); ms = []
    for i in range(ml // 164):
        m = struct.unpack_from("<128s", d, mo + i * 164)[0].split(b"\0")[0].decode().lower()
        org = struct.unpack_from("<3f", d, mo + i * 164 + 128)
        m = re.sub(r"^models/|\.tik$", "", m); ms.append((m, org)); model_n[m] += 1
    for i, (m, o) in enumerate(ms):
        best = None
        for j, (m2, o2) in enumerate(ms):
            if i == j: continue
            dd = math.dist(o, o2)
            if dd < 160 and m2 != m: pairs[tuple(sorted((m, m2)))] += 1
            if best is None or dd < best: best = dd
        if best is not None: spacing[m].append(best)
    per_map[name] = len(ms)
def med(v): v = sorted(v); return v[len(v)//2] if v else 0
out = []
out.append("== stacked (upper on lower): count, median lower height")
for (a, c), k in stack.most_common(80): out.append(f"{k:5d} {a}  ON  {c}  h={med(stack_h[(a,c)])}")
out.append("== static model pairs within 160u (top 80)")
for (a, c), k in pairs.most_common(80): out.append(f"{k:5d} {a} + {c}")
out.append("== nearest neighbour spacing median per model (top 60 models)")
for m, k in model_n.most_common(60): out.append(f"{k:5d} {m} nn={med(spacing[m]):.0f}")
open("grammar.txt", "w").write("\n".join(out))
print("\n".join(out[:40]))
