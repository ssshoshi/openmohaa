#!/usr/bin/env python3
"""Trees and bushes grown anew: a trunk, boughs and branches with bark, and sprays of leaves.

The game's trees are a trunk and a few crossed planes with a picture of a
crown on them. Each is grown again here as the kind of tree it stands for, to
the same height and spread, so it stands where the map put it and fills the
place the map's baked shadow and clip brushes expect:

    tools/flora/flora.py --out DIR            # every species: models, textures, shaders, tiks
    tools/flora/flora.py --out DIR oak        # one
    tools/flora/flora.py --list

DIR gets the files at their game paths (models/static/natural/opm_<name>.skd
and .skc, textures/opm/flora/*, scripts/opm_flora.shader, and
models/static/<game model>.tik for the models that become it).

How a tree is grown: a branch is a bent, tapering tube; along it stand its
children, turned round it by the golden angle, each a shorter, thinner branch
of the next level (trunk, boughs, branches). The last level carries the leaf
cards: bent quads showing one spray of the species' leaf texture
(tools/flora/paint.py: scanned leaves and photographed bark, CC0, downloaded once), lying along the branch, their normals turned partly
outward from the crown so it is lit as a rounded mass. The whole is then
fitted to the height and radius of the model it replaces. Seeded: the same
model every run.

Model space is the game's: z up, units as the map's. The one bone turns the
file's y up (as the game's own trees have it).
"""

import argparse
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "blender", "io_scene_mohaa"))
from mohaa import skc, skd  # noqa: E402

import paint  # noqa: E402

GOLDEN = math.radians(137.5)
UP = np.array([0.0, 0.0, 1.0])


class Level:
    def __init__(self, segs, sides, children, angle, ratio, rratio, start=0.3, gnarl=0.12, tropism=0.0,
                 tip=0.35, angle_var=0.15):
        self.segs = segs            # segments along a branch of this level
        self.sides = sides          # sides of its tube
        self.children = children    # branches of the next level standing on it
        self.angle = angle          # their angle from it, radians
        self.ratio = ratio          # their length, of its own
        self.rratio = rratio        # their radius, of its own where they stand
        self.start = start          # how far along it the first one stands
        self.gnarl = gnarl          # how crooked it grows
        self.tropism = tropism      # how hard it turns upward (down when negative)
        self.tip = tip              # its radius at the tip, of the base
        self.angle_var = angle_var


class Species:
    def __init__(self, **kw):
        self.__dict__.update(kw)


SPECIES = {
    # Pedunculate oak (Quercus robur), the tree of the Normandy bocage: a short stout bole
    # that parts into a few heavy crooked limbs, and a broad round crown as wide as it is tall.
    "oak": Species(
        replaces=["tree_oak"], bone="Object01", height=900.0, radius=400.0, trunk_len=0.34, trunk_radius=26.0,
        levels=[
            Level(6, 10, 7, math.radians(42), 1.05, 0.68, start=0.6, gnarl=0.06, tip=0.6),
            Level(6, 6, 7, math.radians(55), 0.50, 0.55, start=0.25, gnarl=0.16, tropism=0.12, tip=0.25),
            Level(4, 4, 0, math.radians(50), 0.5, 0.5, start=0.2, gnarl=0.20, tropism=0.04, tip=0.2),
        ],
        cards=15, card_size=76.0, card_droop=0.22, puff=0.75, flare=0.75,
        # scanned oak leaves (spring green: darkened to summer's), and furrowed oak bark
        leaf=dict(asset="LeafSet016", tint=(0.50, 0.62, 0.42), twig=(62, 48, 34), leaf_len=0.21, density=2),
        bark=dict(asset="jolcham_oak_bark_01", tint=(0.9, 0.88, 0.85)),
        bark_tile=(2.0, 120.0), seed=11,
    ),
}


def norm(v):
    n = np.linalg.norm(v)
    return v / n if n > 1e-9 else v


def perp(d):
    a = np.cross(d, UP)
    if np.linalg.norm(a) < 1e-3:
        a = np.cross(d, np.array([1.0, 0.0, 0.0]))
    return norm(a)


