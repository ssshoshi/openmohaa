#!/usr/bin/env python3
"""A lamp's turned parts made round: shades, caps and sockets that are polygons.

A shade the game made with eight sides is a surface of revolution drawn as an
octagon. Smoothing it (Loop, Catmull-Clark) pulls its open rim in and leaves it
a polygon; this puts it back on the solid it stands for:

  1. the surface is cut (each triangle into four, without moving anything)
     until its widest piece has about --sides sides (at least 3/4 of them);
  2. each piece of it (vertexes at the same place are one) whose vertexes sit
     in rings round one axis, each ring a regular polygon, is turned: its
     profile is the radius of each ring at its height, and every vertex lying
     on the polygon's faces goes out onto the circle of the profile at its
     height. Normals that faced out from the axis face out from the solid;
     the others (a flat cap's) stay as they were. Texture coordinates are cut,
     not moved, so the texture stays where it was.

Pieces that are not turned (a plank, a hook) are only cut. Only a surface on
one bone is turned (a lamp's are), in that bone's frame.

    tools/lamps/lathe.py IN.skd OUT.skd --surface bulbcover
    tools/lamps/lathe.py IN.skd --list --surface bulbcover   # the pieces it would turn
    tools/lamps/remodel.py does this for a lamp whose lamps.txt entry has `lathe SURFACE...`
"""

import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "model-smooth"))
import model_smooth as ms  # noqa: E402

RING_TOL = 0.02     # of the piece's size: vertexes this close in height are one ring
ROUND_TOL = 0.12    # a ring is a regular polygon when its corners are within this of one radius
ON_FACE = 0.98      # a vertex this close to the polygon (the original's sides) lies on a side


def _pos(v):
    return v.weights[0][2]


def _surface_name(s):
    return s.name.split(b"\0")[0].decode("latin-1")


