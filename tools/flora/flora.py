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
                 tip=0.35, angle_var=0.15, shape="round", upturn=0.0):
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
        self.shape = shape          # how its children's lengths go along it: "round" (longest in the middle), "cone" (longest first)
        self.upturn = upturn        # sags near its foot and turns up toward its tip (a spruce's boughs)


class Species:
    def __init__(self, **kw):
        self.__dict__.update(kw)


def bush(replaces, bone, height, radius, stems, leaf, bark, seed, cards=4, card_size=44.0, twigs=4,
         spread=38, stem_radius=2.6, droop=0.15, lod=0.0):
    """A shrub: several stems from the ground, each with a few twigs that carry the leaves."""
    return Species(
        replaces=replaces, bone=bone, height=height, radius=radius, stems=stems, spread=math.radians(spread),
        trunk_len=0.8, trunk_radius=stem_radius,
        levels=[
            Level(4, 4, twigs, math.radians(45), 0.55, 0.6, start=0.3, gnarl=0.14, tropism=0.12, tip=0.4),
            Level(2, 3, 0, math.radians(45), 0.5, 0.5, gnarl=0.2, tropism=0.05, tip=0.3),
        ],
        cards=cards, card_size=card_size, card_droop=droop, puff=0.7, flare=0.0,
        leaf=leaf, bark=bark, bark_tile=40.0, seed=seed, lod=lod)