class Branch:
    def __init__(self, pts, rads, level):
        self.pts, self.rads, self.level = pts, rads, level

    def at(self, t):
        """(point, direction, radius) at t along it."""
        f = t * (len(self.pts) - 1)
        i = min(int(f), len(self.pts) - 2)
        u = f - i
        return (self.pts[i] * (1 - u) + self.pts[i + 1] * u, norm(self.pts[i + 1] - self.pts[i]),
                self.rads[i] * (1 - u) + self.rads[i + 1] * u)


def grow(sp, rng, start, dirn, length, radius, level, branches, cards):
    lv = sp.levels[level]
    pts, rads, d = [start], [radius], dirn
    for i in range(1, lv.segs + 1):
        t = i / lv.segs
        d = norm(d + rng.normal(0, lv.gnarl, 3) + UP * lv.tropism)
        pts.append(pts[-1] + d * (length / lv.segs))
        rads.append(radius * (1 - t * (1 - lv.tip)))
    br = Branch(pts, rads, level)
    branches.append(br)
    az = rng.uniform(0, 2 * math.pi)
    if level + 1 < len(sp.levels):
        for k in range(lv.children):
            t = lv.start + (1 - lv.start) * (k + rng.uniform(0.25, 0.75)) / lv.children
            p, pd, pr = br.at(t)
            az += GOLDEN + rng.normal(0, 0.3)
            side = perp(pd)
            side = side * math.cos(az) + np.cross(pd, side) * math.sin(az)
            ang = lv.angle + rng.normal(0, lv.angle_var)
            cd = norm(pd * math.cos(ang) + side * math.sin(ang))
            # a crown is fullest in its middle: the lowest and topmost children are shorter
            shape = 0.55 + 0.45 * math.sin(math.pi * min(1.0, (t - lv.start) / (1 - lv.start) * 0.85 + 0.15))
            grow(sp, rng, p - cd * pr * 0.5, cd, length * lv.ratio * shape * rng.uniform(0.85, 1.12),
                 min(pr * lv.rratio, pr * 0.92), level + 1, branches, cards)
        # and it runs on into a child at its tip
        p, pd, pr = br.at(1.0)
        grow(sp, rng, p, pd, length * (0.45 if level else 0.9), pr, level + 1, branches, cards)
    else:
        for k in range(sp.cards):
            t = 0.12 + 0.88 * (k + rng.uniform(0.2, 0.8)) / sp.cards
            p, pd, pr = br.at(t)
            az += GOLDEN + rng.normal(0, 0.4)
            side = perp(pd)
            side = side * math.cos(az) + np.cross(pd, side) * math.sin(az)
            ang = rng.uniform(0.35, 1.1) if k < sp.cards - 1 else rng.uniform(0.0, 0.3)
            cards.append((p, norm(pd * math.cos(ang) + side * math.sin(ang) + UP * rng.uniform(-0.25, 0.35)),
                          rng.uniform(0.7, 1.25), int(rng.integers(4)), rng.uniform(-1.2, 1.2)))


class Mesh:
    """Triangles in pieces the engine takes (1000 vertexes, 2000 triangles a surface)."""

    def __init__(self, name):
        self.name = name
        self.surfaces = [([], [])]

    def add(self, verts, tris):
        """verts: (position, normal, uv); tris index into them."""
        v, t = self.surfaces[-1]
        if len(v) + len(verts) > skd.MAX_VERTS or len(t) + len(tris) > skd.MAX_TRIS:
            self.surfaces.append(([], []))
            v, t = self.surfaces[-1]
        base = len(v)
        v.extend(verts)
        t.extend((a + base, b + base, c + base) for a, b, c in tris)

    def count(self):
        return sum(len(t) for _, t in self.surfaces)