def _pieces(s):
    """Lists of vertex indexes, one per connected piece (vertexes at one place are one)."""
    parent = list(range(len(s.verts)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    at = {}
    for i, v in enumerate(s.verts):
        key = tuple(round(c, 3) for c in _pos(v))
        if key in at:
            parent[find(i)] = find(at[key])
        else:
            at[key] = i
    for a, b, c in s.tris:
        parent[find(a)] = find(b)
        parent[find(b)] = find(c)
    out = {}
    for i in range(len(s.verts)):
        out.setdefault(find(i), []).append(i)
    return list(out.values())


def _rings(points, axis):
    """[(height, [points])] in height order: the points grouped by height along axis."""
    hs = sorted(points, key=lambda p: p[axis])
    size = max(max(p[k] for p in points) - min(p[k] for p in points) for k in range(3))
    tol = RING_TOL * size
    rings = []
    for p in hs:
        if rings and p[axis] - rings[-1][0] <= tol:
            rings[-1][1].append(p)
        else:
            rings.append([p[axis], [p]])
    return [(sum(q[axis] for q in r) / len(r), r) for _, r in rings]


def _profile(points, axis):
    """(centre (u, v), [(height, radius)]) when the points are rings round axis,
    each a regular polygon of at least 5 corners (or a single point on it), else None."""
    u, w = [k for k in range(3) if k != axis]
    rings = _rings(points, axis)
    wide = max(rings, key=lambda r: max(abs(p[u]) + abs(p[w]) for p in r[1]))[1]
    corners = {(round(p[u], 3), round(p[w], 3)) for p in wide}
    if len(corners) < 5:
        return None
    cu = sum(c[0] for c in corners) / len(corners)
    cw = sum(c[1] for c in corners) / len(corners)
    prof = []
    for h, ring in rings:
        rs = sorted({round(math.hypot(p[u] - cu, p[w] - cw), 3) for p in ring})
        r = rs[-1]
        if r < 1e-3:
            prof.append((h, 0.0))
            continue
        # the ring's corners: those at its radius; a regular polygon's are all there
        outer = {(round(p[u], 3), round(p[w], 3)) for p in ring if math.hypot(p[u] - cu, p[w] - cw) >= r * (1 - ROUND_TOL)}
        if len(outer) < 5:
            return None
        prof.append((h, r))
    if len(prof) < 2:
        return None
    return (cu, cw), prof


def _radius_at(prof, h):
    """The profile's radius at height h, and its slope dr/dh (None outside it)."""
    for (h0, r0), (h1, r1) in zip(prof, prof[1:]):
        if h0 - 1e-4 <= h <= h1 + 1e-4:
            if h1 - h0 < 1e-6:
                return max(r0, r1), 0.0
            t = (h - h0) / (h1 - h0)
            return r0 + (r1 - r0) * t, (r1 - r0) / (h1 - h0)
    return None, None


def _sides(points, axis, centre):
    """(corners, the angle of one) on the piece's widest ring: the polygon its rings are."""
    u, w = [k for k in range(3) if k != axis]
    rings = _rings(points, axis)
    best = (0, 0.0, 0.0)
    for _, ring in rings:
        rs = [math.hypot(p[u] - centre[0], p[w] - centre[1]) for p in ring]
        r = max(rs)
        corners = {(round(p[u], 3), round(p[w], 3)) for p, rr in zip(ring, rs) if rr >= r * (1 - ROUND_TOL)}
        if r > best[2]:
            c = next(iter(corners))
            best = (len(corners), math.atan2(c[1] - centre[1], c[0] - centre[0]), r)
    return best[:2]


def turned_pieces(s):
    """[(vertex indexes, axis, centre, profile, (sides, a corner's angle))] for each piece of s that is turned."""
    out = []
    for piece in _pieces(s):
        points = list({tuple(round(c, 4) for c in _pos(s.verts[i])) for i in piece})
        if len(points) < 10:
            continue
        found = None
        for axis in range(3):
            got = _profile(points, axis)
            if got and (found is None or len(got[1]) < len(found[2])):
                found = (axis,) + got
        if found:
            axis, centre, prof = found
            out.append((piece, axis, centre, prof, _sides(points, axis, centre)))
    return out


def lathe_surface(s, sides=32, min_weight=0.01):
    """s with its turned pieces round (a new Surface), or None when it has none."""
    if any(len(v.weights) != 1 or v.weights[0][0] != s.verts[0].weights[0][0] for v in s.verts):
        return None  # turned in one bone's frame: a lamp's surface is on one bone
    found = turned_pieces(s)
    if not found:
        return None
    levels = 0
    n = max(found, key=lambda f: max(r for _, r in f[3]))[4][0]  # the widest piece's sides decide
    while n * (2 ** levels) < sides * 0.75:  # 24 of a 6-sided socket will do
        levels += 1
    levels = max(levels, 1)
    # each turned piece known by where its original vertexes are (a cut keeps them)
    key = lambda p: tuple(round(c, 3) for c in p)  # noqa: E731
    owner = {}
    for k, (piece, _a, _c, _p, _n) in enumerate(found):
        for i in piece:
            owner[key(_pos(s.verts[i]))] = k
    for _ in range(levels):
        cut = ms.subdivide(s, 1.0, 1 << 30, min_weight)  # every piece "a decal": cut, not smoothed
        if cut is None:
            break
        s = cut
    if len(s.verts) > ms.MAX_VERTS or len(s.tris) > ms.MAX_TRIS:
        return None
    # the cut surface's pieces, each the turned one it holds vertexes of: a part that only
    # passes through another (a socket through a cap) is not moved by its profile
    which = {}
    for piece in _pieces(s):
        ks = {owner.get(key(_pos(s.verts[i]))) for i in piece} - {None}
        if len(ks) == 1:
            k = ks.pop()
            for i in piece:
                which[i] = k
    moved = 0
    for vi, v in enumerate(s.verts):
        if vi not in which:
            continue
        p = _pos(v)
        for axis, (cu, cw), prof, (corners, phase) in [found[which[vi]][1:5]]:
            u, w = [k for k in range(3) if k != axis]
            if not (prof[0][0] - 1e-3 <= p[axis] <= prof[-1][0] + 1e-3):
                continue
            want, slope = _radius_at(prof, p[axis])
            r = math.hypot(p[u] - cu, p[w] - cw)
            if want is None or want < 1e-3:
                continue
            a = math.atan2(p[w] - cw, p[u] - cu)
            # on a side when on the polygon at its height, not inside it (a cap's middle)
            step = 2 * math.pi / corners
            off = (a - phase) % step - step / 2
            if r < ON_FACE * want * math.cos(step / 2) / math.cos(off):
                continue
            q = list(p)
            q[u], q[w] = cu + math.cos(a) * want, cw + math.sin(a) * want
            b, wt, _ = v.weights[0]
            v.weights = [(b, wt, tuple(q))]
            # the side's normal: out from the axis, tilted by the profile's slope
            h = 1.0 / math.hypot(1.0, slope)
            n = [0.0, 0.0, 0.0]
            n[u], n[w], n[axis] = math.cos(a) * h, math.sin(a) * h, -slope * h
            d = sum(n[k] * v.normal[k] for k in range(3))
            if abs(d) > 0.5:  # it faced out from (or into) the side: a cap's normal stays
                v.normal = tuple(c if d > 0 else -c for c in n)
            moved += 1
            break
    return s if moved else None


def lathe_skd(data, surfaces, sides=32):
    """The .skd bytes with the named surfaces' turned pieces round; (bytes, report)."""
    skd = ms.parse(data)
    report = {}
    for i, s in enumerate(skd.surfaces):
        name = _surface_name(s)
        if name.lower() not in [x.lower() for x in surfaces]:
            continue
        out = lathe_surface(s, sides)
        if out is None:
            report[name] = "not turned"
            continue
        report[name] = "%d -> %d triangles" % (len(s.tris), len(out.tris))
        skd.surfaces[i] = out
    return ms.write(skd), report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--surface", action="append", required=True, help="a surface to turn (repeatable)")
    ap.add_argument("--sides", type=int, default=32, help="at least this many sides round")
    ap.add_argument("--list", action="store_true", help="only say which pieces are turned")
    args = ap.parse_args()
    if not args.list and not args.output:
        ap.error("an output .skd is needed (or --list)")
    data = open(args.input, "rb").read()
    if args.list:
        for s in ms.parse(data).surfaces:
            if _surface_name(s).lower() in [x.lower() for x in args.surface]:
                for piece, axis, centre, prof, sides in turned_pieces(s):
                    print("%s: %d vertexes, axis %s, %d sides, profile %s" % (
                        _surface_name(s), len(piece), "xyz"[axis], sides[0],
                        ", ".join("%.2f:%.2f" % hr for hr in prof)))
        return
    out, report = lathe_skd(data, args.surface, args.sides)
    for name, what in report.items():
        print("%s: %s" % (name, what))
    open(args.output, "wb").write(out)


if __name__ == "__main__":
    main()