def spruce(replaces, bone, height, radius, branches, seed, trunk_radius, start=0.22, cards=2, card_size=110.0,
           lod=800.0):
    """Norway spruce (Picea abies), the tree of the Ardennes and of Norway: a straight stem to
    the top, branches all the way up it, longest at the bottom, sagging from the stem and
    turning up at their tips, hung all along with short side shoots dense with needles, and
    in these woods snow lying thick on top of them."""
    if not cards:  # the bare lower stems of the tall ones: a pole and a few dead boughs, as they were
        return Species(
            replaces=replaces, bone=bone, height=height, radius=radius, trunk_len=0.97, trunk_radius=trunk_radius,
            levels=[
                Level(8, 7, branches, math.radians(96), 0.42, 0.22, start=start, gnarl=0.008, tip=0.12, shape="cone",
                      angle_var=0.08),
                Level(3, 3, 0, math.radians(40), 0.5, 0.5, gnarl=0.05, tropism=-0.05, tip=0.3),
            ],
            cards=0, card_size=card_size, card_droop=0.3, puff=0.35, flare=0.2, flat=True,
            leaf=dict(asset="ph:fir_tree_01:twig", tint=(0.72, 0.80, 0.74), twig=(58, 46, 36), leaf_len=0.36,
                      density=1, snow=0.35, heavy=True),
            bark=dict(asset="pine_bark", tint=(0.8, 0.78, 0.76)),
            bark_tile=56.0, seed=seed, lod=lod, tex="spruce")
    return Species(
        replaces=replaces, bone=bone, height=height, radius=radius, trunk_len=0.97, trunk_radius=trunk_radius,
        levels=[
            Level(8, 7, int(branches * 1.3), math.radians(96), 0.42, 0.16, start=start, gnarl=0.008, tip=0.12,
                  shape="cone", angle_var=0.08),
            # the boughs: thin, sagging then turning up, clothed in side shoots from near the stem
            Level(5, 3, 7, math.radians(55), 0.34, 0.6, start=0.06, gnarl=0.04, tropism=-0.04, tip=0.25,
                  upturn=0.12),
            Level(2, 3, 0, math.radians(40), 0.5, 0.5, gnarl=0.06, tropism=-0.06, tip=0.3),
        ],
        cards=cards, card_size=card_size * 0.72, card_droop=0.3, puff=0.35, flare=0.2, flat=True,
        leaf=dict(asset="ph:fir_tree_01:twig", tint=(0.72, 0.80, 0.74), twig=(58, 46, 36), leaf_len=0.36, density=1,
                  snow=0.35, heavy=True),  # the game's are all winter trees: snow lies on their boughs
        bark=dict(asset="pine_bark", tint=(0.8, 0.78, 0.76)),
        bark_tile=56.0, seed=seed, lod=lod, tex="spruce", snowcells=True,
        leader=0.05)  # the stem goes right up: its tip is a short leading shoot


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
        cards=15, card_size=76.0, card_droop=0.22, puff=0.75, flare=0.55,
        # scanned oak leaves (spring green: darkened to summer's), and furrowed oak bark
        leaf=dict(asset="LeafSet016", tint=(0.50, 0.62, 0.42), twig=(62, 48, 34), leaf_len=0.21, density=2),
        bark=dict(asset="jolcham_oak_bark_01", tint=(0.9, 0.88, 0.85)),
        bark_tile=60.0, seed=11, lod=1100.0,
    ),
    # The game's "common tree": a young European beech (Fagus sylvatica), as stands in every
    # Norman hedge and lane: one straight smooth grey stem right up through an oval crown of
    # slender ascending branches.
    "beech": Species(
        replaces=["tree_commontree"], bone="Box02", height=391.0, radius=114.0, trunk_len=0.92, trunk_radius=8.5,
        levels=[
            Level(8, 8, 12, math.radians(52), 0.36, 0.5, start=0.3, gnarl=0.03, tip=0.25),
            Level(4, 5, 4, math.radians(50), 0.5, 0.6, start=0.3, gnarl=0.12, tropism=0.10, tip=0.3),
            Level(2, 3, 0, math.radians(45), 0.5, 0.5, gnarl=0.18, tropism=0.03, tip=0.3),
        ],
        cards=6, card_size=52.0, card_droop=0.2, puff=0.7, flare=0.3,
        leaf=dict(asset="LeafSet014", tint=(0.52, 0.66, 0.40), twig=(70, 62, 52), leaf_len=0.19, density=2),
        bark=dict(asset="Bark011", tint=(0.72, 0.72, 0.68)),
        bark_tile=44.0, seed=23, lod=650.0,
    ),
    # Silver birch (Betula pendula): a tall thin pale stem, short ascending branches and
    # hanging twigs with small leaves, light enough to see through.
    "birch": Species(
        replaces=["tree_birch"], bone="Box01", height=1127.0, radius=156.0, trunk_len=0.95, trunk_radius=11.0,
        levels=[
            Level(9, 8, 14, math.radians(42), 0.20, 0.45, start=0.38, gnarl=0.025, tip=0.2),
            Level(4, 4, 4, math.radians(55), 0.55, 0.6, start=0.3, gnarl=0.10, tropism=0.02, tip=0.3),
            Level(2, 3, 0, math.radians(50), 0.5, 0.5, gnarl=0.15, tropism=-0.35, tip=0.3),
        ],
        cards=5, card_size=64.0, card_droop=0.55, puff=0.6, flare=0.25,
        leaf=dict(asset="LeafSet003", tint=(0.62, 0.74, 0.40), twig=(60, 44, 36), leaf_len=0.13, density=2),
        bark=dict(asset="Bark011", tint=(1.25, 1.25, 1.22)),
        bark_tile=44.0, seed=31, lod=900.0,
    ),
    # The winter woods: spruces of every size, and the bare lower stems of the tall ones
    # whose crowns are out of sight overhead.
    "spruce_tall": spruce(["tree_winter_tallpine"], "Box02", 941.0, 260.0, 40, 61, 15.0),
    "spruce_fill": spruce(["tree_winter_fillpine"], "Box02", 928.0, 315.0, 36, 62, 16.0, start=0.3, card_size=125.0),
    "spruce_mid": spruce(["tree_winter_midpine"], "Box02", 498.0, 208.0, 28, 63, 9.0, start=0.12, card_size=95.0),
    "spruce_small": spruce(["tree_winter_smallpine"], "Box01", 332.0, 106.0, 20, 64, 5.5, start=0.08, cards=2,
                           card_size=58.0),
    "stem_thin": spruce(["tree_winter_thintrunk"], "Box02", 854.0, 35.0, 9, 65, 9.0, start=0.45, cards=0),
    "stem_thick": spruce(["tree_winter_thicktrunk"], "thik trunk", 854.0, 57.0, 9, 66, 15.0, start=0.45, cards=0),
    # The hedge shrubs. Hazel (Corylus avellana): many straight rods from one stool.
    "hazel": bush(["bush_full"], "Box02", 129.0, 98.0, 8,
                  dict(asset="LeafSet009", tint=(0.50, 0.60, 0.36), twig=(74, 60, 44), leaf_len=0.17, density=2),
                  dict(asset="bark_brown_02", tint=(0.8, 0.75, 0.7)), 41, card_size=38.0, lod=450.0),
    # Blackthorn (Prunus spinosa), low and dense with small leaves.
    "blackthorn": bush(["bush_regularbush"], "Box01", 35.0, 40.0, 6,
                       dict(asset="LeafSet022", tint=(0.42, 0.55, 0.34), twig=(52, 44, 40), leaf_len=0.11, density=3),
                       dict(asset="bark_brown_02", tint=(0.6, 0.58, 0.56)), 43, cards=3, card_size=20.0, twigs=3,
                       spread=55, stem_radius=1.0, lod=280.0),
    # Alder buckthorn (Frangula alnus): an upright shrub of glossy oval leaves.
    "buckthorn": bush(["bush_buckthorn"], "Box01", 132.0, 49.0, 5,
                      dict(asset="LeafSet022", tint=(0.50, 0.64, 0.38), twig=(70, 52, 44), leaf_len=0.16, density=2),
                      dict(asset="bark_brown_02", tint=(0.7, 0.66, 0.62)), 47, card_size=40.0, spread=22, lod=450.0),
    # A thin shrub and a sapling: young hornbeam (Carpinus betulus), few stems, open.
    "hornbeam": bush(["bush_sparseshrub", "bush_sappling"], "Box01", 83.0, 34.0, 3,
                     dict(asset="LeafSet001", tint=(0.62, 0.74, 0.46), twig=(70, 58, 46), leaf_len=0.18, density=2),
                     dict(asset="bark_brown_02", tint=(0.75, 0.72, 0.68)), 53, cards=3, card_size=32.0, twigs=3,
                     spread=18, stem_radius=1.4, lod=350.0),
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
        d = norm(d + rng.normal(0, lv.gnarl, 3) + UP * (lv.tropism + lv.upturn * (2 * t - 1.2)))
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
            u = (t - lv.start) / (1 - lv.start)
            if lv.shape == "cone":
                shape = 1.0 - 0.88 * u
            else:
                shape = 0.55 + 0.45 * math.sin(math.pi * min(1.0, u * 0.85 + 0.15))
            grow(sp, rng, p - cd * pr * 0.5, cd, length * lv.ratio * shape * rng.uniform(0.85, 1.12),
                 min(pr * lv.rratio, pr * 0.92), level + 1, branches, cards)
        # and it runs on into a child at its tip
        p, pd, pr = br.at(1.0)
        grow(sp, rng, p, pd, length * (0.45 if level else getattr(sp, "leader", 0.9)), pr, level + 1, branches, cards)
    else:
        for k in range(sp.cards):
            t = 0.12 + 0.88 * (k + rng.uniform(0.2, 0.8)) / sp.cards
            p, pd, pr = br.at(t)
            az += GOLDEN + rng.normal(0, 0.4)
            side = perp(pd)
            side = side * math.cos(az) + np.cross(pd, side) * math.sin(az)
            ang = rng.uniform(0.35, 1.1) if k < sp.cards - 1 else rng.uniform(0.0, 0.3)
            if getattr(sp, "flat", False):  # a conifer's sprays lie out flat to either side of the branch
                flat = norm(np.cross(pd, UP)) * (1 if k % 2 else -1)
                cards.append((p, norm(pd * math.cos(ang * 0.8) + flat * math.sin(ang * 0.8)),
                              rng.uniform(0.75, 1.15) * (0.55 + 0.45 * (1 - t)), int(rng.integers(4)), rng.uniform(-0.25, 0.25)))
                continue
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
    """A branch's bark: rings of vertexes along it, closed to a point at its tip. The bark
    goes round it a whole number of times, as near to tile units wide as that allows, and
    is as tall as it is wide."""
    pts, rads = list(br.pts), list(br.rads)
    if flare:  # the foot: rings close together where the roots spread
        for t in (0.5, 0.22, 0.08):
            pts.insert(1, pts[0] * (1 - t) + pts[1] * t)
            rads.insert(1, rads[0] * (1 - t) + rads[1] * t)
    n = len(pts)
    girth = 2 * math.pi * rads[0]
    wraps = max(1, round(girth / tile))
    tall = max(girth / wraps, tile * 0.4)
    verts, tris = [], []
    side = perp(norm(pts[1] - pts[0]))
    along = 0.0
    for i in range(n):
        d = norm(pts[min(i + 1, n - 1)] - pts[max(i - 1, 0)])
        side = norm(side - d * np.dot(side, d))  # carried along, so the tube does not twist
        other = np.cross(d, side)
        if i:
            along += np.linalg.norm(pts[i] - pts[i - 1])
        for j in range(sides + 1):
            a = 2 * math.pi * j / sides
            out = side * math.cos(a) + other * math.sin(a)
            r = rads[i]
            if flare:  # wider at the ground, in soft buttresses
                r *= 1 + flare * math.exp(-along / (rads[0] * 1.3)) * (1 + 0.22 * math.cos(a * 4 + 1.0))
            verts.append((pts[i] + out * r, out, (wraps * j / sides, -along / tall)))
    for i in range(n - 1):
        for j in range(sides):
            a, b = i * (sides + 1) + j, (i + 1) * (sides + 1) + j
            # clockwise seen from outside: the game's renderers cull the
            # counter-clockwise side (GL_FRONT) of a front-sided shader
            tris += [(a, b, a + 1), (a + 1, b, b + 1)]
    tip = len(verts)
    d = norm(pts[-1] - pts[-2])
    verts.append((pts[-1] + d * rads[-1] * 1.5, d, (wraps * 0.5, -(along + rads[-1]) / tall)))
    last = (n - 1) * (sides + 1)
    tris += [(last + j, tip, last + j + 1) for j in range(sides)]
    mesh.add(verts, tris)