def tube(mesh, br, sides, tile, flare=0.0):
    """A branch's bark: rings of vertexes along it, closed to a point at its tip."""
    n = len(br.pts)
    verts, tris = [], []
    side = perp(norm(br.pts[1] - br.pts[0]))
    along = 0.0
    for i in range(n):
        d = norm(br.pts[min(i + 1, n - 1)] - br.pts[max(i - 1, 0)])
        side = norm(side - d * np.dot(side, d))  # carried along, so the tube does not twist
        other = np.cross(d, side)
        if i:
            along += np.linalg.norm(br.pts[i] - br.pts[i - 1])
        for j in range(sides + 1):
            a = 2 * math.pi * j / sides
            out = side * math.cos(a) + other * math.sin(a)
            r = br.rads[i]
            if flare and i < 3:  # the roots: wider at the ground, in buttresses
                r *= 1 + flare * math.exp(-along / (br.rads[0] * 1.6)) * (1 + 0.35 * math.cos(a * 5 + 1.0))
            verts.append((br.pts[i] + out * r, out, (tile[0] * j / sides, -along / tile[1])))
    for i in range(n - 1):
        for j in range(sides):
            a, b = i * (sides + 1) + j, (i + 1) * (sides + 1) + j
            tris += [(a, a + 1, b), (a + 1, b + 1, b)]
    tip = len(verts)
    d = norm(br.pts[-1] - br.pts[-2])
    verts.append((br.pts[-1] + d * br.rads[-1] * 1.5, d, (tile[0] * 0.5, -(along + br.rads[-1]) / tile[1])))
    last = (n - 1) * (sides + 1)
    tris += [(last + j, last + j + 1, tip) for j in range(sides)]
    mesh.add(verts, tris)


