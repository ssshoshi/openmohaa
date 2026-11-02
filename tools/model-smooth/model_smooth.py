#!/usr/bin/env python3
"""model_smooth: smoother MOH:AA skeletal meshes (.skd), by one level of Loop
subdivision, with the skinning exact.

After the RTCW Castle Operations remaster's tools/model-smooth (for RTCW's
.mds). MOH:AA's characters are a few hundred triangles a body part, and their
silhouettes are polygons close up; one Loop level quarters every triangle
and moves the vertexes onto a smooth limit surface.

Skinning stays exact. A vertex is skinned as the sum over its bones of
weight * (bone matrix * offset), which is linear in each bone's weight and
weight * offset. So each vertex is carried as those, per bone; Loop's masks
are linear combinations of vertexes, and a combination of them skins, in
every pose of every animation, to exactly that combination of the skinned
vertexes. The only change afterwards is dropping a new vertex's weakest
weights (below --min-weight, past the most the surface had) and scaling the
rest back to a sum of one.

The mesh is subdivided where it is one surface: vertexes at the same place
with the same weights and close normals are one (UV seams are not edges);
where normals differ more than --crease (a hard edge the artist split), or a
surface ends, the edge is a boundary and keeps its line. Small pieces apart
from the rest (insignia, buttons, under --decal-tris triangles) are only cut,
not smoothed, so they stay on what they sit on. Texture coordinates are not
smoothed, only cut. Morph targets (faces) are carried like positions.

Level of detail stays: the original vertexes keep their order and collapse
data, and the new ones go after them, each collapsing to an end of its edge
at the lowest detail threshold, so a distant model drops them first.

The engine (TIKI_MAX_VERTEXES 2048, TIKI_MAX_TRIANGLES 4096) takes a surface
of up to 2047 vertexes and 4096 triangles; a surface that would go past that
is left as it is.

Reads the game's paks (tools/paks.py, MOHAA_GAME), writes a pk3 for main/.
What it writes is made from the game's models: never commit or share it.

    python3 -I tools/model-smooth/model_smooth.py --list 'models/human/.*'
    python3 -I tools/model-smooth/model_smooth.py --roundtrip 'models/human/.*'
    python3 -I tools/model-smooth/model_smooth.py --smooth 'models/human/german.*' --pk3 OUT.pk3
"""

import argparse
import math
import os
import re
import struct
import sys
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import paks  # noqa: E402

MAX_VERTS = 2047
MAX_TRIS = 4096

HEADER = struct.Struct("<4si64siiiii10iiiii")  # up to ofsMorphTargets; v6 adds a float
SURF = struct.Struct("<4s64s8i")
VERT = struct.Struct("<3f2fii")
MORPH = struct.Struct("<i3f")
WEIGHT = struct.Struct("<if3f")


class Vertex:
    __slots__ = ("normal", "uv", "weights", "morphs")

    def __init__(self, normal, uv, weights, morphs):
        self.normal = normal  # (x, y, z)
        self.uv = uv  # (s, t)
        self.weights = weights  # [(bone, weight, (ox, oy, oz))]
        self.morphs = morphs  # [(index, (x, y, z))]


class Surface:
    def __init__(self):
        self.ident = b"SKL "
        self.name = b""
        self.static = 0
        self.tris = []
        self.verts = []
        self.collapse = []
        self.collapseIndex = []


class Skd:
    pass


def parse(data):
    skd = Skd()
    fields = HEADER.unpack_from(data, 0)
    skd.ident, skd.version = fields[0], fields[1]
    if skd.ident != b"SKMD" or skd.version not in (5, 6):
        raise ValueError("not an SKD 5/6")
    skd.numSurfaces, skd.ofsSurfaces, skd.ofsEnd = fields[3], fields[6], fields[7]
    skd.data = data
    skd.surfaces = []
    ofs = skd.ofsSurfaces
    for _ in range(skd.numSurfaces):
        ident, name, ntri, nv, static, oT, oV, oC, oE, oCI = SURF.unpack_from(data, ofs)
        s = Surface()
        s.ident, s.name, s.static = ident, name, static
        s.ofs, s.size = ofs, oE
        s.layout = (oT, oV, oC, oCI)
        s.tris = [struct.unpack_from("<3i", data, ofs + oT + 12 * i) for i in range(ntri)]
        p = ofs + oV
        for _ in range(nv):
            nx, ny, nz, s0, t0, nw, nm = VERT.unpack_from(data, p)
            p += VERT.size
            morphs = []
            for _ in range(nm):
                mi, mx, my, mz = MORPH.unpack_from(data, p)
                p += MORPH.size
                morphs.append((mi, (mx, my, mz)))
            weights = []
            for _ in range(nw):
                b, w, ox, oy, oz = WEIGHT.unpack_from(data, p)
                p += WEIGHT.size
                weights.append((b, w, (ox, oy, oz)))
            s.verts.append(Vertex((nx, ny, nz), (s0, t0), weights, morphs))
        s.collapse = list(struct.unpack_from("<%di" % nv, data, ofs + oC))
        s.collapseIndex = list(struct.unpack_from("<%di" % nv, data, ofs + oCI))
        skd.surfaces.append(s)
        ofs += oE
    skd.surfacesEnd = ofs
    return skd


