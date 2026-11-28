"""The tech demo's biomes: one zone for each kind of place the game's levels
are set in, with buildings like the ones those levels are built from.

    west of the plaza and field            east of them
    +------------------+                   +------------------+
    | Norway  (m2l1,   |                   | North Africa     |
    |  m2l3)           |                   |  (m1l1-m1l3)     |
    +------------------+  plaza / field    +------------------+
    | Ardennes (m6l1,  |   (techdemo.py)   | Omaha beach      |
    |  m6l2)           |                   |  (m3l1)          |
    +------------------+                   +------------------+
    | Bocage (m3l2,    |                   | Town (m5l1-m5l3, |
    |  m3l3, m4)       |                   |  m4l0)           |
    +------------------+                   +------------------+

Each band is one heightfield. Hill ridges fence the zones off from each other
and from the middle, with a pass through each; a caulk wall in each ridge
stops vis seeing through it at ground level. The textures and models are the
ones the retail levels use most (counted from their BSPs), at the size those
levels draw them (texscale.py). The ground is blended with decals
(ground.py); the buildings come from the kit (kit.py): glazed windows that
break, doors that open, rooms furnished to the models' own bounds.

Zones are laid out in (u, y): u is the distance from the middle's edge
outwards, so a zone reads the same on either side of the map.
"""

import math
import random

from ground import OVERLAYS, Ground, road_patches, splat_patch
import kit
from kit import CAULK, LIGHT_SCALE, Building, Style, T, box6, footprint, overlap, static
from mapgen import smooth_noise
import propbounds

# the middle (techdemo.py's plaza and field) ends at |x| = INNER; the world
# at |x| = OUTER (the retail maps go to 8192 at most); the zones' y edges
INNER = 3584
OUTER = 8064
YEDGE = 3584
ROWS = (-YEDGE, -1152, 1152, YEDGE)
STEP = 128
GROUND_BOTTOM = -448

RIDGE_H = 576
RIDGE_W = 448
EDGE_H = 416
PASS_HALF = 176
# a ridge comes down to a pass over this much (320 left lone steep cones
# at the ends of the ridges either side of a pass)
PASS_RAMP = 640

WATER_Z = -40


def wave(key, t, salt=0):
    """Smooth 1-D noise in -1..1 along t: three long sine waves, their
    phases from key."""
    r = random.Random(repr((key, salt)))
    out = 0.0
    for wl, amp in ((1900, 0.55), (820, 0.3), (370, 0.15)):
        out += amp * math.sin(t / wl * 2 * math.pi + r.uniform(0, 2 * math.pi))
    return out