def card(mesh, sp, p, d, scale, cell, roll, centre):
    """One spray of leaves: a quad along d from p, creased along its middle, its tip drooping."""
    s = sp.card_size * scale
    w = perp(d)
    up = np.cross(w, d)
    if up[2] < 0:
        w, up = -w, -up
    w, up = w * math.cos(roll) + up * math.sin(roll), up * math.cos(roll) - w * math.sin(roll)
    if getattr(sp, "snowcells", False):
        # the bottom two cells carry the snow: sprays that lie facing the sky take them
        cell = (cell % 2) + (2 if abs(up[2]) > 0.55 else 0)
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
    stems = getattr(sp, "stems", 0)
    if stems:  # a shrub: stems from the ground, leaning out all round
        for k in range(stems):
            az = 2 * math.pi * (k + rng.uniform(-0.3, 0.3)) / stems
            lean = sp.spread * rng.uniform(0.3, 1.0) if k else 0.05
            d = np.array([math.cos(az) * math.sin(lean), math.sin(az) * math.sin(lean), math.cos(lean)])
            grow(sp, rng, d * np.array([1.0, 1.0, 0.0]) * sp.trunk_radius * 2.0, d,
                 sp.height * sp.trunk_len * rng.uniform(0.75, 1.0), sp.trunk_radius * rng.uniform(0.7, 1.0), 0,
                 branches, cards)
    else:
        # (its foot a little under the ground: the maps stand trees on slopes)
        grow(sp, rng, np.array([0.0, 0.0, -0.05 * sp.height]),
             norm(np.array([rng.normal(0, 0.02), rng.normal(0, 0.02), 1.0])),
             sp.height * (sp.trunk_len + 0.05), sp.trunk_radius, 0, branches, cards)

    # A conifer's sprays deep inside its crown are not seen from outside but are drawn
    # all the same, in every shadow map too: half of them go. (outer: how far out each
    # spray stands, of the crown's reach at its height)
    outer = [1.0] * len(cards)
    if getattr(sp, "flat", False) and cards:
        z = np.array([c[0][2] for c in cards])
        r = np.array([math.hypot(c[0][0], c[0][1]) for c in cards])
        bins = np.clip(((z - z.min()) / max(np.ptp(z), 1e-6) * 12).astype(int), 0, 11)
        reach = np.array([r[bins == b].max() if (bins == b).any() else 1.0 for b in range(12)])
        outer = list(r / np.maximum(reach[bins], 1e-6))
        keep = [k for k in range(len(cards)) if outer[k] > 0.5 or k % 2 == 0]
        cards = [cards[k] for k in keep]
        outer = [outer[k] for k in keep]

    # fitted to the model it replaces: its height, and its radius about the trunk
    # (of what is drawn, so in a few rounds: the cards do not scale with the branches)
    fit = np.ones(3)
    for _ in range(10):
        thick = (fit[0] * fit[2]) ** 0.5
        fitted = []
        for b in branches:
            keep = not stems and b.level == 0  # a trunk keeps the girth it was given
            fitted.append(Branch([p * fit for p in b.pts], [r if keep else r * thick for r in b.rads], b.level))
        centre = np.array([0.0, 0.0, sp.height * (0.5 if stems else 0.58)])
        bark, leaves = Mesh("bark"), Mesh("leaves")
        for b in fitted:
            # a conifer's last shoots are the twigs in its sprays' pictures: no bark of their own
            if getattr(sp, "flat", False) and b.level == len(sp.levels) - 1 and len(sp.levels) > 2 and sp.cards:
                continue
            tube(bark, b, sp.levels[b.level].sides, sp.bark_tile, sp.flare if b.level == 0 else 0.0)
        for c in cards:
            card(leaves, sp, c[0] * fit, norm(c[1] * fit), c[2], c[3], c[4], centre)
        pts = np.array([v[0] for m in (bark, leaves) for verts, _ in m.surfaces for v in verts])
        rad = np.percentile(np.hypot(pts[:, 0], pts[:, 1]), 99.5)
        last = fit
        fit = fit * np.array([sp.radius / rad, sp.radius / rad, sp.height / pts[:, 2].max()])

    # What it is drawn as far off (GL2's r_floraLod; the shaders' distance fades elsewhere):
    # its stem and biggest limbs with fewer sides, and a third of its sprays, larger.
    meshes = [bark, leaves]
    if getattr(sp, "lod", 0):
        fit = last
        bark_far, leaves_far = Mesh("bark_far"), Mesh("leaves_far")
        for b in fitted:
            if b.level == 0 or (b.level == 1 and not stems and len(sp.levels) > 2 and not getattr(sp, "flat", False)):
                thin = Branch(b.pts[::2] + ([b.pts[-1]] if len(b.pts) % 2 == 0 else []),
                              b.rads[::2] + ([b.rads[-1]] if len(b.rads) % 2 == 0 else []), b.level)
                tube(bark_far, thin, 4 if b.level == 0 else 3, sp.bark_tile)
        # (a conifer's: of the sprays out at its crown's edge, which make its outline,
        # one in three, larger still)
        stride, larger = (3, 2.0) if getattr(sp, "flat", False) else (3, 1.75)
        grown = sp.card_size
        sp.card_size = grown * larger
        shell = [c for c, o in zip(cards, outer) if o > 0.6] if getattr(sp, "flat", False) else cards
        for c in shell[::stride]:
            # centred where the middle of its smaller self was, so the crown keeps its
            # outline and does not bristle with sprays reaching out past it
            d = norm(c[1] * fit)
            card(leaves_far, sp, c[0] * fit - d * (grown * c[2] * (larger - 1) * 0.5), d, c[2], c[3], c[4], centre)
        sp.card_size = grown
        meshes += [bark_far, leaves_far]
    return sp, meshes


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
    """Its bark and leaves; with distance levels, the near pair fading out as the far pair fades in."""
    out = ""
    lod = getattr(sp, "lod", 0)
    for far in ((False, True) if lod else (False,)):
        fade = ""
        if lod:
            fade = "\t\talphaGen %s %d %d\n" % ("oneMinusDistFade" if far else "distFade", lod, lod * 0.25)
        out += """opm_flora/%(n)s_bark%(f)s
{
	qer_editorimage textures/opm/flora/%(t)s_bark.tga
	{
		map textures/opm/flora/%(t)s_bark.tga
		rgbGen vertex
%(fade)s	}
}
opm_flora/%(n)s_leaves%(f)s
{
	qer_editorimage textures/opm/flora/%(t)s_leaves.tga
	cull none
	surfaceparm foliage
	{
		map textures/opm/flora/%(t)s_leaves.tga
		depthWrite
		alphaFunc GE128
		rgbGen vertex
%(fade)s	}
}
""" % {"n": name, "t": getattr(sp, "tex", name), "f": "_far" if far else "", "fade": fade}
    return out


