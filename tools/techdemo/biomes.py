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
and from the middle, with a pass through each ridge; a caulk wall in each
ridge stops vis seeing through it at ground level. The textures and models
are the ones the retail levels use most (counted from their BSPs: the shader
lump weighted by area, the static model lump).

The buildings are detail brushes: they don't split the vis tree, so a full
vis stays quick. Every building can be walked into, and has a lamp lit.

Zones are laid out in (u, y): u is the distance from the middle's edge
outwards, so a zone reads the same on either side of the map.
"""

import math
import random

from mapgen import Brush, Tex, smooth_noise, heightfield

CAULK = "common/caulk"

# the middle (techdemo.py's plaza and field) ends at |x| = INNER; the world
# at |x| = OUTER (the retail maps go to 8192 at most); the zones' y edges
INNER = 3584
OUTER = 8064
YEDGE = 3584
ROWS = (-YEDGE, -1152, 1152, YEDGE)
STEP = 128
GROUND_BOTTOM = -448

# the ridges round the zones: how high, how wide each side of the crest;
# the one along the middle's edge is lower and stands back from it
RIDGE_H = 576
RIDGE_W = 448
EDGE_H = 416
# a pass through a ridge: flat this far each side of its line, then rising
# to the ridge over PASS_RAMP
PASS_HALF = 176
PASS_RAMP = 320

WATER_Z = -40
LIGHT_SCALE = 15


def s(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


def T(shader, scale=0.5, **kw):
    return Tex(shader, scale, **kw)


def wall_tex(shader, z, scale=0.5, h=256):
    """A wall texture with a tile's bottom at height z, so a band (a
    plinth, a base course) sits on the floor it is meant to."""
    return Tex(shader, scale, yoff=(z / scale) % h)


# ---------------------------------------------------------------------------
# brushes


def box6(mins, maxs, t, detail=True, **faces):
    """An axis-aligned box with a texture per face: px nx py ny pz nz (plus
    and minus x, y, z) override `t`."""
    x1, y1, z1 = mins
    x2, y2, z2 = maxs
    if x1 >= x2 or y1 >= y2 or z1 >= z2:
        raise ValueError("empty box %r %r" % (mins, maxs))
    g = lambda k: faces.get(k) or t
    c = ((x1 + x2) / 2, (y1 + y2) / 2, (z1 + z2) / 2)
    return Brush([
        (((x1, y1, z2), (x2, y1, z2), (x2, y2, z2)), g("pz")),
        (((x1, y1, z1), (x2, y1, z1), (x2, y2, z1)), g("nz")),
        (((x1, y1, z1), (x1, y2, z1), (x1, y2, z2)), g("nx")),
        (((x2, y1, z1), (x2, y2, z1), (x2, y2, z2)), g("px")),
        (((x1, y1, z1), (x2, y1, z1), (x2, y1, z2)), g("ny")),
        (((x1, y2, z1), (x2, y2, z1), (x2, y2, z2)), g("py")),
    ], c, detail)


def hull(verts, faces, detail=True):
    """A convex brush from its corners: faces are (three corner indices on the
    face, texture). The winding is worked out from the corners' middle."""
    n = len(verts)
    c = tuple(sum(v[k] for v in verts) / n for k in range(3))
    return Brush([((verts[a], verts[b], verts[d]), t) for (a, b, d), t in faces], c, detail)


class Frame:
    """Local building coordinates: u along the ridge (or the long side), v
    across, z up. `swap` turns u into y and v into x."""

    def __init__(self, swap):
        self.swap = swap

    def p(self, u, v, z):
        return (v, u, z) if self.swap else (u, v, z)

    def box(self, u1, v1, z1, u2, v2, z2, t, **faces):
        if self.swap:
            # local u/v faces become y/x faces
            f = {}
            for k, val in faces.items():
                f[{"pu": "py", "nu": "ny", "pv": "px", "nv": "nx"}.get(k, k)] = val
            return box6((v1, u1, z1), (v2, u2, z2), t, **f)
        f = {}
        for k, val in faces.items():
            f[{"pu": "px", "nu": "nx", "pv": "py", "nv": "ny"}.get(k, k)] = val
        return box6((u1, v1, z1), (u2, v2, z2), t, **f)


# ---------------------------------------------------------------------------
# the building kit


class Style:
    """What a building is made of. Any texture left None falls back to
    `wall`."""

    def __init__(self, wall, inner=None, floor=None, ceil=None, trim=None, base=None, base_h=0,
                 roof=None, under=None, gable=None):
        self.wall = wall
        self.inner = inner or wall
        self.floor = floor or wall
        self.ceil = ceil or self.inner
        self.trim = trim or wall
        self.base = base           # texture of the bottom `base_h` of the outside
        self.base_h = base_h
        self.roof = roof or wall
        self.under = under or self.ceil
        self.gable = gable or wall


def _solid_pieces(lo, hi, zlo, zhi, holes):
    """The pieces of a wall from lo to hi along it and zlo to zhi up, less
    its rectangular holes (a1, a2, z1, z2): (a1, a2, z1, z2) each."""
    cuts = sorted({lo, hi} | {h[0] for h in holes} | {h[1] for h in holes})
    out = []
    for a, b in zip(cuts, cuts[1:]):
        if b <= a:
            continue
        mid = (a + b) / 2
        spans = [(zlo, zhi)]
        for h in holes:
            if h[0] <= mid <= h[1]:
                nxt = []
                for z1, z2 in spans:
                    if h[3] <= z1 or h[2] >= z2:
                        nxt.append((z1, z2))
                        continue
                    if h[2] > z1:
                        nxt.append((z1, h[2]))
                    if h[3] < z2:
                        nxt.append((h[3], z2))
                spans = nxt
        for z1, z2 in spans:
            if z2 > z1:
                out.append((a, b, z1, z2))
    return out