def write_surface(s):
    """A surface in the order the game's files have it: header, triangles,
    vertexes, collapse, collapse index."""
    tris = b"".join(struct.pack("<3i", *t) for t in s.tris)
    verts = []
    for v in s.verts:
        verts.append(VERT.pack(*v.normal, *v.uv, len(v.weights), len(v.morphs)))
        for mi, m in v.morphs:
            verts.append(MORPH.pack(mi, *m))
        for b, w, o in v.weights:
            verts.append(WEIGHT.pack(b, w, *o))
    verts = b"".join(verts)
    coll = struct.pack("<%di" % len(s.verts), *s.collapse)
    colli = struct.pack("<%di" % len(s.verts), *s.collapseIndex)
    oT = SURF.size
    oV = oT + len(tris)
    oC = oV + len(verts)
    oCI = oC + len(coll)
    oE = oCI + len(colli)
    head = SURF.pack(s.ident, s.name, len(s.tris), len(s.verts), s.static, oT, oV, oC, oE, oCI)
    return head + tris + verts + coll + colli


def write(skd):
    """The file with its surfaces block rebuilt and every offset into what
    follows it moved by the change in size."""
    data = skd.data
    block = b"".join(write_surface(s) for s in skd.surfaces)
    delta = len(block) - (skd.surfacesEnd - skd.ofsSurfaces)
    head = bytearray(data[: skd.ofsSurfaces])
    fields = list(HEADER.unpack_from(data, 0))
    # ofsBones (5), ofsSurfaces (6), ofsEnd (7), ofsBoxes (19), ofsMorphTargets (21)
    for i in (5, 7, 19, 21):
        if fields[i] > skd.ofsSurfaces:
            fields[i] += delta
    HEADER.pack_into(head, 0, *fields)
    return bytes(head) + block + data[skd.surfacesEnd:]


def roundtrip_ok(data):
    try:
        return write(parse(data)) == data
    except Exception:
        return False


# --- skinning-space arithmetic: a vertex as {bone: [w, w*ox, w*oy, w*oz]}


def skin_of(v):
    out = {}
    for b, w, o in v.weights:
        e = out.setdefault(b, [0.0, 0.0, 0.0, 0.0])
        e[0] += w
        e[1] += w * o[0]
        e[2] += w * o[1]
        e[3] += w * o[2]
    return out


def mix(terms):
    """sum of a * x for (a, x) where x are dicts of lists"""
    out = {}
    for a, x in terms:
        for k, val in x.items():
            e = out.setdefault(k, [0.0] * len(val))
            for i, c in enumerate(val):
                e[i] += a * c
    return out


def weights_of(skin, max_weights, min_weight):
    items = [(b, e[0], e) for b, e in skin.items() if e[0] > 1e-6]
    items.sort(key=lambda x: -x[1])
    kept = [x for x in items if x[1] >= min_weight][:max_weights] or items[:1]
    total = sum(x[1] for x in kept)
    out = []
    for b, w, e in kept:
        o = (e[1] / w, e[2] / w, e[3] / w)  # the offset is exact for this bone
        out.append((b, w / total, o))
    out.sort(key=lambda x: x[0])
    return out


def morphs_of(v):
    return {mi: list(m) for mi, m in v.morphs}


def normalize(n):
    l = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
    return (n[0] / l, n[1] / l, n[2] / l) if l > 1e-9 else (0.0, 0.0, 1.0)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


# --- one Loop level