def card(mesh, sp, p, d, scale, cell, roll, centre):
    """One spray of leaves: a quad along d from p, creased along its middle, its tip drooping."""
    s = sp.card_size * scale
    w = perp(d)
    up = np.cross(w, d)
    if up[2] < 0:
        w, up = -w, -up
    w, up = w * math.cos(roll) + up * math.sin(roll), up * math.cos(roll) - w * math.sin(roll)
    u0, v0 = (cell % 2) * 0.5, (cell // 2) * 0.5
    pad = 0.004
    verts = []
    for along, lift in ((0.0, 0.0), (1.0, -sp.card_droop)):
        for across, fold in ((-0.5, -0.12), (0.0, 0.0), (0.5, -0.12)):
            pos = p + d * (s * along) + w * (s * across) + up * (s * (lift + fold))
            n = norm(up + w * (across * 0.6))
            n = norm(n * (1 - sp.puff) + norm(pos - centre) * sp.puff)
            verts.append((pos, n, (u0 + pad + (0.5 - 2 * pad) * (across + 0.5), v0 + 0.5 - pad - (0.5 - 2 * pad) * along)))
    tris = [(0, 1, 3), (1, 4, 3), (1, 2, 4), (2, 5, 4)]
    mesh.add(verts, tris)


def build(name):
    sp = SPECIES[name]
    rng = np.random.default_rng(sp.seed)
    branches, cards = [], []
    grow(sp, rng, np.zeros(3), norm(np.array([rng.normal(0, 0.02), rng.normal(0, 0.02), 1.0])),
         sp.height * sp.trunk_len, sp.trunk_radius, 0, branches, cards)

    # fitted to the model it replaces: its height, and its radius about the trunk
    pts = np.array([p for b in branches for p in b.pts] + [c[0] + c[1] * sp.card_size * c[2] for c in cards])
    top = pts[:, 2].max()
    rad = np.percentile(np.hypot(pts[:, 0], pts[:, 1]), 97)
    fit = np.array([sp.radius / rad, sp.radius / rad, sp.height / top])
    for b in branches:
        b.pts = [p * fit for p in b.pts]
        if b.level:
            b.rads = [r * (fit[0] * fit[2]) ** 0.5 for r in b.rads]
    cards = [(c[0] * fit, norm(c[1] * fit), c[2], c[3], c[4]) for c in cards]
    centre = np.array([0.0, 0.0, sp.height * 0.58])

    bark, leaves = Mesh("bark"), Mesh("leaves")
    for b in branches:
        tube(bark, b, sp.levels[b.level].sides, sp.bark_tile, sp.flare if b.level == 0 else 0.0)
    for c in cards:
        card(leaves, sp, c[0], c[1], c[2], c[3], c[4], centre)
    return sp, bark, leaves


def to_skd(name, sp, meshes):
    m = skd.Model("opm_%s.skd" % name)
    m.bones = [skd.Bone(sp.bone)]
    lo, hi = np.full(3, 1e9), np.full(3, -1e9)
    for mesh in meshes:
        for verts, tris in mesh.surfaces:
            if not tris:
                continue
            s = skd.Surface(mesh.name)
            for pos, n, uv in verts:
                lo, hi = np.minimum(lo, pos), np.maximum(hi, pos)
                # the bone turns the file's y up: file (x, y, z) = model (y, z, x)
                s.verts.append(skd.Vertex((n[1], n[2], n[0]), uv, [(0, 1.0, (pos[1], pos[2], pos[0]))]))
            s.triangles = list(tris)
            m.surfaces.append(s)
    errors = skd.validate(m)
    if errors:
        raise SystemExit("%s: %s" % (name, "; ".join(errors)))
    a = skc.Animation()
    a.channels = [sp.bone + " pos", sp.bone + " rot"]
    a.values = [[(0.0, 0.0, 0.0, 0.0), (0.5, 0.5, 0.5, -0.5)]]
    a.frames = [skc.Frame((tuple(lo), tuple(hi)), float(max(np.linalg.norm(lo), np.linalg.norm(hi))))]
    return skd.write(m), skc.write(a), lo, hi


def shader_text(name, sp):
    return """opm_flora/%(n)s_bark
{
	qer_editorimage textures/opm/flora/%(n)s_bark.tga
	{
		map textures/opm/flora/%(n)s_bark.tga
		rgbGen vertex
	}
}
opm_flora/%(n)s_leaves
{
	qer_editorimage textures/opm/flora/%(n)s_leaves.tga
	cull none
	surfaceparm foliage
	{
		map textures/opm/flora/%(n)s_leaves.tga
		depthWrite
		alphaFunc GE128
		rgbGen vertex
	}
}
""" % {"n": name}


def tik_text(name, game_model):
    return """TIKI
setup
{
	scale 1
	path models/static/natural
	skelmodel opm_%(n)s.skd
	lod_scale 100
	surface bark shader opm_flora/%(n)s_bark
	surface leaves shader opm_flora/%(n)s_leaves
}

animations
{
	idle     opm_%(n)s.skc
}
""" % {"n": name}


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode("latin-1"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("species", nargs="*")
    ap.add_argument("--out")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--no-textures", action="store_true", help="models only (quicker while shaping one)")
    args = ap.parse_args()
    if args.list or not args.out:
        for n, sp in SPECIES.items():
            print("%-12s replaces %s" % (n, ", ".join(sp.replaces)))
        return
    shaders = ""
    for name in args.species or SPECIES:
        sp, bark, leaves = build(name)
        model, anim, lo, hi = to_skd(name, sp, [bark, leaves])
        write(os.path.join(args.out, "models/static/natural/opm_%s.skd" % name), model)
        write(os.path.join(args.out, "models/static/natural/opm_%s.skc" % name), anim)
        for game_model in sp.replaces:
            write(os.path.join(args.out, "models/static/%s.tik" % game_model), tik_text(name, game_model))
        shaders += shader_text(name, sp)
        if not args.no_textures:
            tex = os.path.join(args.out, "textures/opm/flora")
            os.makedirs(tex, exist_ok=True)
            im, n = paint.leaf_atlas(sp.leaf["asset"], sp.leaf["tint"], sp.leaf["twig"], sp.seed,
                                     leaf_len=sp.leaf["leaf_len"], density=sp.leaf["density"])
            im.save(os.path.join(tex, name + "_leaves.tga"))
            n.save(os.path.join(tex, name + "_leaves_n.tga"))
            im, n = paint.bark(sp.bark["asset"], sp.bark["tint"])
            im.save(os.path.join(tex, name + "_bark.tga"))
            n.save(os.path.join(tex, name + "_bark_n.tga"))
        print("%-10s bark %5d tris (%d surfaces)  leaves %5d tris (%d surfaces)  %.0f x %.0f x %.0f"
              % (name, bark.count(), len(bark.surfaces), leaves.count(), len(leaves.surfaces),
                 hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]))
    write(os.path.join(args.out, "scripts/opm_flora.shader"), shaders)


if __name__ == "__main__":
    main()