def tik_text(name, surfaces):
    return """TIKI
setup
{
	scale 1
	path models/static/natural
	skelmodel opm_%(n)s.skd
	lod_scale 100
%(s)s}

animations
{
	idle     opm_%(n)s.skc
}
""" % {"n": name, "s": "".join("\tsurface %s shader opm_flora/%s_%s\n" % (m, name, m) for m in surfaces)}


LEAF_FX = """TIKI
// OPM (content/opm-trees, made by tools/flora): leaves shaken off a %(what)s by a
// blast (cgame's CG_LensBlast): they burst out of its crown, and come down
// turning over and over.
setup
{
	scale 1.0
	path models/fx/dummy
	skelmodel dummy2.skd
}

init
{
	client
	{
		cache models/fx/opm_leaf.tik

		sfx originspawn
		(
			model models/fx/opm_leaf.tik
			spawnrange 3000
			count %(count)d
			color 1 1 1
			offset crandom %(wide)d crandom %(wide)d crandom %(tall)d
			angles random 360 random 360 random 360
			avelocity crandom 240 crandom 300 crandom 240
			randvel crandom %(throw)d crandom %(throw)d range -20 %(throw)d
			accel 0 0 -160
			friction 1.6
			life 6.0 1.5
			scalemin 0.8
			scalemax 1.6
			fade
		)
	}
}

animations
{
	idle dummy2.skc
}
"""