def s(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


# ---------------------------------------------------------------------------
# zones


class Zone:
    """One biome. Its ground: base (most of it), rock (where it is steep),
    verge (under its roads), splats [(texture, how many, smallest, biggest)]
    and road_tex (laid along its roads)."""

    seed = 0
    amp = 60
    base = rock = verge = None
    splats = ()
    road_tex = None
    rock_slope = 0.95
    kinds = None
    # what grows along its ridges' crests (retail hills are topped by tree
    # lines, which also hide where the world ends): models, spacing
    crest = None

    def __init__(self, band, row, m):
        self.band, self.row, self.m = band, row, m
        self.sgn = band
        self.ylo, self.yhi = ROWS[row], ROWS[row + 1]
        self.passes = []
        self.pads = []
        self.roads = []                   # ([(x, y)...], half width)
        self.keep = []                    # rectangles nothing else may stand in
        self.rnd = random.Random(1000 + self.seed)

    def X(self, u):
        return self.sgn * (INNER + u)

    def P(self, u, y):
        return (self.X(u), y)

    def rect(self, u1, y1, u2, y2):
        xa, xb = self.X(u1), self.X(u2)
        return min(xa, xb), y1, max(xa, xb), y2

    def side_dists(self, x, y):
        u = abs(x) - INNER
        return {"mid": u, "out": (OUTER - INNER) - u, "s": y - self.ylo, "n": self.yhi - y}

    def side_kind(self, side):
        if side == "mid":
            return "mid"
        if side == "out" and self.kinds:
            return self.kinds.get("out", "ridge")
        return "ridge"

    def raw(self, x, y, noise):
        return noise * self.amp

    def flat_pad(self, u1, y1, u2, y2, z=None, margin=320):
        r = self.rect(u1, y1, u2, y2)
        self.pads.append(r + (z, margin))
        self.keep.append((r[0] - 96, r[1] - 96, r[2] + 96, r[3] + 96))
        return r

    def near_road(self, x, y, extra=0):
        for pts, w in self.roads:
            for (ax, ay), (bx, by) in zip(pts, pts[1:]):
                dx, dy = bx - ax, by - ay
                L2 = dx * dx + dy * dy or 1
                t = max(0, min(1, ((x - ax) * dx + (y - ay) * dy) / L2))
                if math.hypot(x - ax - t * dx, y - ay - t * dy) < w + extra:
                    return True
        return False

    def tex(self, x, y, h, slope):
        if slope > self.rock_slope:
            return T(self.rock, density=kit.ROCK_DENSITY)
        if self.road_tex and self.near_road(x, y, 40):
            return T(self.verge, density=kit.GROUND_DENSITY)
        return T(self.base, density=kit.GROUND_DENSITY)

    def plan(self):
        """Roads, pads and passes, before the ground is made."""

    def build(self, m, g):
        """Buildings and props, once the ground is made."""

    # -- props outside ----------------------------------------------------------

    def put(self, model, x, y, g, yaw=0, scale=None, sink=1, check=True):
        """A model on the ground, unless it would stand in something kept clear."""
        fp = footprint(model, x, y, round(yaw / 90) * 90, scale or 1.0)
        if check and fp and any(overlap(fp[:4], k, 4) for k in self.keep):
            return None
        static(self.m, model, x, y, g(x, y) - sink, yaw, scale)
        if fp:
            self.keep.append(fp[:4])
        return fp

    def scatter(self, g, models, count, u1, y1, u2, y2, scales=(0.8, 1.3), sink=6, spacing=96):
        placed, tries, done = 0, 0, []
        while placed < count and tries < count * 40:
            tries += 1
            u = self.rnd.uniform(u1, u2)
            y = self.rnd.uniform(y1, y2)
            x = self.X(u)
            if self.near_road(x, y, 48):
                continue
            if any(math.hypot(x - a, y - b) < spacing for a, b in done):
                continue
            if any(k[0] - 48 < x < k[2] + 48 and k[1] - 48 < y < k[3] + 48 for k in self.keep):
                continue
            model = models[placed % len(models)]
            static(self.m, model, x, y, g(x, y) - sink, self.rnd.randrange(360), self.rnd.uniform(*scales))
            done.append((x, y))
            placed += 1

    def line(self, g, model, pts, spacing, yaw_off=90, sink=2, scale=None):
        """Models in a row (sandbags, wire): each one's length lies across its facing."""
        for (ax, ay), (bx, by) in zip(pts, pts[1:]):
            L = math.hypot(bx - ax, by - ay)
            yaw = math.degrees(math.atan2(by - ay, bx - ax)) + yaw_off
            n = max(1, int(L // spacing))
            for i in range(n):
                t = (i + 0.5) / n
                x, y = ax + (bx - ax) * t, ay + (by - ay) * t
                static(self.m, model, x, y, g(x, y) - sink, yaw, scale)
            self.keep.append((min(ax, bx) - 40, min(ay, by) - 40, max(ax, bx) + 40, max(ay, by) + 40))

    def pad_z(self, r):
        return self.padz[r]

    def side_toward(self, r, x, y):
        """Which side of a building faces a point (its door toward a road)."""
        cx, cy = (r[0] + r[2]) / 2, (r[1] + r[3]) / 2
        dx, dy = x - cx, y - cy
        if abs(dx) * (r[3] - r[1]) > abs(dy) * (r[2] - r[0]):
            return "e" if dx > 0 else "w"
        return "n" if dy > 0 else "s"

    def door_lamp(self, r, side, at, z, model, color=(1, 0.85, 0.6), value=7, thick=16):
        """A lamp on the wall beside a door (its back on the wall's face), and its light."""
        x1, y1, x2, y2 = r
        b = propbounds.bounds(model)
        back = -b[0][0] + 0.5 if b else 4
        out = thick + back
        if side in "ns":
            x = x1 + (x2 - x1) * at + 60
            y = y1 - out if side == "s" else y2 + out
            yaw = 270 if side == "s" else 90
            ly = y - 24 if side == "s" else y + 24
            lx = x
        else:
            y = y1 + (y2 - y1) * at + 60
            x = x1 - out if side == "w" else x2 + out
            yaw = 180 if side == "w" else 0
            lx = x - 24 if side == "w" else x + 24
            ly = y
        static(self.m, model, x, y, z + 8 + 124, yaw)
        self.m.ent("light", origin=(lx, ly, z + 8 + 120), light=value * LIGHT_SCALE, _color=color)


def W(zone):
    """'w' or 'e': the side of a zone's buildings that faces the middle."""
    return "e" if zone.sgn < 0 else "w"


def OUT(zone):
    return "w" if zone.sgn < 0 else "e"


# --- the zones ----------------------------------------------------------------


class Norway(Zone):
    """m2l1 and m2l3: a Kriegsmarine research post in the snow. Grey concrete
    blocks with a weathered base course and snow on their flat roofs, a
    barracks hut with a pitched roof, a corrugated workshop, a bunker; pines,
    sandbags, wire, a lorry."""

    crest = (("static/tree_winter_tallpine.tik", "static/tree_winter_midpine.tik", "static/tree_winter_smallpine.tik", "static/rock_winter_medium.tik"), 150)
    name = "norway"
    seed = 21
    amp = 70
    base = "norway/norsnow_lite256"
    rock = "central_europe_winter/cliffset_2snow"
    verge = "norway/norsnowmedium"
    splats = (("norway/norsnowmedium", 26, 320, 640), ("central_europe_winter/forstsnow_rough256", 10, 256, 448))
    road_tex = "norway/road_nortraveled"
    rock_slope = 1.0

    def plan(self):
        self.passes = [("mid", 2880), ("s", -5824)]
        self.roads = [([self.P(0, 2880), self.P(1100, 2720), self.P(1800, 2300), self.P(2240, 1500), self.P(2240, 1152)], 80)]
        self.hq = self.flat_pad(1060, 1700, 1700, 2060)
        self.bunker = self.flat_pad(2560, 2560, 2816, 2816)
        self.shed = self.flat_pad(2500, 1700, 2900, 1956)
        self.hut = self.flat_pad(1300, 2850, 1652, 3090)
        # the compound (m2l1, m2l3): a yard closed by long blocks and a
        # security fence, a lane in from the road through its gate
        self.yard = (2400, 1640, 3720, 3100)
        self.gate = (2190, 2450)
        self.roads.append(([self.P(1880, 2155), self.P(2400, 2310), self.P(3300, 2330)], 64))
        self.block_a = self.flat_pad(3060, 1720, 3640, 1960, margin=256)
        self.block_b = self.flat_pad(2960, 2840, 3640, 3040, margin=256)

    def build(self, m, g):
        conc = Style("norway/resrchnorway_wall1", inner="general_structure/plaster_wall2a", floor="norway/nord_floortile",
                     ceil="general_structure/jh_conc512b", trim="general_structure/jh_conc512b",
                     base="norway/resrchnorway_wall1low", base_h=128, roof="norway/csnowconc", sill="norway/csnowconc",
                     door="central_europe_winter/door1simpl_winter")
        x1, y1, x2, y2 = self.hq
        z = self.pad_z(self.hq)
        road_side = self.side_toward(self.hq, *self.P(2000, 2000))
        Building(self, x1, y1, x2, y2, z, conc, storeys=2, storey_h=160, roof="flat", parapet=24,
                 doors=[(road_side, 0.5, 64, 120), ("n", 0.3, 64, 120)], rooms=[("office", "barracks"), ("office", "office")],
                 partition=True, lamp="static/lightbulb_caged.tik", light=12, light_color=(0.9, 0.95, 1),
                 decals=[("norway/versdepot1", "n", 0.7, 128, 128, 32)])
        self.door_lamp(self.hq, road_side, 0.5, z, "static/lightbulb_caged.tik", (0.9, 0.95, 1))
        static(m, "static/static_kriegsflag.tik", (x1 + x2) / 2, y1 - 17, z + 8 + 200, 270)

        bun = Style("general_structure/concretewall_winter_bunker", inner="general_structure/jh_conc512b", floor="algiers/whsflrset1_1a",
                    ceil="general_structure/jh_conc512b", trim="general_structure/jh_conc512b", roof="norway/csnowconc")
        x1, y1, x2, y2 = self.bunker
        Building(self, x1, y1, x2, y2, self.pad_z(self.bunker), bun, storey_h=120, thick=32, roof="flat", roof_t=48, windows=None,
                 doors=[("s", 0.5, 64, 104)], slits=[("n", 0.5, 128, 56, 80), (OUT(self), 0.5, 96, 56, 80)], rooms=["bunker"],
                 lamp="static/lightbulb_caged.tik", light=8,
                 decals=[("norway/mundepot1", "s", 0.5, 108, 128, 32), ("general_industrial/bunkervent_1", W(self), 0.5, 70, 32, 32)])

        shed = Style("general_structure/jh_corrugate_3", inner="general_structure/jh_corrugate_3", floor="general_structure/garagefloor",
                     trim="central_europe_winter/ibeam_vertwnter", roof="central_europe_winter/corrugated_snowcovered",
                     under="general_structure/jh_corrugate_3", gable="general_structure/jh_corrugate_3")
        x1, y1, x2, y2 = self.shed
        Building(self, x1, y1, x2, y2, self.pad_z(self.shed), shed, storey_h=176, roof="gable", slope=0.3, windows=None, chimney=False,
                 attic=False, doors=[("n", 0.5, 160, 144, "open")], rooms=["garage"], lamp="static/lightbulb_caged.tik", light=10,
                 decals=[("norway/torpedolager1", "n", 0.5, 152, 160, 40)])

        hut = Style("general_structure/plank_flat", inner="central_europe/interiorwall_set2wtrim", floor="general_structure/plank_flat",
                    trim="central_europe_winter/joist_w", roof="central_europe_winter/winterroof_set1aw", under="general_structure/plank_flat",
                    base="norway/resrchnorway_wall1low", base_h=24, icicles=True, door="central_europe_winter/door1simpl_winter")
        x1, y1, x2, y2 = self.hut
        Building(self, x1, y1, x2, y2, self.pad_z(self.hut), hut, storey_h=152, roof="gable", slope=0.7,
                 doors=[("s", 0.5, 64, 112)], rooms=["hut"], lamp="static/lightbulb_caged.tik", light=8)

        # the long blocks round the yard: nobody goes in, so their windows
        # are painted and lit, in even rows (m2l1)
        blk = Style("norway/resrchnorway_wall1", base="norway/resrchnorway_wall1low", base_h=128, roof="norway/csnowconc",
                    sill="norway/csnowconc", band="general_structure/jh_conc512b")
        lit = ("light/jh_rustwin1lit", 112, 88, 48, 208)
        door = "central_europe_winter/door1simpl_winter"
        x1, y1, x2, y2 = self.block_a
        kit.block(m, x1, y1, x2, y2, self.pad_z(self.block_a), blk, storey_h=184, windows=lit, sides="ns" + W(self),
                  doors=[("n", 0.25, 64, 120, door)], decals=[("norway/versdepot1", "n", 0.75, 112, 96, 24)])
        x1, y1, x2, y2 = self.block_b
        kit.block(m, x1, y1, x2, y2, self.pad_z(self.block_b), blk, storeys=2, storey_h=152, windows=lit, sides="ns" + W(self),
                  doors=[("s", 0.6, 64, 120, door)])
        static(m, "static/static_kriegsflag.tik", (x1 + x2) / 2, y1 - 17, self.pad_z(self.block_b) + 8 + 240, 270)
        self.fence(g)
        self.poles(g)

        # the yard: a lorry by the workshop, crates and cable drums, sandbags, wire
        sx1, sy1, sx2, sy2 = self.shed
        self.put("static/vehicle_opeltruck.tik", (sx1 + sx2) / 2, sy2 + 150, g, yaw=0)
        for i, mdl in enumerate(("static/static_cablespool_full.tik", "static/nazi_crate.tik", "static/nazi_crate.tik", "static/exp_crate1.tik")):
            self.put(mdl, (sx2 + 70) if self.sgn > 0 else (sx1 - 70), sy1 + 40 + i * 64, g, yaw=90 * (i % 2))
        bx1, by1, bx2, by2 = self.bunker
        self.line(g, "static/sandbag_link_main_winter.tik", [(bx1 - 130, by1 - 130), (bx2 + 130, by1 - 130)], 104)
        u1, y1, u2, y2 = self.yard
        self.line(g, "static/barbwire_long.tik", [self.P(u2 + 90, y1 + 40), self.P(u2 + 90, y2 - 40)], 432)
        hx1, hy1, hx2, hy2 = self.hq
        for k in range(3):
            self.put("static/nazi_crate.tik", (hx1 - 70) if self.sgn < 0 else (hx2 + 70), hy1 + 60 + k * 64, g, yaw=0)
        # the compound is open snow; the pines stand outside it and on the hills
        self.scatter(g, ["static/tree_winter_midpine.tik", "static/tree_winter_tallpine.tik", "static/tree_winter_smallpine.tik"],
                     9, 900, 2500, 2300, 3136, scales=(0.8, 1.3), spacing=300)
        self.scatter(g, ["static/rock_winter_medium.tik", "static/bush_winter_denseshrub.tik"], 6, 900, 1600, 2300, 3136, sink=10, spacing=260)

    def fence(self, g):
        """The yard's security fence (m2l1: secfence1_wntr panels 324 long and
        164 high between I-beam posts), open at the gate."""
        u1, y1, u2, y2 = self.yard
        ga, gb = self.gate
        X = self.X
        runs = [((u1, y1), (u2, y1)), ((u2, y1), (u2, y2)), ((u1, y2), (u2, y2)), ((u1, y1), (u1, ga)), ((u1, gb), (u1, y2))]
        out = []
        mesh = "central_europe_winter/secfence1_wntr"
        post = "central_europe_winter/ibeam_vertwnter"
        posts = set()
        for (au, ay), (bu, by) in runs:
            L = abs(bu - au) + abs(by - ay)
            n = max(1, int(round(L / 324)))
            along_x = by == ay
            for k in range(n + 1):
                f = k / n
                u, y = au + (bu - au) * f, ay + (by - ay) * f
                x = X(u)
                gz = g(x, y)
                if (round(x), round(y)) not in posts:
                    posts.add((round(x), round(y)))
                    out.append(box6((x - 5, y - 5, gz - 32), (x + 5, y + 5, gz + 184), T(post, u0=x if along_x else y, v0=gz)))
                if k == n:
                    continue
                ua, ya = au + (bu - au) * k / n, ay + (by - ay) * k / n
                ub, yb = au + (bu - au) * (k + 1) / n, ay + (by - ay) * (k + 1) / n
                xa, xb = sorted((X(ua) , X(ub)))
                ya, yb = sorted((ya, yb))
                zs = [g(X(ua), ya), g(X(ub), yb), g((X(ua) + X(ub)) / 2, (ya + yb) / 2)]
                zb, zt = min(zs) - 24, max(zs) + 164
                if along_x:
                    t = T(mesh, u0=xa, v0=zt - 192)
                    out.append(box6((xa + 5, ya - 0.5, zb), (xb - 5, ya + 0.5, zt), t))
                else:
                    t = T(mesh, u0=ya, v0=zt - 192)
                    out.append(box6((xa - 0.5, ya + 5, zb), (xa + 0.5, yb - 5, zt), t))
        self.m.add(out)
        for (au, ay), (bu, by) in runs:
            self.keep.append((min(X(au), X(bu)) - 32, min(ay, by) - 32, max(X(au), X(bu)) + 32, max(ay, by) + 32))

    def poles(self, g):
        """Lamps along the lanes at a steady pitch, and the poles that carry
        the wires along the road (m2l1's are 14 by 14 and nearly 500 high)."""
        pole = "central_europe_winter/joist_w"
        out = []
        for pts, half in self.roads:
            run = 0.0
            for (ax, ay), (bx, by) in zip(pts, pts[1:]):
                L = math.hypot(bx - ax, by - ay)
                nx, ny = -(by - ay) / L, (bx - ax) / L
                t = (448 - run) % 448
                while t < L:
                    f = t / L
                    x, y = ax + (bx - ax) * f + nx * (half + 72), ay + (by - ay) * f + ny * (half + 72)
                    if not any(k[0] - 16 < x < k[2] + 16 and k[1] - 16 < y < k[3] + 16 for k in self.keep):
                        gz = g(x, y)
                        if half > 70:
                            # a wire pole with its cross arm
                            out.append(box6((x - 7, y - 7, gz - 40), (x + 7, y + 7, gz + 440), T(pole, u0=x, v0=gz)))
                            out.append(box6((x - 7, y - 56, gz + 400), (x + 7, y + 56, gz + 412), T(pole, u0=y, v0=gz + 400)))
                        else:
                            static(self.m, "static/lightpost_globe_winter.tik", x, y, gz - 2, 0)
                            self.m.ent("light", origin=(round(x, 1), round(y, 1), round(gz + 150, 1)), light=6 * LIGHT_SCALE, _color=(1, 0.9, 0.7))
                        self.keep.append((x - 24, y - 24, x + 24, y + 24))
                    t += 448
                run = (run + L) % 448
        self.m.add(out)


class Ardennes(Zone):
    """m6l1 and m6l2: the Siegfried forest in winter. Pines close together,
    rocks, a snowed-in bunker with sandbags, and two half-timbered houses on
    a snowy street (the Communications Blackout's village), icicles on their
    eaves."""

    crest = (("static/tree_winter_tallpine.tik", "static/tree_winter_midpine.tik", "static/tree_winter_tallpine.tik", "static/bush_winter_denseshrub.tik"), 130)
    name = "ardennes"
    seed = 33
    amp = 110
    base = "central_europe_winter/forstsnow_lite256"
    rock = "central_europe_winter/cliffset_2snow"
    verge = "central_europe_winter/forstsnow_med256"
    splats = (("central_europe_winter/forstsnow_rough256", 22, 320, 640), ("central_europe_winter/snow_bumpy_1", 14, 256, 512),
              ("central_europe_winter/forstsnow_med256", 14, 384, 640))
    road_tex = "central_europe_winter/stset_1awinter"
    rock_slope = 0.9

    def plan(self):
        self.passes = [("mid", 250), ("n", -5824), ("s", -6400)]
        self.roads = [([self.P(0, 250), self.P(900, 200), self.P(1500, 0), self.P(2240, 0), self.P(2816, -400), self.P(2816, -1152)], 88),
                      ([self.P(2240, 0), self.P(2240, 1152)], 64)]
        self.h1 = self.flat_pad(1240, 210, 1720, 530)
        self.h2 = self.flat_pad(1900, -520, 2300, -200)
        self.bunker = self.flat_pad(3000, 280, 3320, 560)

    def build(self, m, g):
        for r, broken, rooms in ((self.h1, (), [("kitchen", "living"), "bedroom"]), (self.h2, ((1, 1),), ["living", "bedroom"])):
            x1, y1, x2, y2 = r
            z = self.pad_z(r)
            st = Style("central_europe_winter/tudor_set1w_exwall1flatw", inner="central_europe/interiorwall_set2wtrim",
                       floor="interior/manor_woodfloor", ceil="general_structure/plank_flat", trim="central_europe_winter/joist_w",
                       base="central_europe_winter/stonebricks1drk_wntr", base_h=48, roof="central_europe_winter/winterroof_set1w",
                       under="general_structure/plank_flat", gable="central_europe_winter/tudor_set2bw", icicles=True,
                       door="central_europe_winter/door1simpl_winter")
            door_side = "s" if r is self.h1 else "n"
            Building(self, x1, y1, x2, y2, z, st, storeys=2, storey_h=152, roof="gable", slope=0.75, broken=broken,
                     doors=[(door_side, 0.6, 64, 112)], rooms=rooms, partition=True, lamp="lights/hang5lamp.tik", light=10)
            self.door_lamp(r, door_side, 0.6, z, "static/lightbulb_caged.tik")
        x1, y1, x2, y2 = self.bunker
        st = Style("central_europe_winter/winterbunkerwall", inner="general_structure/bunker_wall", floor="algiers/whsflrset1_1b",
                   ceil="general_structure/jh_conc512b", trim="general_structure/bunker_wall", roof="norway/csnowconc")
        Building(self, x1, y1, x2, y2, self.pad_z(self.bunker), st, storey_h=120, thick=40, roof="flat", roof_t=56, windows=None,
                 doors=[("s", 0.3, 64, 104)], slits=[("n", 0.5, 160, 56, 80)], rooms=["bunker"],
                 lamp="static/lightbulb_caged.tik", light=8)
        self.line(g, "static/sandbag_link_main_winter.tik", [(x1 - 110, y2 + 130), (x2 + 110, y2 + 130)], 104)
        self.put("static/sandbag_large_semicircle_winter.tik", (x1 + x2) / 2 - 420 * self.sgn, y2 + 230, g, yaw=90)
        hx1, hy1, hx2, hy2 = self.h1
        self.put("static/vehicle_bmwbike.tik", (hx2 + 110) if self.sgn > 0 else (hx1 - 110), hy1 - 90, g, yaw=90)
        pines = ["static/tree_winter_thintrunk.tik", "static/tree_winter_smallpine.tik", "static/tree_winter_fillpine.tik",
                 "static/tree_winter_thintrunk.tik", "static/tree_winter_thicktrunk.tik", "static/tree_winter_tallpine.tik",
                 "static/tree_winter_midpine.tik"]
        self.scatter(g, pines, 110, 800, -1000, 3900, 1000, scales=(0.8, 1.3), spacing=110)
        self.scatter(g, ["static/rock_winter_medium.tik", "static/rock_winter_large.tik", "static/bush_winter_denseshrub.tik"],
                     22, 800, -1000, 3900, 1000, scales=(0.7, 1.2), sink=12, spacing=160)


class Bocage(Zone):
    """m3l2, m3l3 and the m4 maps: Normandy farmland. Fields fenced by
    hedgerows on earth banks, a dirt lane, a stone farmhouse with a shingle
    roof, a plank barn with a wagon and hay, a small stone chapel."""

    crest = (("static/tree_oak.tik", "static/tree_commontree.tik", "static/bush_full.tik", "static/tree_oak.tik", "static/bush_regularbush.tik"), 150)
    name = "bocage"
    seed = 45
    amp = 64
    base = "wilderness/m3l3grass_1rough"
    rock = "wilderness/wldrrckset1_1"
    verge = "misc_outside/bocroad_set1b"
    splats = (("wilderness/m3l3grass_1", 26, 384, 768), ("misc_outside/bocroad_set1b", 8, 256, 448), ("brest/mud_set1", 6, 256, 384))
    road_tex = "misc_outside/bocroad_set1b"

    def plan(self):
        self.passes = [("mid", -2400), ("n", -6400)]
        self.roads = [([self.P(0, -2400), self.P(800, -2350), self.P(1500, -2100), self.P(2000, -2100), self.P(2816, -1700), self.P(2816, -1152)], 72)]
        self.farm = self.flat_pad(1300, -2740, 1748, -2468)
        self.barn = self.flat_pad(1960, -2820, 2344, -2500)
        self.church = self.flat_pad(2600, -2640, 3112, -2384)
        # its tower and spire: the landmark seen from across the fields
        self.tower = self.flat_pad(3128, -2576, 3256, -2448, margin=200)

    def build(self, m, g):
        x1, y1, x2, y2 = self.farm
        st = Style("brest/brest_wal3a", inner="central_europe/interiorwall_set2chrrl", floor="interior/tiledfloor_manordark2",
                   ceil="general_structure/beam_wood1", trim="general_structure/beam_wood1", roof="general_structure/jh_woodshingles1a",
                   under="general_structure/plank_flat", gable="brest/brest_wal3a", sill="general_structure/stonebricks1drk")
        Building(self, x1, y1, x2, y2, self.pad_z(self.farm), st, storeys=2, storey_h=152, roof="gable", slope=0.7,
                 doors=[("n", 0.4, 64, 112)], rooms=[("kitchen", "living"), "bedroom"], partition=True,
                 lamp="lights/hang5lamp.tik", light=10)
        x1, y1, x2, y2 = self.barn
        z = self.pad_z(self.barn)
        st = Style("general_structure/plank_flat", inner="general_structure/plank_flat", floor="misc_outside/bocroad_set1b",
                   trim="general_structure/beam_wood1", base="misc_outside/fieldstone_wall", base_h=40,
                   roof="general_structure/shingleornsml1", under="general_structure/plank_flat", door="general_structure/jh_barndoor1")
        Building(self, x1, y1, x2, y2, z, st, storey_h=200, roof="gable", slope=0.8, windows=None, chimney=False,
                 attic=False, doors=[("n", 0.5, 128, 156, "open"), ("s", 0.5, 64, 112)], rooms=["barn"], lamp="static/lightbulb_caged.tik", light=8,
                 decals=[("french/st_lo_paris", W(self), 0.5, 96, 56, 56)])
        self.put("static/wagon.tik", (x1 - 150) if self.sgn > 0 else (x2 + 150), (y1 + y2) / 2, g, yaw=90)
        self.put("static/haystack.tik", (x1 + x2) / 2 + 60, y2 + 180, g, yaw=0)
        self.put("static/peetrough.tik", (x2 + 60) if self.sgn > 0 else (x1 - 60), y2 - 70, g, yaw=0)
        x1, y1, x2, y2 = self.church
        z = self.pad_z(self.church)
        st = Style("general_structure/stonebricks1drk", inner="interior/hearth_set1basef", floor="algiers/fort_floor_cobbleflttile",
                   trim="general_structure/stonebricks1drk", roof="general_structure/shingleornsml1", under="general_structure/beam_wood1",
                   door="general_structure/interior_door2")
        Building(self, x1, y1, x2, y2, z, st, storey_h=288, roof="gable", slope=0.75, windows=(32, 128, 112, 112), chimney=False,
                 attic=False, doors=[(W(self), 0.5, 80, 144)], lamp="lights/hang5lamp.tik", light=12)
        for k in range(5):
            px = x1 + 150 + k * 64 if self.sgn > 0 else x2 - 150 - k * 64
            for py in (y1 + 72, y2 - 72):
                static(m, "static/churchpew.tik", px, py, z + 8, 0 if self.sgn > 0 else 180)
        self.spire(m)
        alt = x2 - 60 if self.sgn > 0 else x1 + 60
        static(m, "static/round_table.tik", alt, (y1 + y2) / 2, z + 8, 0)
        static(m, "static/german_lantern.tik", alt, (y1 + y2) / 2 + 10, z + 8 + 35, 0)
        static(m, "static/german_lantern.tik", alt, (y1 + y2) / 2 - 10, z + 8 + 35, 0)
        # hedgerows: earth banks with bushes thick along them, fields between
        rows = [
            [self.P(900, -1800), self.P(2400, -1800)],
            [self.P(3200, -1800), self.P(3800, -1800)],
            [self.P(3500, -3100), self.P(3500, -2000)],
            [self.P(950, -3000), self.P(1150, -3000)],
        ]
        for pts in rows:
            wall_run(m, pts, g, 52, "misc_outside/fencerow_stone1", top="wilderness/m3l3grass_1rough", thick=72)
            hedge(m, pts, g, lift=52)
            self.line(g, "static/bush_full.tik", pts, 230, sink=-40, scale=0.8)
            for (ax, ay), (bx, by) in zip(pts, pts[1:]):
                n = int(math.hypot(bx - ax, by - ay) // 520)
                for k in range(n):
                    t = (k + 0.5 + self.rnd.uniform(-0.25, 0.25)) / n
                    static(m, "static/tree_oak.tik", ax + (bx - ax) * t, ay + (by - ay) * t,
                           g(ax + (bx - ax) * t, ay + (by - ay) * t) + 40, self.rnd.randrange(360), self.rnd.uniform(1.0, 1.4))
        self.put("static/signpost_caen.tik", *self.P(1560, -2230), g, yaw=45)
        self.scatter(g, ["static/tree_oak.tik", "static/bush_full.tik", "static/tree_juniper.tik", "static/bush_regularbush.tik"],
                     30, 900, -3136, 3950, -1600, scales=(0.8, 1.3), spacing=180)


    def spire(self, m):
        """A square stone tower against the chapel's far gable, a louvred
        belfry near its top and a shingled spire, about 860 high in all."""
        x1, y1, x2, y2 = self.tower
        # on the chapel's floor level, so the two meet
        z = self.pad_z(self.church)
        st = Style("general_structure/church_exterior_flat", base="general_structure/church_exterior_low", base_h=136,
                   roof="general_structure/church_exterior_top", sill="general_structure/jh_conc512b", band="general_structure/jh_conc512b")
        top = kit.block(m, x1, y1, x2, y2, z, st, storeys=3, storey_h=176, parapet=0,
                        windows=("general_structure/shutter", 44, 112, 40, 128), sides="nsew", rows=[2])
        ov = 10
        roof = T("general_structure/shingleornsml1", density=kit.OUTSIDE_DENSITY)
        eave = box6((x1 - ov, y1 - ov, top), (x2 + ov, y2 + ov, top + 8), T("general_structure/jh_conc512b"))
        cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
        verts = [(x1 - ov, y1 - ov, top + 8), (x2 + ov, y1 - ov, top + 8), (x2 + ov, y2 + ov, top + 8), (x1 - ov, y2 + ov, top + 8),
                 (cx, cy, top + 8 + 330)]
        sp = kit.hull(verts, [((0, 1, 2), CAULK), ((0, 1, 4), roof), ((1, 2, 4), roof), ((2, 3, 4), roof), ((3, 0, 4), roof)])
        m.add([eave, sp])


class Desert(Zone):
    """m1l1 to m1l3: a North African town (Arzew, Algiers). Flat-roofed
    plaster houses with parapets, mosaic bands, iron grilles over their
    windows and shuttered ones on blank walls, a two-storey house, a
    corrugated motor pool with a lorry, a cafe table in the street; palms."""

    crest = (("static/tree_regularpalm.tik", "static/bush_sparseshrub.tik", "static/tree_squatpalm.tik"), 420)
    name = "desert"
    seed = 57
    amp = 48
    base = "algiers/grndset_1"
    rock = "algiers/cliffset_1"
    verge = "algiers/grndset_1a"
    splats = (("algiers/grndset_1a", 24, 320, 640), ("algiers/grndset_2af", 14, 256, 512))
    road_tex = "algiers/stset_2sand"
    rock_slope = 0.9

    def plan(self):
        self.passes = [("mid", 2300), ("s", 5824)]
        self.roads = [([self.P(0, 2300), self.P(900, 2300), self.P(2240, 2300), self.P(2240, 1152)], 104)]
        self.houses = [
            self.flat_pad(1000, 2480, 1320, 2800),
            self.flat_pad(1400, 2480, 1856, 2752),
            self.flat_pad(1000, 1720, 1384, 2104),
            self.flat_pad(1500, 1760, 1820, 2080),
        ]
        self.shed = self.flat_pad(2600, 1700, 3240, 2100)
        self.big = self.flat_pad(2560, 2480, 3008, 2864)

    def build(self, m, g):
        walls = ["algiers/afrik_wall1a", "algiers/afrik_wall1b", "algiers/algierwall1_0", "algiers/afrik_wall2fltdrk"]
        bands = ["algiers/algierwall1b_mozaic_bl", "algiers/algierwall1b_mozaic_red", "algiers/afrik_wall1brick", "algiers/algierwall1b_mozaic_bl"]
        for k, r in enumerate(self.houses + [self.big]):
            x1, y1, x2, y2 = r
            z = self.pad_z(r)
            big = r is self.big
            st = Style(walls[k % 4], inner="algiers/interiorwall_afrika1flat", floor="algiers/whsflrset1_1b", ceil="algiers/algier_ciel5",
                       trim="algiers/fort_roofwhite", base=bands[k % 4], base_h=32, roof="algiers/fort_roofwhite", sill="algiers/fort_roofwhite",
                       grille="algiers/window_decor_set1", door="mohcommon/doorold_1a")
            door_side = "s" if y1 > 2300 else "n"
            Building(self, x1, y1, x2, y2, z, st, storeys=2 if big else 1, storey_h=160, roof="flat", parapet=36,
                     doors=[(door_side, 0.5, 64, 112)], windows=(48, 96, 40, 200),
                     rooms=[("desert", "kitchen"), "desert"] if big else ["desert"], partition=big,
                     lamp="static/lightbulb_desertcage.tik", light=10, light_color=(1, 0.85, 0.6),
                     decals=[("algiers/afrika_windecal", OUT(self), 0.5, 44, 56, 63)])
            self.door_lamp(r, door_side, 0.5, z, "static/lightbulb_desertcage.tik", (1, 0.8, 0.5))
            cx = x1 + (x2 - x1) * 0.5
            dy = y1 - 40 if door_side == "s" else y2 + 40
            pot = ["static/wicker_basket_2.tik", "static/wicker_basket_3.tik", "static/basket1.tik"][k % 3]
            self.put(pot, cx - 76, dy, g, check=False)
        x1, y1, x2, y2 = self.shed
        z = self.pad_z(self.shed)
        st = Style("general_structure/jh_corrugate4a", inner="general_structure/jh_corrugate4", floor="general_industrial/basecrete_seem_pit2",
                   trim="general_industrial/verticalbrace", roof="general_structure/jh_corrugate4", under="general_structure/jh_corrugate4",
                   gable="general_structure/jh_corrugate4a")
        Building(self, x1, y1, x2, y2, z, st, storey_h=208, roof="gable", slope=0.3, windows=None, chimney=False,
                 attic=False, doors=[("n", 0.5, 256, 176, "open")], rooms=["garage"], lamp="static/lightbulb_covered_nowire.tik", light=12)
        self.put("static/vehicle_opeltruck.tik", (x1 + x2) / 2, y2 + 210, g, yaw=0)
        cx, cy = self.P(1950, 2570)
        self.put("static/tablewithumbrella.tik", cx, cy, g)
        for ox, oy, yw in ((-70, 0, 0), (70, 0, 180), (0, -70, 90)):
            self.put("furniture/simplechair.tik", cx + ox, cy + oy, g, yaw=yw)
        self.put("static/produce_cart.tik", *self.P(1180, 2390), g, yaw=0)
        self.scatter(g, ["static/tree_regularpalm.tik", "static/tree_squatpalm.tik", "static/bush_full.tik", "static/tree_tallyucca.tik"], 26,
                     900, 1600, 3700, 3136, scales=(1.0, 1.5), spacing=200)


class Beach(Zone):
    """m3l1: Omaha. A grassy bluff with a concrete casemate and two
    pillboxes, sandbagged firing positions, looking down on sand and a sea
    cove full of hedgehogs, mined poles, ramps and wire."""

    name = "beach"
    seed = 69
    amp = 30
    base = "test/omaha_pjspick5"
    # a sandy earth bluff (algiers' cliffs), not masonry
    rock = "algiers/cliffset2"
    verge = "test/omaha_pjspick5"
    splats = (("wilderness/m3l3grass_1", 18, 320, 640),)
    BLUFF = 1800
    SAND = 2500
    SHORE = 3350

    # shell craters (u, y, radius, depth): a bowl with a thrown-up lip,
    # on the bluff top and the sand (m3l1's are 200-300 across)
    CRATERS = ((900, -420, 256, 56), (2000, 450, 256, 40), (2150, -520, 300, 40), (2760, 160, 256, 28), (1050, 300, 224, 44))

    def raw(self, x, y, noise):
        u = abs(x) - INNER
        bluff = 144 + noise * 40
        beach = -12 - (u - self.SAND) * 0.02 + noise * 8
        if u < self.BLUFF:
            h = bluff
        elif u < self.SAND:
            t = s((u - self.BLUFF) / (self.SAND - self.BLUFF))
            h = bluff * (1 - t) + beach * t
        elif u < self.SHORE:
            h = beach
        else:
            return max(beach - (u - self.SHORE) * 0.3, -260)
        for cu, cy, r, d in self.CRATERS:
            q = math.hypot(u - cu, y - cy) / r
            if q < 1:
                h -= d * (1 - q * q) ** 2 - d * 0.3 * q ** 4
            elif q < 1.6:
                h += d * 0.3 * (1 - (q - 1) / 0.6) ** 2
        return h

    def tex(self, x, y, h, slope):
        u = abs(x) - INNER
        if slope > 1.1:
            return T(self.rock, density=kit.ROCK_DENSITY)
        if u < self.BLUFF + 200 and h > 60:
            return T("wilderness/m3l3grass_1rough", density=kit.GROUND_DENSITY)
        return T(self.base, density=kit.GROUND_DENSITY)

    def splat_region(self, u, y, material):
        if "grass" in material:
            return u < self.BLUFF + 300
        return u < self.SHORE - 100

    def plan(self):
        self.passes = [("mid", 250)]
        self.case = self.flat_pad(1300, -240, 1620, 180)
        self.pill = self.flat_pad(1200, 600, 1392, 792)
        self.pill2 = self.flat_pad(1240, -760, 1432, -568)

    def build(self, m, g):
        st = Style("blasted/dday_bunker_wall1bsml", inner="normandy/bunker_conc2", floor="algiers/whsflrset1_1a",
                   ceil="normandy/bunk_ceiling", trim="normandy/bunker_conc2", roof="blasted/dday_bunker_wall1crkdsml")
        sea, land = ("e", "w") if self.sgn > 0 else ("w", "e")
        x1, y1, x2, y2 = self.case
        z = self.pad_z(self.case)
        b = Building(self, x1, y1, x2, y2, z, st, storey_h=128, thick=48, roof="flat", roof_t=64, windows=None,
                     doors=[(land, 0.5, 64, 104)], slits=[(sea, 0.5, 224, 56, 84)], lamp="static/lightbulb_caged.tik", light=8,
                     decals=[("common/blastmark3", sea, 0.2, 20, 96, 96), ("general_industrial/bunkervent_1", "n", 0.5, 70, 32, 32)])
        # the gun on a table under the slit, then the rest of the room round it
        room = b.rooms[0]
        room.openings[sea] = []
        fp = room.against("static/bunkertable.tik", side=sea, along=(y1 + y2) / 2, gap=4)
        if fp:
            static(m, "statweapons/mg42_bipod.tik", (fp[0] + fp[2]) / 2, (fp[1] + fp[3]) / 2, room.z + fp[4] + 17, 0 if self.sgn > 0 else 180)
        kit.kit_bunker(room)
        for r in (self.pill, self.pill2):
            x1, y1, x2, y2 = r
            Building(self, x1, y1, x2, y2, self.pad_z(r), st, storey_h=104, thick=40, roof="flat", roof_t=48, windows=None,
                     doors=[(land, 0.5, 56, 96, "open")], slits=[(sea, 0.5, 112, 48, 72), ("n", 0.5, 64, 48, 72), ("s", 0.5, 64, 48, 72)],
                     light=5)
        top = self.BLUFF - 120
        self.line(g, "static/sandbag_link_main.tik", [self.P(top, -1000), self.P(top, -300)], 104)
        self.line(g, "static/sandbag_link_main.tik", [self.P(top, 300), self.P(top, 1000)], 104)
        self.put("static/mg42ammoboxwbelt.tik", *self.P(top - 90, 380), g, check=False)
        for row, (model, spacing) in enumerate((("static/hedgehog.tik", 200), ("static/minepole.tik", 240),
                                                ("static/ramp_tripleleg.tik", 320), ("static/hedgehog.tik", 220))):
            u = self.SAND + 160 + row * 190
            for y in range(-900, 901, spacing):
                yy = y + self.rnd.uniform(-40, 40) + (spacing / 2 if row % 2 else 0)
                if abs(yy) > 960:
                    continue
                x = self.X(u + self.rnd.uniform(-30, 30))
                static(m, model, x, yy, g(x, yy) - 4, self.rnd.randrange(360))
        self.line(g, "static/barbwire_long.tik", [self.P(self.SAND - 60, -960), self.P(self.SAND - 60, 960)], 432)
        for y in (-700, -200, 400, 850):
            x = self.X(self.SAND + 40)
            static(m, "static/barbwire_tetra_post.tik", x, y, g(x, y) - 2, self.rnd.randrange(360))
        self.scatter(g, ["static/bush_full.tik", "static/bush_sparseshrub.tik"], 12, 900, -1000, self.BLUFF - 200, 1000, spacing=200)

    def sea(self, m):
        xa, xb = sorted((self.X(self.SHORE - 200), self.X(OUTER - INNER)))
        m.add(box6((xa, self.ylo, GROUND_BOTTOM + 32), (xb, self.yhi, WATER_Z), T("misc_outside/deepbluesea"), detail=False))


class Town(Zone):
    """m5l1 to m5l3 and m4l0: a small European town. Half-timbered and
    plastered houses with tile and shingle roofs along a cobbled street, one
    of them shelled open, a bigger plastered house, posters on the walls,
    street lamps, a burnt-out car, field stone walls round the gardens."""

    crest = (("static/tree_commontree.tik", "static/tree_oak.tik", "static/bush_full.tik", "static/tree_juniper.tik"), 170)
    name = "town"
    seed = 81
    amp = 36
    base = "wilderness/m3l3grass_1rough"
    rock = "wilderness/wldrrckset1_1"
    verge = "brest/mud_set1"
    splats = (("brest/mud_set1", 22, 320, 640), ("wilderness/m3l3grass_1", 12, 384, 640))
    road_tex = "central_europe/small_cobble"

    def plan(self):
        self.passes = [("mid", -2400), ("n", 5528)]
        self.roads = [([self.P(0, -2400), self.P(900, -2400), self.P(3200, -2400)], 120),
                      ([self.P(1944, -2400), self.P(1944, -1152)], 64)]
        # terraced, the retail town way (m5l1, m4l0): the houses stand wall
        # to wall along the street (two walls' thickness apart), so the street
        # is a corridor of fronts; the north row leaves the lane at u 1944 open
        self.row_n, self.row_s = [], []
        u = 1000
        for w in (320, 384, 0, 288, 352, 320):
            if not w:
                u = 2016
                continue
            self.row_n.append(self.flat_pad(u, -2200, u + w, -1900))
            u += w + 32
        u = 1040
        for w in (320, 384, 288, 352):
            self.row_s.append(self.flat_pad(u, -2900, u + w, -2600))
            u += w + 32
        self.hall = self.flat_pad(3150, -3000, 3700, -2600)

    def build(self, m, g):
        roofs = ["misc_outside/tileroof1", "general_structure/rotshingle1"]
        looks = [("central_europe/remgn_wallexter1", True), ("general_structure/plaster_wall2a", False),
                 ("central_europe/tudor_set1w_exwall1flat", False), ("central_europe/nrmndywlset_2a", True)]
        posters = ["french/renault", "french/revue_poster2", "french/st_lo_paris", "french/1st_stage", "french/motosalerepair2"]
        kinds = [[("kitchen", "living"), "bedroom"], ["living", ("bedroom", "bedroom")], ["kitchen", "bedroom"], [("living", "kitchen"), "bedroom"]]
        for k, r in enumerate(self.row_n + self.row_s):
            x1, y1, x2, y2 = r
            z = self.pad_z(r)
            north = k < len(self.row_n)
            wall, timber = looks[k % 4]
            sh = (152, 144, 160)[k % 3]
            # the base course a storey's worth or most of one (retail: 128
            # to 224; the ground storey of stone under timber or plaster),
            # and a beam band at each floor
            base, base_h = (("misc_outside/fieldstone_wall", 96), ("general_structure/stonebricks1drk", sh - 8),
                            ("misc_outside/fieldstone_wall", 128), ("general_structure/stonebricks1drk", sh - 8))[k % 4]
            st = Style(wall, inner="central_europe/interiorwall_set3chrrl", floor="interior/manor_woodfloor", ceil="general_structure/plank_flat",
                       trim="general_structure/beam_wood1", base=base, base_h=base_h, roof=roofs[k % 2],
                       under="general_structure/plank_flat", storey_tile=timber, door=["mohcommon/doorold_1a", "general_structure/interior_door2"][k % 2],
                       band="general_structure/beam_wood1")
            broken = ((-1 if north else 1, 1), (-1 if north else 1, 2)) if k == 2 else ()
            door_side = "s" if north else "n"
            poster = posters[k % 5]
            # the poster at its own shape, about 64 high (96 wide at most)
            iw, ih = kit._image_size(poster)
            ph = 64 if iw <= ih * 1.5 else 40
            pw = min(96, ph * iw / ih)
            ph = pw * ih / iw
            decals = [(poster, door_side, 0.2, 40, pw, ph)]
            if k == 2:
                decals.append(("common/blastmark3", door_side, 0.78, 60, 128, 128))
            Building(self, x1, y1, x2, y2, z, st, storeys=(2, 3, 2, 2, 3)[k % 5], storey_h=sh, roof="gable",
                     slope=(0.7, 0.6, 0.8)[k % 3], broken=broken, overhang=8,
                     doors=[(door_side, 0.5, 64, 112)], rooms=kinds[k % 4], partition=True, lamp="lights/hang5lamp.tik", light=10,
                     decals=decals)
            if k == 2:
                # the shelled house's rubble spills into the street
                fy = y1 - 16 if north else y2 + 16
                d = -1 if north else 1
                self.put("static/rubble_bigpile.tik", x1 + (x2 - x1) * 0.72, fy + d * 70, g, yaw=self.rnd.randrange(360), sink=4, check=False)
                for f, o in ((0.45, 60), (0.9, 110), (0.62, 170)):
                    self.put("static/rubble_smallpile.tik", x1 + (x2 - x1) * f, fy + d * o, g, yaw=self.rnd.randrange(360), sink=2, check=False)
        x1, y1, x2, y2 = self.hall
        z = self.pad_z(self.hall)
        st = Style("general_structure/plaster_wall2", inner="central_europe/interiorwall_set2chrrl", floor="interior/tiledfloor_manordark2",
                   ceil="general_structure/plaster_wall2", trim="general_structure/stonebricks1drk", base="general_structure/stonebricks1drk",
                   base_h=56, roof="misc_outside/tileroof1", under="general_structure/beam_wood1", door="general_structure/interior_door2")
        Building(self, x1, y1, x2, y2, z, st, storeys=2, storey_h=176, roof="gable", slope=0.55,
                 doors=[("n", 0.5, 64, 128)], windows=(48, 96, 48, 150), rooms=[("hall", "office"), ("hall", "bedroom")], partition=True,
                 lamp="lights/hang5lamp.tik", light=12, decals=[("french/familistere", "n", 0.5, 150, 192, 24)])
        for u in (1100, 1700, 2300, 2900):
            for yy, du in ((-2260, 0), (-2540, 60)):
                self.put("static/lightpost_globe.tik", *self.P(u + du, yy), g, check=False)
        for u in (1400, 2600):
            x, y = self.P(u, -2400)
            m.ent("light", origin=(x, y, g(x, y) + 128), light=7 * LIGHT_SCALE, _color=(1, 0.85, 0.6))
        self.put("static/vehicle_car_burnt.tik", *self.P(2450, -2440), g, yaw=8)
        self.put("static/w_parkbench.tik", *self.P(3330, -2290), g, yaw=270)
        self.put("static/cart.tik", *self.P(1290, -2520), g, yaw=0)
        self.put("static/signpost_caen.tik", *self.P(1980, -2290), g, yaw=135)
        wall = "misc_outside/fieldstone_wall"
        wall_run(m, [self.P(950, -1720), self.P(2900, -1720)], g, 48, wall)
        wall_run(m, [self.P(980, -3060), self.P(3000, -3060)], g, 48, wall)
        self.scatter(g, ["static/tree_commontree.tik", "static/bush_full.tik", "static/tree_juniper.tik"], 16,
                     900, -3136, 3900, -1600, scales=(0.8, 1.2), spacing=200)


HEDGE = "wilderness/m3l3hedgerowsml"


def hedge(m, pts, ground, lift=0, height=380, sink=60, gap=44, seg=520):
    """A bocage hedgerow the retail way (m3l2, m4l0): tall curved curtains
    of the hedge picture, one picture per segment about 500 long, sunk into
    the ground, two of them a little apart so it has a body, bowed out in
    the middle and wandering a little along its line."""
    from ground import Patch
    rnd = random.Random(repr(pts))
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        L = math.hypot(bx - ax, by - ay)
        n = max(1, int(round(L / seg)))
        nx, ny = -(by - ay) / L, (bx - ax) / L
        for k in range(n):
            for side in (-1, 1):
                rows = []
                for i in range(3):
                    f = (k + i / 2) / n
                    wob = rnd.uniform(-14, 14) if i == 1 else 0
                    cx, cy = ax + (bx - ax) * f, ay + (by - ay) * f
                    g = ground(cx, cy) + lift
                    h = height * rnd.uniform(0.9, 1.08) if i == 1 else height
                    row = []
                    for j, (z, bulge) in enumerate(((g - sink, 0), (g + h * 0.45, 30), (g + h, -6))):
                        o = side * (gap / 2 + bulge) + wob
                        row.append((round(cx + nx * o, 1), round(cy + ny * o, 1), round(z, 1),
                                    i / 2 if side > 0 else 1 - i / 2, 1 - j / 2))
                    rows.append(row)
                m.add([Patch(HEDGE, rows)])


def wall_run(m, pts, ground, height, tex, top=None, thick=24, step=128, sink=40):
    """A low wall along a polyline, following the ground in step-long pieces."""
    out = []
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        L = math.hypot(bx - ax, by - ay)
        n = max(1, int(round(L / step)))
        for i in range(n):
            px1, py1 = ax + (bx - ax) * i / n, ay + (by - ay) * i / n
            px2, py2 = ax + (bx - ax) * (i + 1) / n, ay + (by - ay) * (i + 1) / n
            gz = [ground(px1, py1), ground(px2, py2)]
            zb, zt = min(gz) - sink, max(gz) + height
            along_x = abs(bx - ax) >= abs(by - ay)
            side = T(tex, u0=min(px1, px2) if along_x else min(py1, py2), v0=zb)
            tt = T(top or tex)
            if along_x:
                out.append(box6((min(px1, px2), min(py1, py2) - thick / 2, zb), (max(px1, px2), max(py1, py2) + thick / 2, zt), side, pz=tt))
            else:
                out.append(box6((min(px1, px2) - thick / 2, min(py1, py2), zb), (max(px1, px2) + thick / 2, max(py1, py2), zt), side, pz=tt))
    m.add(out)


ZONES = [
    (-1, 2, Norway), (-1, 1, Ardennes), (-1, 0, Bocage),
    (1, 2, Desert), (1, 1, Beach), (1, 0, Town),
]


# ---------------------------------------------------------------------------
# the ground


class Band:
    """The ground on one side of the middle: a heightfield over its three zones."""

    def __init__(self, band, zones):
        self.band = band
        self.zones = zones
        self.x0 = INNER if band > 0 else -OUTER
        self.nx = (OUTER - INNER) // STEP + 1
        self.ny = (2 * YEDGE) // STEP + 1
        self.noise = {z: smooth_noise(self.nx, self.ny, seed=z.seed, octaves=3) for z in zones}

    def zone_at(self, y):
        for z in self.zones:
            if z.ylo <= y <= z.yhi:
                return z
        return self.zones[0]

    def point(self, i, j):
        return self.x0 + i * STEP, -YEDGE + j * STEP

    # A ridge's crest: retail hills never run dead straight at one height,
    # so the crest wanders across its line and rises and falls along it.
    # Keyed by the line itself, so the two zones it parts agree on it.
    def _line_key(self, side, z):
        if side == "s":
            return ("y", z.ylo)
        if side == "n":
            return ("y", z.yhi)
        return (side, z.row)

    def crest_off(self, side, z, along):
        """How far into the zone the crest lies from the zone's edge."""
        if side == "mid":
            return 0.0
        m = wave(self._line_key(side, z), along, self.band)
        if side == "out":
            return 80 + 80 * m                 # 0..160 in from the world's edge
        return 120 * m * (1 if side == "s" else -1)

    def crest_h(self, side, z, along):
        m = wave(self._line_key(side, z) + ("h",), along, self.band)
        return (0.95 + 0.15 * m) if side == "mid" else (0.9 + 0.2 * m)

    def height(self, i, j):
        x, y = self.point(i, j)
        z = self.zone_at(y)
        raw = z.raw(x, y, self.noise[z][j][i])
        d = z.side_dists(x, y)
        fade, ridge = 1.0, 0.0
        near_mid = s(d["mid"] / (2 * RIDGE_W))
        for side, dist in d.items():
            kind = z.side_kind(side)
            if kind == "open":
                continue
            along = y if side in ("mid", "out") else x
            keep = 1.0
            for pside, pat in z.passes:
                if pside == side:
                    keep = min(keep, s((abs(along - pat) - PASS_HALF) / PASS_RAMP))
            if kind == "mid":
                fade = min(fade, s(dist / (2 * RIDGE_W)))
                if dist < 2 * RIDGE_W:
                    hk = self.crest_h(side, z, along)
                    ridge = max(ridge, EDGE_H * hk * s(1 - abs(dist - RIDGE_W) / RIDGE_W) * keep)
            else:
                fade = min(fade, s(dist / RIDGE_W))
                # the crest wanders off the zone's edge and rises and falls
                # along it (the same for the zones either side of it)
                dc = abs(dist - self.crest_off(side, z, along))
                if dc < RIDGE_W:
                    hk = self.crest_h(side, z, along)
                    ridge = max(ridge, RIDGE_H * hk * s(1 - dc / RIDGE_W) * keep * near_mid)
        h = raw * fade + ridge
        for (px1, py1, px2, py2, pz, margin) in z.pads:
            dx = max(px1 - x, 0, x - px2)
            dy = max(py1 - y, 0, y - py2)
            w = 1 - s((math.hypot(dx, dy) - 96) / margin)
            if w > 0:
                h = h * (1 - w) + z.padz[(px1, py1, px2, py2)] * w
        return max(h, GROUND_BOTTOM + 96)

    def make(self):
        for z in self.zones:
            z.padz = {}
            for (px1, py1, px2, py2, pz, margin) in z.pads:
                if pz is None:
                    i = int(round(((px1 + px2) / 2 - self.x0) / STEP))
                    j = int(round(((py1 + py2) / 2 + YEDGE) / STEP))
                    saved, z.pads = z.pads, []
                    pz = round(self.height(i, j))
                    z.pads = saved
                z.padz[(px1, py1, px2, py2)] = pz
        self.hs = [[round(self.height(i, j)) for i in range(self.nx)] for j in range(self.ny)]
        # the column that meets the middle stays as it is (the field and plaza meet it)
        mid_i = 0 if self.band > 0 else self.nx - 1
        self.ground = Ground(self.x0, -YEDGE, STEP, self.hs, self.tex_for, keep=lambda i, j: i == mid_i, seed=77 + self.band)
        self.at = self.ground.at

    def slope(self, i, j):
        hs = self.hs
        c = [hs[j][i], hs[j][i + 1], hs[j + 1][i], hs[j + 1][i + 1]]
        return (max(c) - min(c)) / STEP, sum(c) / 4

    def tex_for(self, i, j):
        sl, h = self.slope(i, j)
        x, y = self.point(i, j)
        x += STEP / 2
        y += STEP / 2
        return self.zone_at(y).tex(x, y, h, sl)

    def overlays(self, m):
        """The decals that blend the grounds: splats, the rock's edges, roads."""
        out = []
        at = self.at
        for z in self.zones:
            rnd = random.Random(500 + z.seed)
            u1, u2 = 2 * RIDGE_W, OUTER - INNER - RIDGE_W // 2
            for mat, count, lo, hi in z.splats:
                for k in range(count):
                    for _ in range(20):
                        u = rnd.uniform(u1, u2)
                        y = rnd.uniform(z.ylo + RIDGE_W * 0.6, z.yhi - RIDGE_W * 0.6)
                        if not hasattr(z, "splat_region") or z.splat_region(u, y, mat):
                            break
                    shader = OVERLAYS.splat(mat, 512, k % 3)
                    out.append(splat_patch(at, z.X(u), y, rnd.uniform(lo, hi), rnd.uniform(0, 360), shader))
            # the zone's own ground over the edge of its rock
            for (i, j) in list(self.ground.cell):
                x, y = self.point(i, j)
                if self.zone_at(y + STEP / 2) is not z:
                    continue
                if self.slope(i, j)[0] <= z.rock_slope:
                    continue
                flat_next = any((i + di, j + dj) in self.ground.cell and self.slope(i + di, j + dj)[0] <= z.rock_slope * 0.8
                                for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                if flat_next and rnd.random() < 0.35:
                    shader = OVERLAYS.splat(z.base, 512, rnd.randrange(3))
                    out.append(splat_patch(at, x + STEP / 2 + rnd.uniform(-40, 40), y + STEP / 2 + rnd.uniform(-40, 40),
                                           rnd.uniform(224, 352), rnd.uniform(0, 360), shader, 2.0))
            if z.road_tex:
                for pts, half in z.roads:
                    width = int(half * 2 + 96)
                    shader = OVERLAYS.road(z.road_tex, int(half * 2), width, width)
                    out += road_patches(at, pts, width, shader, width, 2.5)
        m.add(out)

    def crest_trees(self, m):
        """Each zone's crest planting along its ridges, kept off passes,
        roads and what the zone keeps clear."""
        for z in self.zones:
            if not z.crest:
                continue
            models, spacing = z.crest
            rnd = random.Random(900 + z.seed)
            span = OUTER - INNER
            lines = []
            for side in ("s", "n", "out"):
                if z.side_kind(side) != "ridge":
                    continue
                lines.append(side)
            k = 0
            for side in lines:
                lo, hi = (z.ylo, z.yhi) if side == "out" else (2 * RIDGE_W, span)
                t = lo + rnd.uniform(0, spacing)
                while t < hi:
                    t += spacing * rnd.uniform(0.6, 1.4)
                    if side == "out":
                        along = t
                        y = t + rnd.uniform(-40, 40)
                        x = z.X(span - self.crest_off(side, z, along) + rnd.uniform(-90, 60))
                    else:
                        x = z.X(t)
                        along = x
                        off = self.crest_off(side, z, along) + rnd.uniform(-110, 110)
                        y = z.ylo + off if side == "s" else z.yhi - off
                    if not (z.ylo + 24 < y < z.yhi - 24) or abs(x) > OUTER - 40:
                        continue
                    if any(ps == side and abs(along - pat) < PASS_HALF + PASS_RAMP for ps, pat in z.passes):
                        continue
                    if z.near_road(x, y, 96) or any(r[0] - 64 < x < r[2] + 64 and r[1] - 64 < y < r[3] + 64 for r in z.keep):
                        continue
                    model = models[k % len(models)]
                    k += 1
                    static(m, model, x, y, self.at(x, y) - 6, rnd.randrange(360), rnd.uniform(0.85, 1.35))

    def vis_walls(self, m):
        top = RIDGE_H - 96
        out = []
        xa, xb = sorted((self.band * INNER, self.band * OUTER))

        def cut(lo, hi, passes):
            spans = [(lo, hi)]
            for p in passes:
                a, b = p - PASS_HALF - PASS_RAMP // 2, p + PASS_HALF + PASS_RAMP // 2
                nxt = []
                for c, d in spans:
                    if b <= c or a >= d:
                        nxt.append((c, d))
                        continue
                    if a > c:
                        nxt.append((c, a))
                    if b < d:
                        nxt.append((b, d))
                spans = nxt
            return spans

        if self.band > 0:
            xa = INNER + 2 * RIDGE_W
        else:
            xb = -INNER - 2 * RIDGE_W
        # the ridges rise and fall, so the walls go in pieces, each well
        # under the ground over it
        def pieces(c, d, at, cap):
            n = max(1, int(math.ceil((d - c) / 256)))
            for k in range(n):
                a, b = c + (d - c) * k / n, c + (d - c) * (k + 1) / n
                low = min(at(a + (b - a) * f / 8) for f in range(9))
                t = min(cap, low - 64)
                if t > GROUND_BOTTOM + 64:
                    yield a, b, round(t)

        for z in self.zones:
            if z.row == 0:
                continue
            passes = [p for side, p in z.passes if side == "s"]
            for c, d in cut(xa, xb, passes):
                for a, b, t in pieces(c, d, lambda x: min(self.at(x, z.ylo - 8), self.at(x, z.ylo + 8)), top):
                    out.append(box6((a, z.ylo - 8, GROUND_BOTTOM), (b, z.ylo + 8, t), CAULK, detail=False))
        cx = self.band * (INNER + RIDGE_W)
        for z in self.zones:
            passes = [p for side, p in z.passes if side == "mid"]
            for c, d in cut(z.ylo, z.yhi, passes):
                for a, b, t in pieces(c, d, lambda y: min(self.at(cx - 8, y), self.at(cx + 8, y)), EDGE_H - 96):
                    out.append(box6((cx - 8, a, GROUND_BOTTOM), (cx + 8, b, t), CAULK, detail=False))
        m.add(out)


def build_biomes(m):
    """Adds the six zones to the map. Returns {zone name: (x, y, z)} of a spot
    just inside each one's pass from the middle."""
    spots = {}
    for band in (-1, 1):
        zones = [cls(b, row, m) for b, row, cls in ZONES if b == band]
        for z in zones:
            z.plan()
        for z in zones:
            for side, at in list(z.passes):
                if side in ("n", "s"):
                    other_row = z.row + (1 if side == "n" else -1)
                    for o in zones:
                        if o.row == other_row and ("s" if side == "n" else "n", at) not in o.passes:
                            o.passes.append(("s" if side == "n" else "n", at))
        b = Band(band, sorted(zones, key=lambda z: z.row))
        b.make()
        m.add(b.ground.brushes(GROUND_BOTTOM))
        b.vis_walls(m)
        b.overlays(m)
        for z in zones:
            z.build(m, b.at)
        b.crest_trees(m)
        for z in zones:
            if hasattr(z, "sea"):
                z.sea(m)
            mid = [at for side, at in z.passes if side == "mid"][0]
            x = z.X(RIDGE_W * 2 + 64)
            spots[z.name] = (x, mid, round(b.at(x, mid)) + 24)
    return spots