class Building:
    """A building on a pad: walls with doors and windows, storeys joined by a
    stair, and a flat, parapet or gable roof.

    `doors` are (side, at, width, height): side is n, s, e or w, `at` the
    door's middle along that side from its west or south end (0 to 1 as a
    fraction, or units). `windows` is (width, sill, top, spacing) for every
    storey, or None."""

    def __init__(self, m, x1, y1, x2, y2, z0, style, storeys=1, storey_h=160, thick=16,
                 roof="gable", slope=0.6, overhang=24, parapet=0, roof_t=16,
                 doors=(), windows=(48, 48, 120, 176), holes=(), lamp=None, light=12, light_color=(1, 0.9, 0.75),
                 broken=(), stair=True):
        self.m = m
        self.x1, self.y1, self.x2, self.y2, self.z0 = x1, y1, x2, y2, z0
        self.st = style
        self.T = thick
        self.floor_z = z0 + 8
        self.storeys = storeys
        self.storey_h = storey_h
        self.top = self.floor_z + storeys * storey_h
        self.doors = doors
        self.windows = windows
        self.holes = holes
        self.brushes = []
        self._walls()
        self._floors(stair and storeys > 1)
        if roof == "gable":
            self._gable(slope, overhang, broken)
        else:
            self._flat(roof_t, parapet)
        self._lights(lamp, light, light_color)
        m.add(self.brushes)

    # sides as (axis the wall runs along, fixed coordinate range, outward sign)
    def _side(self, side):
        x1, y1, x2, y2, T = self.x1, self.y1, self.x2, self.y2, self.T
        if side == "s":
            return "x", (y1 - T, y1), -1, (x1 - T, x2 + T), x1, x2
        if side == "n":
            return "x", (y2, y2 + T), 1, (x1 - T, x2 + T), x1, x2
        if side == "w":
            return "y", (x1 - T, x1), -1, (y1, y2), y1, y2
        return "y", (x2, x2 + T), 1, (y1, y2), y1, y2

    def _walls(self):
        st = self.st
        for side in "nsew":
            axis, (c1, c2), sign, (lo, hi), ilo, ihi = self._side(side)
            holes = []
            for dside, at, w, h in self.doors:
                if dside != side:
                    continue
                c = ilo + at * (ihi - ilo) if at <= 1 else ilo + at
                holes.append((c - w / 2, c + w / 2, self.floor_z, self.floor_z + h))
            for hside, at, w, sill, top in self.holes:
                if hside == side:
                    c = ilo + at * (ihi - ilo) if at <= 1 else ilo + at
                    holes.append((c - w / 2, c + w / 2, self.floor_z + sill, self.floor_z + top))
            if self.windows:
                ww, sill, wtop, gap = self.windows
                n = int((ihi - ilo - 64) // gap)
                for k in range(self.storeys):
                    zb = self.floor_z + k * self.storey_h
                    for i in range(n):
                        c = ilo + (ihi - ilo) * (i + 0.5) / n
                        a, b = c - ww / 2, c + ww / 2
                        if any(not (b + 24 <= h[0] or a - 24 >= h[1]) for h in holes if h[2] < zb + wtop and h[3] > zb + sill):
                            continue
                        holes.append((a, b, zb + sill, zb + wtop))
            zlo = self.z0 - 64
            band = self.floor_z + st.base_h if st.base else None
            pieces = _solid_pieces(lo, hi, zlo, self.top, holes)
            for a, b, z1, z2 in pieces:
                splits = [z1, z2]
                if band and z1 < band < z2:
                    splits = [z1, band, z2]
                for za, zb in zip(splits, splits[1:]):
                    outside = st.base if (band and zb <= band) else st.wall
                    out_face = "p" if sign > 0 else "n"
                    in_face = "n" if sign > 0 else "p"
                    if axis == "x":
                        faces = {out_face + "y": outside, in_face + "y": st.inner}
                        self.brushes.append(box6((a, c1, za), (b, c2, zb), st.trim, **faces))
                    else:
                        faces = {out_face + "x": outside, in_face + "x": st.inner}
                        self.brushes.append(box6((c1, a, za), (c2, b, zb), st.trim, **faces))

    def _floors(self, stair):
        st = self.st
        x1, y1, x2, y2 = self.x1, self.y1, self.x2, self.y2
        self.brushes.append(box6((x1, y1, self.z0 - 56), (x2, y2, self.floor_z), CAULK, pz=st.floor))
        for k in range(1, self.storeys):
            z = self.floor_z + k * self.storey_h
            # a stair up the west wall, in a hole in the floor over it
            run, rise, width = 24, 16, 56
            steps = int(math.ceil(self.storey_h / rise))
            sy1 = y1 + 16
            sy2 = sy1 + steps * run
            hole = (x1, sy1 - 8, x1 + width, sy2 + 8) if stair else None
            slab = []
            if hole:
                hx1, hy1, hx2, hy2 = hole
                slab = [(x1, y1, x2, hy1), (x1, hy2, x2, y2), (hx2, hy1, x2, hy2)]
            else:
                slab = [(x1, y1, x2, y2)]
            for a, b, c, d in slab:
                if c > a and d > b:
                    self.brushes.append(box6((a, b, z - 8), (c, d, z), st.trim, pz=st.floor, nz=st.ceil))
            if stair:
                zb = self.floor_z + (k - 1) * self.storey_h
                for i in range(steps):
                    top = min(zb + (i + 1) * rise, z)
                    self.brushes.append(box6((x1, sy1 + i * run, zb), (x1 + width, sy1 + (i + 1) * run, top), st.trim, pz=st.floor))

    def _flat(self, t, parapet):
        st, T = self.st, self.T
        x1, y1, x2, y2, z = self.x1, self.y1, self.x2, self.y2, self.top
        self.brushes.append(box6((x1 - T, y1 - T, z), (x2 + T, y2 + T, z + t), st.trim, pz=st.roof, nz=st.ceil))
        if parapet:
            zt = z + t + parapet
            for a, b in (((x1 - T, y1 - T), (x2 + T, y1)), ((x1 - T, y2), (x2 + T, y2 + T)),
                         ((x1 - T, y1), (x1, y2)), ((x2, y1), (x2 + T, y2))):
                self.brushes.append(box6((a[0], a[1], z + t), (b[0], b[1], zt), st.wall, pz=st.trim))

    def _gable(self, slope, ov, broken):
        st, T = self.st, self.T
        # the ridge along the long side
        swap = (self.y2 - self.y1) > (self.x2 - self.x1)
        f = Frame(swap)
        if swap:
            u1, u2, v1, v2 = self.y1, self.y2, self.x1, self.x2
        else:
            u1, u2, v1, v2 = self.x1, self.x2, self.y1, self.y2
        z = self.top
        vm = (v1 + v2) / 2
        rt = 10

        def under(v):
            # the roof's underside: at the wall top on the walls' outer faces
            return z + slope * (min(v - (v1 - T), (v2 + T) - v))

        ua, ub = u1 - T - ov, u2 + T + ov
        # each slope in pieces along the ridge, so that a broken roof can lose some
        n = max(1, int((ub - ua) // 192))
        for side in (-1, 1):
            ve = v1 - T - ov if side < 0 else v2 + T + ov
            for i in range(n):
                pa = ua + (ub - ua) * i / n
                pb = ua + (ub - ua) * (i + 1) / n
                if (side, i) in broken:
                    continue
                pts = []
                for uu in (pa, pb):
                    for vv in (ve, vm):
                        zz = under(vv)
                        pts.append(f.p(uu, vv, zz))
                        pts.append(f.p(uu, vv, zz + rt))
                # pts: 0 a-eave-under, 1 a-eave-top, 2 a-ridge-under, 3 a-ridge-top, 4..7 the same at b
                faces = [
                    ((1, 3, 7), st.roof),     # top
                    ((0, 2, 6), st.under),    # underside
                    ((0, 1, 5), st.trim),     # eave
                    ((2, 3, 7), CAULK),       # at the ridge
                    ((0, 1, 3), st.trim),     # ends
                    ((4, 5, 7), st.trim),
                ]
                self.brushes.append(hull(pts, faces))
        # gable ends: triangles from the wall top up under the roof
        top = under(vm)
        for ga, gb, outer in ((u1 - T, u1, "a"), (u2, u2 + T, "b")):
            pts = []
            for uu in (ga, gb):
                pts += [f.p(uu, v1 - T, z), f.p(uu, v2 + T, z), f.p(uu, vm, top)]
            out_i, in_i = ((0, 1, 2), (3, 4, 5)) if outer == "a" else ((3, 4, 5), (0, 1, 2))
            faces = [(out_i, st.gable), (in_i, st.inner), ((0, 1, 4), CAULK), ((0, 2, 5), CAULK), ((1, 2, 5), CAULK)]
            self.brushes.append(hull(pts, faces))

    def _lights(self, lamp, value, color):
        cx, cy = (self.x1 + self.x2) / 2, (self.y1 + self.y2) / 2
        for k in range(self.storeys):
            ceil = self.floor_z + (k + 1) * self.storey_h - (8 if k + 1 < self.storeys else 0)
            if lamp:
                self.m.ent("static_" + lamp.split("/")[-1][:-4].replace("-", "_"), model=lamp, origin=(cx, cy, ceil - 4))
            self.m.ent("light", origin=(cx, cy, ceil - 40), light=value * LIGHT_SCALE, _color=color)


def wall_run(m, pts, ground, height, t, top=None, thick=24, step=128, sink=40):
    """A low wall along a polyline, following the ground in `step` pieces."""
    out = []
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        L = math.hypot(bx - ax, by - ay)
        n = max(1, int(round(L / step)))
        for i in range(n):
            px1, py1 = ax + (bx - ax) * i / n, ay + (by - ay) * i / n
            px2, py2 = ax + (bx - ax) * (i + 1) / n, ay + (by - ay) * (i + 1) / n
            g = [ground(px1, py1), ground(px2, py2)]
            zb, zt = min(g) - sink, max(g) + height
            if abs(bx - ax) >= abs(by - ay):
                out.append(box6((min(px1, px2), min(py1, py2) - thick / 2, zb), (max(px1, px2), max(py1, py2) + thick / 2, zt), t, pz=top or t))
            else:
                out.append(box6((min(px1, px2) - thick / 2, min(py1, py2), zb), (max(px1, px2) + thick / 2, max(py1, py2), zt), t, pz=top or t))
    m.add(out)


# ---------------------------------------------------------------------------
# zones


class Zone:
    """One biome. `kinds` says what is on each side: 'mid' (the middle's edge,
    with a pass), 'ridge' or 'open' (the beach's sea)."""

    seed = 0
    amp = 60
    rough = 0.0
    kinds = None

    def __init__(self, band, row):
        self.band, self.row = band, row       # band -1 west, +1 east; row 0 south, 1 middle, 2 north
        self.sgn = band
        self.ylo, self.yhi = ROWS[row], ROWS[row + 1]
        self.passes = []                      # (side, along)
        self.pads = []                        # (x1, y1, x2, y2, z)
        self.roads = []                       # [(x, y), ...], width
        self.keep = []                        # (x, y, r): no trees here
        self.rnd = random.Random(1000 + self.seed)

    # u: distance out from the middle's edge
    def X(self, u):
        return self.sgn * (INNER + u)

    def rect(self, u1, y1, u2, y2):
        xa, xb = self.X(u1), self.X(u2)
        return min(xa, xb), y1, max(xa, xb), y2

    def side_dists(self, x, y):
        u = abs(x) - INNER
        return {"mid": u, "out": (OUTER - INNER) - u, "s": y - self.ylo, "n": self.yhi - y}

    def side_kind(self, side):
        if side == "mid":
            return "mid"
        if side == "out":
            return self.kinds.get("out", "ridge") if self.kinds else "ridge"
        return "ridge"

    def raw(self, x, y, noise):
        return noise * self.amp

    def tex(self, x, y, h, slope, patch):
        raise NotImplementedError

    def flat_pad(self, u1, y1, u2, y2, z=None, margin=320):
        x1, y1_, x2, y2_ = self.rect(u1, y1, u2, y2)
        self.pads.append((x1, y1_, x2, y2_, z, margin))
        self.keep.append(((x1 + x2) / 2, (y1_ + y2_) / 2, max(x2 - x1, y2_ - y1_) / 2 + 160))
        return x1, y1_, x2, y2_

    def on_road(self, x, y):
        for pts, w in self.roads:
            for (ax, ay), (bx, by) in zip(pts, pts[1:]):
                dx, dy = bx - ax, by - ay
                L2 = dx * dx + dy * dy or 1
                t = max(0, min(1, ((x - ax) * dx + (y - ay) * dy) / L2))
                if math.hypot(x - ax - t * dx, y - ay - t * dy) < w:
                    return True
        return False

    def plan(self):
        """Lay out roads, pads and passes before the ground is made."""

    def build(self, m, ground):
        """Put the buildings and props in, once the ground is made."""

    # helpers for build()
    def static(self, m, model, x, y, z, yaw=0, scale=None):
        cls = "static_" + model.split("/")[-1].replace(".tik", "").replace("-", "_")
        e = m.ent(cls, model=model, origin=(round(x), round(y), round(z)), angle=round(yaw) % 360)
        if scale:
            e.set("scale", round(scale, 2))
        return e

    def scatter(self, m, ground, models, count, u1, y1, u2, y2, scales=(0.8, 1.3), sink=6, avoid_roads=True, spacing=96):
        placed, tries, done = 0, 0, []
        while placed < count and tries < count * 40:
            tries += 1
            u = self.rnd.uniform(u1, u2)
            y = self.rnd.uniform(y1, y2)
            x = self.X(u)
            if any(math.hypot(x - kx, y - ky) < r for kx, ky, r in self.keep):
                continue
            if avoid_roads and self.on_road(x, y):
                continue
            if any(math.hypot(x - a, y - b) < spacing for a, b in done):
                continue
            model = models[placed % len(models)]
            self.static(m, model, x, y, ground(x, y) - sink, yaw=self.rnd.randrange(360), scale=self.rnd.uniform(*scales))
            done.append((x, y))
            placed += 1

    def line(self, m, ground, model, pts, spacing, yaw_off=90, sink=2, scale=None):
        """Models in a row along a polyline (sandbags, barbed wire): each one's
        length lies across its facing, so it faces the line's direction + 90."""
        for (ax, ay), (bx, by) in zip(pts, pts[1:]):
            L = math.hypot(bx - ax, by - ay)
            yaw = math.degrees(math.atan2(by - ay, bx - ax)) + yaw_off
            n = max(1, int(L // spacing))
            for i in range(n):
                t = (i + 0.5) / n
                x, y = ax + (bx - ax) * t, ay + (by - ay) * t
                self.static(m, model, x, y, ground(x, y) - sink, yaw=yaw, scale=scale)

    def P(self, u, y):
        return (self.X(u), y)


def _seg_dist(x, y, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    L2 = dx * dx + dy * dy or 1
    t = max(0, min(1, ((x - a[0]) * dx + (y - a[1]) * dy) / L2))
    return math.hypot(x - a[0] - t * dx, y - a[1] - t * dy)


# --- the zones' looks -------------------------------------------------------


class Norway(Zone):
    """m2l1 and m2l3: a Kriegsmarine research post in the snow. Grey
    concrete blocks with a weathered base course, snow on the flat roofs,
    corrugated sheds, bunkers, pines, sandbags and wire."""

    name = "norway"
    seed = 21
    amp = 70

    def tex(self, x, y, h, slope, patch):
        if slope > 1.0:
            return T("central_europe_winter/forstsnow_rock256")
        if self.on_road(x, y):
            return T("norway/road_nortraveled")
        return T("norway/norsnowmedium") if patch > 0.62 else T("norway/norsnow_lite256")

    def plan(self):
        self.passes = [("mid", 2880), ("s", -5824)]
        self.roads = [([self.P(0, 2880), self.P(1100, 2720), self.P(1800, 2300), self.P(2240, 1500), self.P(2240, 1152)], 72)]
        self.hq = self.flat_pad(1060, 1700, 1700, 2060)
        self.bunker = self.flat_pad(2560, 2560, 2816, 2816)
        self.shed = self.flat_pad(2500, 1700, 2900, 1956)
        self.hut = self.flat_pad(1300, 2850, 1620, 3090)

    def build(self, m, g):
        conc = T("norway/resrchnorway_wall1")
        st = Style(conc, inner=T("general_structure/plaster_wall2a"), floor=T("norway/nord_floortile"),
                   ceil=T("general_structure/jh_conc512b"), trim=T("general_structure/jh_conc512b"),
                   base=None, roof=T("norway/csnowconc"))
        x1, y1, x2, y2 = self.hq
        z = self._padz(self.hq)
        st.base = wall_tex("norway/resrchnorway_wall1low", z + 8)
        st.base_h = 128
        Building(m, x1, y1, x2, y2, z, st, storeys=2, storey_h=160, roof="flat", parapet=24,
                 doors=[("s", 0.5, 72, 112), ("n", 0.75, 72, 112)],
                 lamp="static/lightbulb_caged.tik", light=14, light_color=(0.9, 0.95, 1))
        cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
        self.static(m, "static/metaldesk.tik", cx + 120, cy, z + 8, yaw=90)
        self.static(m, "static/typewriter.tik", cx + 120, cy, z + 8 + 30, yaw=90)
        self.static(m, "static/bigfilecabinet.tik", x2 - 24, y2 - 24, z + 8, yaw=180)
        self.static(m, "static/bigfilecabinet.tik", x2 - 64, y2 - 24, z + 8, yaw=180)
        self.static(m, "furniture/simplechair.tik", cx + 80, cy, z + 8, yaw=0)

        bst = Style(T("general_structure/concretewall_winter_bunker"), inner=T("general_structure/jh_conc512b"),
                    floor=T("algiers/whsflrset1_1a"), ceil=T("general_structure/jh_conc512b"),
                    trim=T("general_structure/jh_conc512b"), roof=T("norway/csnowconc"))
        x1, y1, x2, y2 = self.bunker
        z = self._padz(self.bunker)
        Building(m, x1, y1, x2, y2, z, bst, storey_h=112, thick=32, roof="flat", roof_t=48,
                 doors=[("s", 0.5, 64, 96)], windows=None, holes=[("n", 0.5, 128, 56, 80), ("e", 0.5, 96, 56, 80), ("w", 0.5, 96, 56, 80)],
                 lamp="static/lightbulb_caged.tik", light=8)

        sst = Style(T("general_structure/jh_corrugate_3"), inner=T("general_structure/jh_corrugate_3"),
                    floor=T("general_structure/garagefloor"), trim=T("central_europe_winter/ibeam_vertwnter"),
                    roof=T("central_europe_winter/corrugated_snowcovered"), under=T("general_structure/jh_corrugate_3"),
                    gable=T("general_structure/jh_corrugate_3"))
        x1, y1, x2, y2 = self.shed
        z = self._padz(self.shed)
        Building(m, x1, y1, x2, y2, z, sst, storey_h=176, roof="gable", slope=0.3, windows=None,
                 doors=[("n", 0.5, 160, 144)], lamp="static/lightbulb_caged.tik", light=10)
        self.static(m, "static/nazi_crate.tik", x1 + 48, y1 + 48, z + 8, yaw=10)
        self.static(m, "static/nazi_crate.tik", x1 + 48, y1 + 100, z + 8, yaw=0)

        # a wooden hut with a snowy pitched roof, like the dock buildings
        hst = Style(T("norway/resrchnorway_wall1"), inner=T("central_europe/interiorwall_set2wtrim"),
                    floor=T("general_structure/plank_flat"), trim=T("central_europe_winter/joist_w"),
                    roof=T("central_europe_winter/winterroof_set1aw"), under=T("general_structure/plank_flat"))
        x1, y1, x2, y2 = self.hut
        z = self._padz(self.hut)
        Building(m, x1, y1, x2, y2, z, hst, storey_h=144, roof="gable", slope=0.7,
                 doors=[("s", 0.5, 64, 112)], lamp="static/lightbulb_caged.tik", light=8)
        self.static(m, "static/cot.tik", x1 + 40, (y1 + y2) / 2, z + 8, yaw=90)

        # sandbags round the bunker, wire along the far side
        bx1, by1, bx2, by2 = self.bunker
        self.line(m, g, "static/sandbag_link_main_winter.tik", [(bx1 - 120, by1 - 120), (bx2 + 120, by1 - 120)], 104)
        u_far = 2950
        self.line(m, g, "static/barbwire_long.tik", [self.P(u_far, 1650), self.P(u_far, 3050)], 432)
        self.scatter(m, g, ["static/tree_winter_midpine.tik"], 26, 900, 1600, 3100, 3136, scales=(0.7, 1.4), spacing=200)

    def _padz(self, r):
        return self.pad_z[r]


class Ardennes(Zone):
    """m6l1 and m6l2: the Siegfried forest in winter. Pines close
    together, rocks, a snowed-in bunker, and two half-timbered houses on a
    snowy street (the Communications Blackout's village)."""

    name = "ardennes"
    seed = 33
    amp = 110

    def tex(self, x, y, h, slope, patch):
        if slope > 0.9:
            return T("central_europe_winter/forstsnow_rock256")
        if self.on_road(x, y):
            return T("central_europe_winter/stset_1awinter")
        if patch > 0.66:
            return T("central_europe_winter/snow_bumpy_1")
        return T("central_europe_winter/forstsnow_rough256") if patch < 0.3 else T("central_europe_winter/forstsnow_lite256")

    def plan(self):
        self.passes = [("mid", 250), ("n", -5824), ("s", -6400)]
        self.roads = [([self.P(0, 250), self.P(900, 200), self.P(1500, 0), self.P(2240, 0), self.P(2816, -400), self.P(2816, -1152)], 72),
                      ([self.P(2240, 0), self.P(2240, 1152)], 64)]
        self.h1 = self.flat_pad(1240, 210, 1720, 530)
        self.h2 = self.flat_pad(1900, -500, 2300, -180)
        self.bunker = self.flat_pad(3000, 280, 3320, 560)

    def build(self, m, g):
        for r, broken in ((self.h1, ()), (self.h2, ((1, 1),))):
            x1, y1, x2, y2 = r
            z = self.pad_z[r]
            st = Style(T("central_europe_winter/tudor_set1w_exwall1flatw"), inner=T("central_europe/interiorwall_set2wtrim"),
                       floor=T("interior/manor_woodfloor"), ceil=T("general_structure/plank_flat"),
                       trim=T("central_europe_winter/joist_w"), base=wall_tex("central_europe_winter/stonebricks1drk_wntr", z + 8),
                       base_h=48, roof=T("central_europe_winter/winterroof_set1w"), under=T("general_structure/plank_flat"),
                       gable=T("central_europe_winter/tudor_set2bw"))
            Building(m, x1, y1, x2, y2, z, st, storeys=2, storey_h=144, roof="gable", slope=0.75, broken=broken,
                     doors=[("s" if r is self.h1 else "n", 0.6, 64, 112)], lamp="lights/hang5lamp.tik", light=10)
            self.static(m, "furniture/armchairposh.tik", x2 - 60, y2 - 60, z + 8, yaw=225)
            self.static(m, "static/lightpost_sidemounted_winter.tik", (x1 + x2) / 2, y1 - 18 if r is self.h1 else y2 + 18, z + 120,
                        yaw=270 if r is self.h1 else 90)
        x1, y1, x2, y2 = self.bunker
        z = self.pad_z[self.bunker]
        st = Style(T("central_europe_winter/winterbunkerwall"), inner=T("general_structure/bunker_wall"),
                   floor=T("algiers/whsflrset1_1b"), ceil=T("general_structure/jh_conc512b"),
                   trim=T("general_structure/bunker_wall"), roof=T("norway/csnowconc"))
        Building(m, x1, y1, x2, y2, z, st, storey_h=112, thick=40, roof="flat", roof_t=56,
                 doors=[("s", 0.3, 64, 96)], windows=None, holes=[("n", 0.5, 160, 56, 80)],
                 lamp="static/lightbulb_caged.tik", light=8)
        self.line(m, g, "static/sandbag_link_main_winter.tik", [(x1 - 100, y1 - 110), (x2 + 100, y1 - 110)], 104)
        # the forest: the retail maps' mix, thin trunks the most
        pines = ["static/tree_winter_thintrunk.tik", "static/tree_winter_smallpine.tik", "static/tree_winter_fillpine.tik",
                 "static/tree_winter_thintrunk.tik", "static/tree_winter_thicktrunk.tik", "static/tree_winter_tallpine.tik",
                 "static/tree_winter_midpine.tik"]
        self.scatter(m, g, pines, 110, 800, -1000, 3900, 1000, scales=(0.8, 1.3), spacing=110)
        self.scatter(m, g, ["static/rock_winter_medium.tik", "static/rock_winter_large.tik", "static/bush_winter_denseshrub.tik"],
                     22, 800, -1000, 3900, 1000, scales=(0.7, 1.2), sink=12, spacing=160)


class Bocage(Zone):
    """m3l2, m3l3 and the m4 maps: Normandy farmland. Fields fenced by
    hedgerows on earth banks, a dirt lane, a stone farmhouse with a shingle
    roof, a plank barn and a small stone church."""

    name = "bocage"
    seed = 45
    amp = 64

    def tex(self, x, y, h, slope, patch):
        if slope > 0.95:
            return T("wilderness/wldrrckset1_1")
        if self.on_road(x, y):
            return T("misc_outside/bocroad_set1b")
        return T("wilderness/m3l3grass_1") if patch > 0.6 else T("wilderness/m3l3grass_1rough")

    def plan(self):
        self.passes = [("mid", -2400), ("n", -6400)]
        self.roads = [([self.P(0, -2400), self.P(800, -2350), self.P(1500, -2100), self.P(2000, -2100), self.P(2816, -1700), self.P(2816, -1152)], 64)]
        self.farm = self.flat_pad(1300, -2700, 1748, -2444)
        self.barn = self.flat_pad(1960, -2800, 2344, -2480)
        self.church = self.flat_pad(2600, -2600, 3112, -2344)

    def build(self, m, g):
        x1, y1, x2, y2 = self.farm
        z = self.pad_z[self.farm]
        st = Style(T("brest/brest_wal3a"), inner=T("central_europe/interiorwall_set2chrrl"), floor=T("interior/tiledfloor_manordark2"),
                   ceil=T("general_structure/beam_wood1"), trim=T("general_structure/beam_wood1"),
                   roof=T("general_structure/jh_woodshingles1a"), under=T("general_structure/plank_flat"),
                   gable=T("brest/brest_wal3a"))
        Building(m, x1, y1, x2, y2, z, st, storeys=2, storey_h=144, roof="gable", slope=0.7,
                 doors=[("n", 0.4, 64, 112)], lamp="lights/hang5lamp.tik", light=10)
        cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
        self.static(m, "furniture/table.tik", cx + 60, cy, z + 8)
        self.static(m, "furniture/simplechair.tik", cx + 60, cy - 40, z + 8, yaw=90)
        self.static(m, "static/bigbed.tik", x2 - 60, y1 + 50, z + 8 + 144, yaw=180)

        x1, y1, x2, y2 = self.barn
        z = self.pad_z[self.barn]
        st = Style(T("general_structure/plank_flat"), inner=T("general_structure/plank_flat"), floor=T("misc_outside/bocroad_set1b"),
                   trim=T("general_structure/beam_wood1"), base=wall_tex("misc_outside/fieldstone_wall", z + 8), base_h=40,
                   roof=T("general_structure/shingleornsml1"), under=T("general_structure/plank_flat"))
        Building(m, x1, y1, x2, y2, z, st, storey_h=192, roof="gable", slope=0.8, windows=None,
                 doors=[("n", 0.5, 128, 152), ("s", 0.5, 64, 112)], lamp="static/lightbulb_caged.tik", light=8)

        x1, y1, x2, y2 = self.church
        z = self.pad_z[self.church]
        st = Style(T("general_structure/stonebricks1drk"), inner=T("interior/hearth_set1basef"), floor=T("algiers/fort_floor_cobbleflttile"),
                   trim=T("general_structure/stonebricks1drk"), roof=T("general_structure/shingleornsml1"),
                   under=T("general_structure/beam_wood1"))
        Building(m, x1, y1, x2, y2, z, st, storey_h=288, roof="gable", slope=0.75, windows=(32, 112, 240, 112),
                 doors=[("w" if self.sgn > 0 else "e", 0.5, 80, 144)], lamp="lights/hang5lamp.tik", light=12)
        for k in range(5):
            px = x1 + 120 + k * 64 if self.sgn > 0 else x2 - 120 - k * 64
            for py in (y1 + 70, y2 - 70):
                self.static(m, "static/churchpew.tik", px, py, z + 8, yaw=0 if self.sgn > 0 else 180)

        # hedgerows: earth banks with bushes along them, fields between
        bank = T("misc_outside/fencerow_stone1")
        grass = T("wilderness/m3l3grass_1rough")
        rows = [
            [self.P(900, -1800), self.P(2400, -1800)],
            [self.P(3200, -1800), self.P(3800, -1800)],
            [self.P(1100, -3050), self.P(1100, -2900)],
            [self.P(3500, -3100), self.P(3500, -2000)],
            [self.P(900, -2950), self.P(1100, -2950)],
        ]
        for pts in rows:
            wall_run(m, pts, g, 56, bank, top=grass, thick=64)
            self.line(m, g, "static/bush_regularbush.tik", pts, 80, sink=-50, scale=1.2)
        self.scatter(m, g, ["static/tree_oak.tik", "static/bush_full.tik", "static/tree_juniper.tik", "static/bush_regularbush.tik"],
                     30, 900, -3136, 3950, -1600, scales=(0.8, 1.3), spacing=180)


class Desert(Zone):
    """m1l1 to m1l3: a North African town (Arzew, Algiers). Flat-roofed
    plaster houses with parapets and mosaic bands, a two-storey house, a
    corrugated motor pool shed, palms, baskets and caged lamps."""

    name = "desert"
    seed = 57
    amp = 48

    def tex(self, x, y, h, slope, patch):
        if slope > 0.9:
            return T("algiers/cliffset_1")
        if self.on_road(x, y):
            return T("algiers/stset_2sand")
        return T("algiers/grndset_1a") if patch > 0.6 else T("algiers/grndset_1")

    def plan(self):
        self.passes = [("mid", 2300), ("s", 5824)]
        self.roads = [([self.P(0, 2300), self.P(900, 2300), self.P(2240, 2300), self.P(2240, 1152)], 96)]
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
        trims = ["algiers/algierwall1b_mozaic_bl", "algiers/algierwall1b_mozaic_red", "algiers/afrik_wall1brick", "algiers/algierwall1b_mozaic_bl"]
        for k, r in enumerate(self.houses + [self.big]):
            x1, y1, x2, y2 = r
            z = self.pad_z[r]
            big = r is self.big
            st = Style(T(walls[k % 4]), inner=T("algiers/interiorwall_afrika1flat"), floor=T("algiers/whsflrset1_1b"),
                       ceil=T("algiers/algier_ciel5"), trim=T(trims[k % 4]), roof=T("algiers/fort_roofwhite"),
                       base=wall_tex(trims[k % 4], z + 8), base_h=24)
            door_side = "s" if y1 > 2300 else "n"
            Building(m, x1, y1, x2, y2, z, st, storeys=2 if big else 1, storey_h=152, roof="flat", parapet=32,
                     doors=[(door_side, 0.5, 64, 112)], windows=(40, 56, 112, 200),
                     lamp="static/lightbulb_desertcage.tik", light=10, light_color=(1, 0.85, 0.6))
            self.static(m, "static/basket1.tik", x1 + 32, y1 + 32, z + 8, yaw=k * 40)
            if k % 2:
                self.static(m, "static/square_table.tik", (x1 + x2) / 2, (y1 + y2) / 2, z + 8)
        x1, y1, x2, y2 = self.shed
        z = self.pad_z[self.shed]
        st = Style(T("general_structure/jh_corrugate4a"), inner=T("general_structure/jh_corrugate4"), floor=T("general_industrial/basecrete_seem_pit2"),
                   trim=T("general_industrial/verticalbrace"), roof=T("general_structure/jh_corrugate4"),
                   under=T("general_structure/jh_corrugate4"), gable=T("general_structure/jh_corrugate4a"))
        Building(m, x1, y1, x2, y2, z, st, storey_h=208, roof="gable", slope=0.3, windows=None,
                 doors=[("n", 0.5, 256, 176)], lamp="static/lightbulb_covered_nowire.tik", light=12)
        for i in range(4):
            self.static(m, "static/88shell.tik", x1 + 60 + i * 20, y1 + 60, z + 8, yaw=90)
        self.static(m, "static/nazi_crate.tik", x2 - 60, y1 + 60, z + 8, yaw=15)
        self.static(m, "static/nazi_crate.tik", x2 - 60, y1 + 110, z + 8, yaw=0)
        self.scatter(m, g, ["static/tree_regularpalm.tik", "static/tree_squatpalm.tik", "static/bush_full.tik"], 24,
                     900, 1600, 3700, 3136, scales=(1.0, 1.5), spacing=200)


class Beach(Zone):
    """m3l1: Omaha. A grassy bluff with a concrete casemate and a pillbox
    looking down on sand and sea, the beach full of hedgehogs, mined poles,
    ramps and wire."""

    name = "beach"
    seed = 69
    amp = 30
    kinds = {"out": "open"}

    BLUFF = 1800      # where the bluff starts falling
    SAND = 2500       # where the sand starts
    SHORE = 3350      # about where the sea starts

    def raw(self, x, y, noise):
        u = abs(x) - INNER
        bluff = 144 + noise * 40
        beach = -12 - (u - self.SAND) * 0.02 + noise * 8
        if u < self.BLUFF:
            return bluff
        if u < self.SAND:
            t = s((u - self.BLUFF) / (self.SAND - self.BLUFF))
            return bluff * (1 - t) + beach * t
        if u < self.SHORE:
            return beach
        return max(beach - (u - self.SHORE) * 0.3, -260)

    def tex(self, x, y, h, slope, patch):
        u = abs(x) - INNER
        if slope > 1.1:
            return T("misc_outside/rockwall2")
        if u < self.BLUFF + 200 and h > 60:
            return T("wilderness/m3l3grass_1rough") if patch > 0.4 else T("misc_outside/trench_mud")
        return T("test/omaha_pjspick5")

    def plan(self):
        self.passes = [("mid", 250)]
        self.case = self.flat_pad(1300, -240, 1620, 180)
        self.pill = self.flat_pad(1200, 600, 1392, 792)
        self.pill2 = self.flat_pad(1240, -760, 1432, -568)

    def build(self, m, g):
        x1, y1, x2, y2 = self.case
        z = self.pad_z[self.case]
        st = Style(T("blasted/dday_bunker_wall1bsml"), inner=T("normandy/bunker_conc2"), floor=T("algiers/whsflrset1_1a"),
                   ceil=T("normandy/bunk_ceiling"), trim=T("normandy/bunker_conc2"), roof=T("blasted/dday_bunker_wall1crkdsml"))
        # the embrasure looks out to sea, the door in from the land
        sea, land = ("e", "w") if self.sgn > 0 else ("w", "e")
        # the gun slit looks out to sea, the door in from the land
        Building(m, x1, y1, x2, y2, z, st, storey_h=128, thick=48, roof="flat", roof_t=64, windows=None,
                 doors=[(land, 0.5, 64, 104)], holes=[(sea, 0.5, 224, 56, 84)], lamp="static/lightbulb_caged.tik", light=8)
        for r in (self.pill, self.pill2):
            x1, y1, x2, y2 = r
            z = self.pad_z[r]
            Building(m, x1, y1, x2, y2, z, st, storey_h=104, thick=40, roof="flat", roof_t=48, windows=None,
                     doors=[(land, 0.5, 56, 96)], holes=[(sea, 0.5, 112, 48, 72), ("n", 0.5, 64, 48, 72), ("s", 0.5, 64, 48, 72)],
                     lamp=None, light=5)
        # a trench line along the bluff top: sandbags
        top = self.BLUFF - 120
        self.line(m, g, "static/sandbag_link_main.tik", [self.P(top, -1000), self.P(top, -300)], 104)
        self.line(m, g, "static/sandbag_link_main.tik", [self.P(top, 300), self.P(top, 1000)], 104)
        # the beach obstacles, in rows along the shore
        for row, (model, spacing) in enumerate((("static/hedgehog.tik", 200), ("static/minepole.tik", 240),
                                                ("static/ramp_tripleleg.tik", 320), ("static/hedgehog.tik", 220))):
            u = self.SAND + 160 + row * 190
            for y in range(-900, 901, spacing):
                yy = y + self.rnd.uniform(-40, 40) + (spacing / 2 if row % 2 else 0)
                if abs(yy) > 960:
                    continue
                x = self.X(u + self.rnd.uniform(-30, 30))
                self.static(m, model, x, yy, g(x, yy) - 4, yaw=self.rnd.randrange(360))
        self.line(m, g, "static/barbwire_long.tik", [self.P(self.SAND - 60, -960), self.P(self.SAND - 60, 960)], 432)
        for y in (-700, -200, 400, 850):
            x = self.X(self.SAND + 40)
            self.static(m, "static/barbwire_tetra_post.tik", x, y, g(x, y) - 2, yaw=self.rnd.randrange(360))
        self.scatter(m, g, ["static/bush_full.tik", "static/bush_sparseshrub.tik"], 10, 900, -1000, self.BLUFF - 200, 1000, spacing=200)

    def sea(self, m):
        xa, xb = sorted((self.X(self.SHORE - 200), self.X(OUTER - INNER)))
        m.add(box6((xa, self.ylo, GROUND_BOTTOM + 32), (xb, self.yhi, WATER_Z), T("misc_outside/deepbluesea"), detail=False))


class Town(Zone):
    """m5l1 to m5l3 and m4l0: a small European town. Half-timbered and
    plastered houses with tile and shingle roofs along a cobbled street,
    one of them shelled open, a bigger plastered house, field stone walls."""

    name = "town"
    seed = 81
    amp = 36

    def tex(self, x, y, h, slope, patch):
        if slope > 0.95:
            return T("misc_outside/rockwall2")
        if self.on_road(x, y):
            return T("central_europe/small_cobble")
        return T("brest/mud_set1") if patch > 0.58 else T("wilderness/m3l3grass_1rough")

    def plan(self):
        self.passes = [("mid", -2400), ("n", 6400)]
        self.roads = [([self.P(0, -2400), self.P(900, -2400), self.P(3200, -2400)], 112),
                      ([self.P(2816, -2400), self.P(2816, -1152)], 80)]
        self.row_n = []
        self.row_s = []
        u = 1000
        for w in (320, 384, 288, 352):
            self.row_n.append(self.flat_pad(u, -2200, u + w, -1900))
            self.row_s.append(self.flat_pad(u + 40, -2900, u + 40 + w, -2600))
            u += w + 160
        self.hall = self.flat_pad(3150, -3000, 3700, -2600)

    def build(self, m, g):
        roofs = ["misc_outside/tileroof1", "general_structure/rotshingle1"]
        walls = ["central_europe/remgn_wallexter1", "general_structure/plaster_wall2a", "central_europe/tudor_set1w_exwall1flat",
                 "central_europe/nrmndywlset_2a"]
        for k, r in enumerate(self.row_n + self.row_s):
            x1, y1, x2, y2 = r
            z = self.pad_z[r]
            north = k < len(self.row_n)
            st = Style(T(walls[k % 4]), inner=T("central_europe/interiorwall_set3chrrl"), floor=T("interior/manor_woodfloor"),
                       ceil=T("general_structure/plank_flat"), trim=T("general_structure/beam_wood1"),
                       base=wall_tex("misc_outside/fieldstone_wall", z + 8), base_h=40,
                       roof=T(roofs[k % 2]), under=T("general_structure/plank_flat"))
            broken = ((-1 if north else 1, 1), (-1 if north else 1, 2)) if k == 2 else ()
            Building(m, x1, y1, x2, y2, z, st, storeys=2, storey_h=144, roof="gable", slope=0.7, broken=broken,
                         doors=[("s" if north else "n", 0.5, 64, 112)], lamp="lights/hang5lamp.tik", light=10)
            cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
            up = z + 8 + 144
            self.static(m, "static/bigbed.tik", x2 - 60, cy, up, yaw=180)
            self.static(m, ["static/dresser.tik", "static/wardrobe_damaged.tik", "static/rolltop_desk.tik"][k % 3],
                        cx, y2 - 30 if north else y1 + 30, z + 8, yaw=270 if north else 90)
        x1, y1, x2, y2 = self.hall
        z = self.pad_z[self.hall]
        st = Style(T("general_structure/facade1"), inner=T("central_europe/interiorwall_set2chrrl"), floor=T("interior/tiledfloor_manordark2"),
                   ceil=T("general_structure/plaster_wall2"), trim=T("general_structure/plaster_wall2"),
                   base=wall_tex("general_structure/stonebricks1drk", z + 8), base_h=56,
                   roof=T("misc_outside/tileroof1"), under=T("general_structure/beam_wood1"))
        Building(m, x1, y1, x2, y2, z, st, storeys=2, storey_h=176, roof="gable", slope=0.55,
                 doors=[("n", 0.5, 96, 136)], windows=(56, 56, 144, 160), lamp="lights/hang5lamp.tik", light=12)
        self.static(m, "static/rolltop_desk.tik", x2 - 50, (y1 + y2) / 2, z + 8, yaw=180)
        # field stone walls round the gardens behind the houses
        wall = T("misc_outside/fieldstone_wall")
        wall_run(m, [self.P(950, -1720), self.P(2900, -1720)], g, 48, wall)
        wall_run(m, [self.P(980, -3060), self.P(3000, -3060)], g, 48, wall)
        self.scatter(m, g, ["static/tree_commontree.tik", "static/bush_full.tik", "static/tree_juniper.tik"], 16,
                     900, -3136, 3900, -1600, scales=(0.8, 1.2), spacing=200)


ZONES = [
    (-1, 2, Norway), (-1, 1, Ardennes), (-1, 0, Bocage),
    (1, 2, Desert), (1, 1, Beach), (1, 0, Town),
]


# ---------------------------------------------------------------------------
# the ground


class Band:
    """The ground on one side of the middle: a heightfield over the band's
    three zones."""

    def __init__(self, band, zones):
        self.band = band
        self.zones = zones
        self.x0 = INNER if band > 0 else -OUTER
        self.nx = (OUTER - INNER) // STEP + 1
        self.ny = (2 * YEDGE) // STEP + 1
        self.noise = {z: smooth_noise(self.nx, self.ny, seed=z.seed, octaves=3) for z in zones}
        self.patch = {z: smooth_noise(self.nx, self.ny, seed=z.seed + 5, octaves=2) for z in zones}

    def zone_at(self, y):
        for z in self.zones:
            if z.ylo <= y <= z.yhi:
                return z
        return self.zones[0]

    def point(self, i, j):
        return self.x0 + i * STEP, -YEDGE + j * STEP

    def height(self, i, j):
        x, y = self.point(i, j)
        z = self.zone_at(y)
        raw = z.raw(x, y, self.noise[z][j][i])
        d = z.side_dists(x, y)
        fade, ridge = 1.0, 0.0
        # the other ridges come down to nothing where the band meets the
        # middle, which is flat there
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
                    ridge = max(ridge, EDGE_H * s(1 - abs(dist - RIDGE_W) / RIDGE_W) * keep)
            else:
                fade = min(fade, s(dist / RIDGE_W))
                if dist < RIDGE_W:
                    ridge = max(ridge, RIDGE_H * s(1 - dist / RIDGE_W) * keep * near_mid)
        h = raw * fade + ridge
        for (px1, py1, px2, py2, pz, margin) in z.pads:
            dx = max(px1 - x, 0, x - px2)
            dy = max(py1 - y, 0, y - py2)
            w = 1 - s((math.hypot(dx, dy) - 96) / margin)
            if w > 0:
                h = h * (1 - w) + z.pad_z[(px1, py1, px2, py2)] * w
        return max(h, GROUND_BOTTOM + 96)

    def make(self):
        # pads first: each one flat at the height the ground had at its middle
        for z in self.zones:
            z.pad_z = {}
            for (px1, py1, px2, py2, pz, margin) in z.pads:
                if pz is None:
                    i = int(round(((px1 + px2) / 2 - self.x0) / STEP))
                    j = int(round(((py1 + py2) / 2 + YEDGE) / STEP))
                    saved, z.pads = z.pads, []
                    pz = round(self.height(i, j))
                    z.pads = saved
                z.pad_z[(px1, py1, px2, py2)] = pz
        self.hs = [[round(self.height(i, j)) for i in range(self.nx)] for j in range(self.ny)]

    def at(self, x, y):
        fx = (x - self.x0) / STEP
        fy = (y + YEDGE) / STEP
        i = max(0, min(int(fx), self.nx - 2))
        j = max(0, min(int(fy), self.ny - 2))
        tx, ty = fx - i, fy - j
        a = self.hs[j][i] * (1 - tx) + self.hs[j][i + 1] * tx
        b = self.hs[j + 1][i] * (1 - tx) + self.hs[j + 1][i + 1] * tx
        return a * (1 - ty) + b * ty

    def tex_for(self, i, j):
        hs = self.hs
        c = [hs[j][i], hs[j][i + 1], hs[j + 1][i], hs[j + 1][i + 1]]
        slope = (max(c) - min(c)) / STEP
        x, y = self.point(i, j)
        x += STEP / 2
        y += STEP / 2
        z = self.zone_at(y)
        return z.tex(x, y, sum(c) / 4, slope, self.patch[z][j][i])

    def vis_walls(self, m):
        """Caulk walls inside the ridges, so vis doesn't see through them at
        ground level. They stop where a pass goes through."""
        top = RIDGE_H - 96
        out = []
        xa, xb = sorted((self.band * INNER, self.band * OUTER))

        def cut(lo, hi, passes):
            spans = [(lo, hi)]
            for p in passes:
                a, b = p - PASS_HALF - PASS_RAMP, p + PASS_HALF + PASS_RAMP
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

        # between zones, along y = ROWS[1], ROWS[2]
        if self.band > 0:
            xa = INNER + 2 * RIDGE_W
        else:
            xb = -INNER - 2 * RIDGE_W
        for z in self.zones:
            if z.row == 0:
                continue
            passes = [p for side, p in z.passes if side == "s"]
            for c, d in cut(xa, xb, passes):
                out.append(box6((c, z.ylo - 8, GROUND_BOTTOM), (d, z.ylo + 8, top), CAULK, detail=False))
        # along the middle's edge, at the crest of its ridge
        cx = self.band * (INNER + RIDGE_W)
        for z in self.zones:
            passes = [p for side, p in z.passes if side == "mid"]
            for c, d in cut(z.ylo, z.yhi, passes):
                out.append(box6((cx - 8, c, GROUND_BOTTOM), (cx + 8, d, EDGE_H - 96), CAULK, detail=False))
        m.add(out)


def build_biomes(m):
    """Adds the six zones to the map. Returns {zone name: (x, y, z)} of a
    spot just inside each one's pass from the middle."""
    spots = {}
    for band in (-1, 1):
        zones = [cls(b, row) for b, row, cls in ZONES if b == band]
        # the passes between zones go both ways
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
        m.add(heightfield(b.x0, -YEDGE, STEP, b.hs, b.tex_for, GROUND_BOTTOM))
        b.vis_walls(m)
        for z in zones:
            z.build(m, b.at)
            if hasattr(z, "sea"):
                z.sea(m)
            mid = [at for side, at in z.passes if side == "mid"][0]
            x = z.X(RIDGE_W * 2 + 64)
            spots[z.name] = (x, mid, round(b.at(x, mid)) + 24)
    return spots