def subdivide(s, crease_cos, decal_tris, min_weight, scheme="butterfly"):
    n = len(s.verts)
    tris = s.tris
    if n < 3 or not tris:
        return None

    # weld: the same weights and offsets, close normals
    topo = [0] * n
    groups = {}
    reps = []  # topo vertex -> original vertex standing for it
    for i, v in enumerate(s.verts):
        key = tuple((b, round(w, 5), tuple(round(c, 3) for c in o)) for b, w, o in sorted(v.weights))
        found = -1
        for t in groups.get(key, []):
            if dot(s.verts[reps[t]].normal, v.normal) >= crease_cos:
                found = t
                break
        if found < 0:
            found = len(reps)
            reps.append(i)
            groups.setdefault(key, []).append(found)
        topo[i] = found
    nt = len(reps)

    # edges between welded vertexes, and the triangles' third vertexes
    edges = {}
    for ti, (a, b, c) in enumerate(tris):
        for x, y, z in ((a, b, c), (b, c, a), (c, a, b)):
            tx, ty = topo[x], topo[y]
            if tx == ty:
                continue
            key = (min(tx, ty), max(tx, ty))
            edges.setdefault(key, []).append(topo[z])

    neighbours = [set() for _ in range(nt)]
    boundary = [set() for _ in range(nt)]
    for (a, b), opp in edges.items():
        neighbours[a].add(b)
        neighbours[b].add(a)
        if len(opp) != 2:
            boundary[a].add(b)
            boundary[b].add(a)

    # pieces apart from the rest, and how big they are
    comp = list(range(nt))

    def find(x):
        while comp[x] != x:
            comp[x] = comp[comp[x]]
            x = comp[x]
        return x

    for a, b in edges:
        ra, rb = find(a), find(b)
        if ra != rb:
            comp[ra] = rb
    size = {}
    for a, b, c in tris:
        r = find(topo[a])
        size[r] = size.get(r, 0) + 1
    flat = [size.get(find(t), 0) < decal_tris for t in range(nt)]

    skins = [skin_of(s.verts[reps[t]]) for t in range(nt)]
    morphs = [morphs_of(s.verts[reps[t]]) for t in range(nt)]
    normals = [s.verts[reps[t]].normal for t in range(nt)]

    # the vertexes moved (Loop); the butterfly scheme leaves them where they are
    new_skin, new_morph, new_normal = [], [], []
    for t in range(nt):
        if flat[t] or scheme == "butterfly":
            terms = [(1.0, t)]
        elif boundary[t]:
            if len(boundary[t]) == 2:
                b0, b1 = boundary[t]
                terms = [(0.75, t), (0.125, b0), (0.125, b1)]
            else:
                terms = [(1.0, t)]  # a corner of the boundary keeps its place
        else:
            k = len(neighbours[t])
            if k < 3:
                terms = [(1.0, t)]
            else:
                beta = (1.0 / k) * (0.625 - (0.375 + 0.25 * math.cos(2 * math.pi / k)) ** 2)
                terms = [(1.0 - k * beta, t)] + [(beta, u) for u in neighbours[t]]
        new_skin.append(mix([(a, skins[u]) for a, u in terms]))
        new_morph.append(mix([(a, morphs[u]) for a, u in terms]))
        new_normal.append(normalize(tuple(sum(a * normals[u][i] for a, u in terms) for i in range(3))))

    # the edges' new vertexes, per welded edge
    def wing(x, y, z):
        # the vertex across edge x-y from z, or None
        o = edges.get((min(x, y), max(x, y)), [])
        if len(o) != 2:
            return None
        return o[1] if o[0] == z else o[0] if o[1] == z else None

    edge_skin, edge_morph, edge_normal = {}, {}, {}
    for key, opp in edges.items():
        a, b = key
        if flat[a] or len(opp) != 2:
            terms = [(0.5, a), (0.5, b)]
        elif scheme == "butterfly":
            # interpolating: the original vertexes stay, the new one sits on
            # the curve through its neighbours (Dyn, Levin, Gregory 1990)
            c, d = opp
            wings = [wing(a, c, b), wing(b, c, a), wing(a, d, b), wing(b, d, a)]
            if None in wings:
                terms = [(0.5, a), (0.5, b)]
            else:
                terms = [(0.5, a), (0.5, b), (0.125, c), (0.125, d)] + [(-0.0625, w) for w in wings]
        else:
            terms = [(0.375, a), (0.375, b), (0.125, opp[0]), (0.125, opp[1])]
        edge_skin[key] = mix([(w, skins[u]) for w, u in terms])
        edge_morph[key] = mix([(w, morphs[u]) for w, u in terms])
        edge_normal[key] = normalize(tuple(sum(w * normals[u][i] for w, u in terms) for i in range(3)))

    max_weights = max(len(v.weights) for v in s.verts)
    out = Surface()
    out.ident, out.name, out.static = s.ident, s.name, s.static

    for i, v in enumerate(s.verts):
        t = topo[i]
        w = weights_of(new_skin[t], max_weights, min_weight)
        m = sorted((mi, tuple(val)) for mi, val in new_morph[t].items())
        out.verts.append(Vertex(new_normal[t], v.uv, w, m))
    out.collapse = list(s.collapse)
    out.collapseIndex = list(s.collapseIndex)
    lowest = min(s.collapseIndex) if s.collapseIndex else 0

    # one new vertex per original edge (texture coordinates differ across a
    # seam, so a welded edge can have two)
    mid = {}

    def midpoint(x, y):
        key = (min(x, y), max(x, y))
        if key in mid:
            return mid[key]
        tx, ty = topo[x], topo[y]
        tkey = (min(tx, ty), max(tx, ty))
        if tx == ty or tkey not in edge_skin:
            sk = mix([(0.5, skins[tx]), (0.5, skins[ty])])
            mo = mix([(0.5, morphs[tx]), (0.5, morphs[ty])])
            nr = normalize(tuple(normals[tx][i] + normals[ty][i] for i in range(3)))
        else:
            sk, mo, nr = edge_skin[tkey], edge_morph[tkey], edge_normal[tkey]
        uv = ((s.verts[x].uv[0] + s.verts[y].uv[0]) * 0.5, (s.verts[x].uv[1] + s.verts[y].uv[1]) * 0.5)
        idx = len(out.verts)
        out.verts.append(Vertex(nr, uv, weights_of(sk, max_weights, min_weight), sorted((mi, tuple(val)) for mi, val in mo.items())))
        # it goes before either end does: to whichever end stays longer
        keep = x if s.collapseIndex[x] >= s.collapseIndex[y] else y
        out.collapse.append(keep)
        out.collapseIndex.append(lowest)
        mid[key] = idx
        return idx

    for a, b, c in tris:
        ab, bc, ca = midpoint(a, b), midpoint(b, c), midpoint(c, a)
        out.tris += [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]

    if len(out.verts) > MAX_VERTS or len(out.tris) > MAX_TRIS:
        return None
    return out