def leaf_fx(out, textures):
    """What a blast shakes down: one leaf as a model (a quad), and the two effects that throw them."""
    m = skd.Model("opm_leaf.skd")
    m.bones = [skd.Bone("leaf")]
    s = skd.Surface("leaf")
    for x, y, u, v in ((-3.0, -4.5, 0, 1), (3.0, -4.5, 1, 1), (3.0, 4.5, 1, 0), (-3.0, 4.5, 0, 0)):
        s.verts.append(skd.Vertex((0.0, 0.0, 1.0), (u, v), [(0, 1.0, (x, y, 0.0))]))
    s.triangles = [(0, 1, 2), (0, 2, 3)]
    m.surfaces.append(s)
    a = skc.Animation()
    a.channels = ["leaf pos", "leaf rot"]
    a.values = [[(0.0, 0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)]]
    a.frames = [skc.Frame(((-5, -5, -1), (5, 5, 1)), 6.0)]
    write(os.path.join(out, "models/fx/opm_leaf.skd"), skd.write(m))
    write(os.path.join(out, "models/fx/opm_leaf.skc"), skc.write(a))
    write(os.path.join(out, "models/fx/opm_leaf.tik"), """TIKI
setup
{
	scale 1
	path models/fx
	skelmodel opm_leaf.skd
	surface leaf shader opm_flora/leaf_fall
	lod_scale 100
}

animations
{
	idle opm_leaf.skc
}
""")
    write(os.path.join(out, "models/fx/opm_leaves_tree.tik"),
          LEAF_FX % dict(what="tree", count=22, wide=130, tall=70, throw=70))
    write(os.path.join(out, "models/fx/opm_leaves_bush.tik"),
          LEAF_FX % dict(what="bush", count=9, wide=30, tall=20, throw=45))
    if textures:
        tex = os.path.join(out, "textures/opm/flora")
        os.makedirs(tex, exist_ok=True)
        paint.fallen_leaf("LeafSet014", (0.55, 0.66, 0.40)).save(os.path.join(tex, "leaf_fall.tga"))
    return """opm_flora/leaf_fall
{
	cull none
	{
		map textures/opm/flora/leaf_fall.tga
		alphaFunc GE128
		depthWrite
		rgbGen lightingSpherical
	}
}
"""


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
        sp, meshes = build(name)
        bark, leaves = meshes[0], meshes[1]
        model, anim, lo, hi = to_skd(name, sp, meshes)
        write(os.path.join(args.out, "models/static/natural/opm_%s.skd" % name), model)
        write(os.path.join(args.out, "models/static/natural/opm_%s.skc" % name), anim)
        for game_model in sp.replaces:
            write(os.path.join(args.out, "models/static/%s.tik" % game_model),
                  tik_text(name, [m.name for m in meshes if m.count()]))
        shaders += shader_text(name, sp)
        if not args.no_textures:
            tex = os.path.join(args.out, "textures/opm/flora")
            os.makedirs(tex, exist_ok=True)
            shared = getattr(sp, "tex", name)  # (kinds that differ only in shape share their textures)
            if not os.path.exists(os.path.join(tex, shared + "_bark.tga")) or shared == name:
                im, n = paint.leaf_atlas(sp.leaf["asset"], sp.leaf["tint"], sp.leaf["twig"], sp.seed,
                                         leaf_len=sp.leaf["leaf_len"], density=sp.leaf["density"],
                                         snow=sp.leaf.get("snow", 0.0), heavy=sp.leaf.get("heavy", False))
                im.save(os.path.join(tex, shared + "_leaves.tga"))
                n.save(os.path.join(tex, shared + "_leaves_n.tga"))
                im, n = paint.bark(sp.bark["asset"], sp.bark["tint"])
                im.save(os.path.join(tex, shared + "_bark.tga"))
                n.save(os.path.join(tex, shared + "_bark_n.tga"))
        print("%-12s bark %5d tris (%d surfaces)  leaves %5d tris (%d surfaces)  %.0f x %.0f x %.0f"
              % (name, bark.count(), len(bark.surfaces), leaves.count(), len(leaves.surfaces),
                 hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]))
    shaders += leaf_fx(args.out, not args.no_textures)
    write(os.path.join(args.out, "scripts/opm_flora.shader"), shaders)


if __name__ == "__main__":
    main()