def winners(pattern):
    rx = re.compile(pattern, re.I)
    win = {}
    for name, files in paks.index().items():
        for f in files:
            if f.lower().endswith(".skd") and rx.search(f):
                win[f.lower()] = (f, name)
    return sorted(win.values())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", metavar="REGEX", help="the models and their surfaces' sizes")
    ap.add_argument("--roundtrip", metavar="REGEX", help="parse and write each back: must be byte identical")
    ap.add_argument("--smooth", metavar="REGEX", help="subdivide the models matching")
    ap.add_argument("--pk3", default="zzzzzzzzz-opm-smooth.pk3", help="where to write them")
    ap.add_argument("--crease", type=float, default=60.0, help="degrees between normals past which an edge stays a hard edge")
    ap.add_argument("--decal-tris", type=int, default=24, help="pieces apart from the rest under this are cut, not smoothed")
    ap.add_argument("--min-weight", type=float, default=0.01, help="weights under this are dropped from new vertexes")
    ap.add_argument("--loop", metavar="REGEX", help="models matching this use the loop scheme whatever --scheme says "
                    "(closed, smooth shapes nothing is laid over, such as helmets)")
    ap.add_argument("--scheme", choices=("butterfly", "loop"), default="butterfly",
                    help="butterfly (default) keeps every original vertex where it was and only places the new ones on a smooth "
                    "curve, so pieces laid over others (straps, a pack on a shirt) stay where they were drawn and textures stay "
                    "pinned; loop moves every vertex onto the limit surface: smoother, but overlays can sink into what is under them")
    args = ap.parse_args()

    if args.list:
        for path, pak in winners(args.list):
            try:
                skd = parse(paks.read(pak, path))
            except ValueError as e:
                print(f"{path}: {e}")
                continue
            sizes = ", ".join(f"{s.name.split(b'\0')[0].decode('latin-1')} {len(s.verts)}v/{len(s.tris)}t" for s in skd.surfaces)
            print(f"{path} (v{skd.version}): {sizes}")
        return

    if args.roundtrip:
        good = bad = 0
        for path, pak in winners(args.roundtrip):
            if roundtrip_ok(paks.read(pak, path)):
                good += 1
            else:
                bad += 1
                print(f"differs: {path}")
        print(f"{good} byte identical, {bad} not")
        sys.exit(1 if bad else 0)

    if args.smooth:
        crease_cos = math.cos(math.radians(args.crease))
        z = zipfile.ZipFile(args.pk3, "w", zipfile.ZIP_DEFLATED)
        total = done = 0
        for path, pak in winners(args.smooth):
            data = paks.read(pak, path)
            try:
                skd = parse(data)
            except ValueError as e:
                print(f"{path}: skipped ({e})")
                continue
            before = sum(len(s.tris) for s in skd.surfaces)
            changed = 0
            scheme = "loop" if args.loop and re.search(args.loop, path, re.I) else args.scheme
            for i, s in enumerate(skd.surfaces):
                out = subdivide(s, crease_cos, args.decal_tris, args.min_weight, scheme)
                if out:
                    skd.surfaces[i] = out
                    changed += 1
            after = sum(len(s.tris) for s in skd.surfaces)
            total += 1
            if changed:
                done += 1
                z.writestr(path, write(skd))
            print(f"{path}: {changed}/{len(skd.surfaces)} surfaces, {before} -> {after} triangles ({scheme})")
        z.close()
        print(f"{done} of {total} models written to {args.pk3}")
        return

    ap.print_help()


if __name__ == "__main__":
    main()
