"""The tech demo's building kit: textures at the retail levels' size and lined
up with what they are on, brushes, decals, buildings with glazed windows and
working doors, and rooms furnished from the models' own bounds.

Textures: every texture is drawn at the size the retail levels draw it
(texscale.py, measured from their BSPs). On a wall its tiles start at the
building's corner and its floor, and a wall takes a whole number of tiles
(stretched a little to fit), so a timber frame or a base course lines up with
the storeys and the corners. Faces of the .map project their texture along
the axis nearest their normal (Q3's QuakeTextureVecs): s runs along +x (or
+y on faces facing x), t down -z (or -y on floors). So a tile's left edge is
where s is 0 and its bottom where t is.

Furniture: the bounds of each model (propbounds.py, read from its SKD) let a
room stand it against a wall, its back to the wall, or put a lamp on a table;
nothing goes in front of a window, a door's swing or the stairs, and nothing
overlaps. Models face +x at yaw 0.
"""

import math
import random

import propbounds
import texscale
from mapgen import Brush, Tex

CAULK = "common/caulk"
NODRAW = "common/nodraw"      # nonsolid and not drawn: the hidden sides of decals
ORIGIN = "common/origin"
LIGHT_SCALE = 15


# ---------------------------------------------------------------------------
# textures


def T(shader, sx=None, sy=None, u0=0.0, v0=0.0, length=None, height=None, rot=0, mult=1.0, density=None):
    """A texture at its retail size. u0, v0: where a tile starts along the
    face (s) and up it (t: a tile's bottom at v0 on a wall, its top edge at
    v0 on a floor). length, height: stretch the scale (by up to a fifth) so a
    whole number of tiles fills that much."""
    rsx, rsy, w, h = texscale.scale(shader)
    sx = (sx or rsx) * mult
    sy = (sy or rsy) * mult
    if length:
        sx = _fit(length, w, sx)
    if height:
        sy = _fit(height, h, sy)
    xoff = (-u0 / sx) % w
    yoff = (v0 / sy) % h
    return Tex(shader, xoff=round(xoff, 2), yoff=round(yoff, 2), rot=rot, sx=round(sx, 4), sy=round(sy, 4), density=density)


# lightmap texel sizes (surfaceDensity): the open ground and the outsides of
# buildings coarser than rooms, or the map's lightmaps overflow (MOHlight's
# 256 of 128 by 128 with the patched MOHlight, build.py); the retail levels
# use 64 on much of theirs
GROUND_DENSITY = 32
ROCK_DENSITY = 48
OUTSIDE_DENSITY = 24


def _fit(span, pixels, scale):
    tile = pixels * scale
    n = max(1, round(span / tile))
    fitted = span / (n * pixels)
    return fitted if abs(fitted / scale - 1) <= 0.2 else scale


# the size of pictures a shader's first stage doesn't show (windowframe1's
# first stage is its glass's reflection)
IMAGE_SIZE = {"general_structure/windowframe1": (128, 256)}


def fitted(shader, u0, v0, w, h, rot=0):
    """A texture laid once over a w by h rectangle whose left is at u0 and
    bottom at v0 (a door, a pane, a decal)."""
    _, _, pw, ph = texscale.scale(shader)
    if shader in IMAGE_SIZE:
        pw, ph = IMAGE_SIZE[shader]
    elif pw == 256 and ph == 256 and shader not in texscale.table():
        pw, ph = _image_size(shader)
    sx, sy = w / pw, h / ph
    return Tex(shader, xoff=round((-u0 / sx) % pw, 2), yoff=round(((v0 + h) / sy) % ph, 2), rot=rot, sx=round(sx, 5), sy=round(sy, 5))


_sizes = {}


def _image_size(shader):
    """The size of a texture the retail levels never use (a decal, a door):
    read from its image, through the shader's first map."""
    if shader in _sizes:
        return _sizes[shader]
    import io
    import os
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lamps"))
    import lamps
    from PIL import Image
    path = lamps.shader_image("textures/" + shader) or ("textures/" + shader)
    size = (256, 256)
    base = path.rsplit(".", 1)[0] if path.lower().endswith((".tga", ".jpg")) else path
    for ext in (".tga", ".jpg"):
        data = lamps.read(base + ext)
        if data:
            size = Image.open(io.BytesIO(data)).size
            break
    _sizes[shader] = size
    return size


def tex_rot(shader, cx, cy, angle, sx, sy, w, h):
    """A floor texture turned by angle (degrees) with the image's middle at
    (cx, cy): for decals laid on the ground (splats, a road's strip)."""
    a = math.radians(angle)
    xoff = w / 2 - (cx * math.cos(a) + cy * math.sin(a)) / sx
    yoff = h / 2 - (cx * math.sin(a) - cy * math.cos(a)) / sy
    return Tex(shader, xoff=round(xoff % w, 2), yoff=round(yoff % h, 2), rot=round(angle, 3), sx=round(sx, 5), sy=round(sy, 5))


# ---------------------------------------------------------------------------
# brushes


def box6(mins, maxs, t, detail=True, **faces):
    """An axis-aligned box with a texture per face: px nx py ny pz nz override t."""
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
    face, texture); the winding is worked out from the corners' middle."""
    n = len(verts)
    c = tuple(sum(v[k] for v in verts) / n for k in range(3))
    return Brush([((verts[a], verts[b], verts[d]), t) for (a, b, d), t in faces], c, detail)


def overlay_tri(tri, t, lift=1.0, depth=6):
    """A decal laid on the ground: a thin prism whose top is the triangle
    lifted a little, drawn with t; its other faces are not drawn and not solid."""
    p = [(x, y, z + lift) for x, y, z in tri]
    zb = min(q[2] for q in p) - depth
    cx = sum(q[0] for q in p) / 3
    cy = sum(q[1] for q in p) / 3
    faces = [((p[0], p[1], p[2]), t), (((p[0][0], p[0][1], zb), (p[1][0], p[1][1], zb), (p[2][0], p[2][1], zb)), NODRAW)]
    for a, b in ((p[0], p[1]), (p[1], p[2]), (p[2], p[0])):
        faces.append((((a[0], a[1], zb), (b[0], b[1], zb), (b[0], b[1], zb + 1)), NODRAW))
    return Brush(faces, (cx, cy, (min(q[2] for q in p) + zb) / 2), True)


# walls: which way a side faces, and which coordinate runs along it
SIDES = {
    "n": ((0, 1), "x"), "s": ((0, -1), "x"),
    "e": ((1, 0), "y"), "w": ((-1, 0), "y"),
}
YAW_IN = {"w": 0, "e": 180, "s": 90, "n": 270}  # facing into a room from that wall


def wall_decal(shader, side, plane, a, b, z1, z2, off=0.5):
    """A decal on a wall: a thin brush just off the wall's face (plane: the
    face's coordinate; side: the way the face looks), the image fitted to it."""
    (nx, ny), axis = SIDES[side]
    t = fitted(shader, a, z1, b - a, z2 - z1)
    if axis == "x":
        y0 = plane + ny * off
        y1 = y0 + ny * 1
        return box6((a, min(y0, y1), z1), (b, max(y0, y1), z2), NODRAW, **{("py" if ny > 0 else "ny"): t})
    x0 = plane + nx * off
    x1 = x0 + nx * 1
    return box6((min(x0, x1), a, z1), (max(x0, x1), b, z2), NODRAW, **{("px" if nx > 0 else "nx"): t})


# ---------------------------------------------------------------------------
# models


def static(m, model, x, y, z, yaw=0, scale=None):
    cls = "static_" + model.split("/")[-1].replace(".tik", "").replace("-", "_")
    e = m.ent(cls, model=model, origin=(round(x, 1), round(y, 1), round(z, 1)), angle=round(yaw) % 360)
    if scale and abs(scale - 1) > 0.01:
        e.set("scale", round(scale, 2))
    return e


def footprint(model, x, y, yaw, scale=1.0):
    """(x1, y1, x2, y2, height) of a model stood at x y turned to yaw (a
    multiple of 90), or None when its bounds aren't known."""
    b = propbounds.bounds(model)
    if not b:
        return None
    (ax, ay, az), (bx, by, bz) = b
    c, s = round(math.cos(math.radians(yaw))), round(math.sin(math.radians(yaw)))
    xs, ys = [], []
    for px, py in ((ax, ay), (ax, by), (bx, ay), (bx, by)):
        xs.append(x + (px * c - py * s) * scale)
        ys.append(y + (px * s + py * c) * scale)
    return min(xs), min(ys), max(xs), max(ys), bz * scale


def overlap(r, q, margin=0):
    return not (r[2] + margin <= q[0] or q[2] + margin <= r[0] or r[3] + margin <= q[1] or q[3] + margin <= r[1])


# ---------------------------------------------------------------------------
# rooms


class Room:
    """A room's floor, its openings, and what is in it so far."""

    def __init__(self, m, x1, y1, x2, y2, z, h, rnd):
        self.m = m
        self.x1, self.y1, self.x2, self.y2, self.z, self.h = x1, y1, x2, y2, z, h
        self.rnd = rnd
        self.used = []        # footprints (x1, y1, x2, y2)
        self.openings = {k: [] for k in "nsew"}   # (a, b, zlo, zhi) along each wall
        self.tables = []      # (x1, y1, x2, y2, top z)

    def block(self, x1, y1, x2, y2):
        self.used.append((x1, y1, x2, y2))

    def _fits(self, fp, side=None, tall_from=None, margin=6):
        x1, y1, x2, y2, h = fp
        if x1 < self.x1 + 1 or y1 < self.y1 + 1 or x2 > self.x2 - 1 or y2 > self.y2 - 1:
            return False
        if any(overlap((x1, y1, x2, y2), u, margin) for u in self.used):
            return False
        if side:
            axis = SIDES[side][1]
            a, b = (x1, x2) if axis == "x" else (y1, y2)
            for oa, ob, zlo, zhi in self.openings[side]:
                if self.z + h > zlo - 2 and not (b + 8 <= oa or ob + 8 <= a):
                    return False
        return True

    def against(self, model, side=None, along=None, gap=1.0, z=None, tries=40, scale=1.0, hang=False):
        """Stands a model with its back to a wall (any wall, or side), facing
        into the room. hang: it hangs on the wall at height z (a picture)
        and only has to keep clear of openings and what stands there."""
        b = propbounds.bounds(model)
        if not b:
            return None
        sides = [side] if side else self.rnd.sample("nsew", 4)
        for _ in range(tries):
            sd = sides[_ % len(sides)]
            yaw = YAW_IN[sd]
            back = b[0][0] * scale
            (nx, ny), axis = SIDES[sd]
            # the inner face of that wall
            if sd == "w":
                px = self.x1 + gap - back
                lo, hi = self.y1, self.y2
            elif sd == "e":
                px = self.x2 - gap + back
                lo, hi = self.y1, self.y2
            elif sd == "s":
                py = self.y1 + gap - back
                lo, hi = self.x1, self.x2
            else:
                py = self.y2 - gap + back
                lo, hi = self.x1, self.x2
            t = along if along is not None and _ == 0 else self.rnd.uniform(lo + 24, hi - 24)
            if axis == "y":
                x, y = px, t
            else:
                x, y = t, py
            fp = footprint(model, x, y, yaw, scale)
            zz = self.z if z is None else z
            if hang:
                if not self._hang_fits(fp, sd, zz, zz + b[0][2] * scale):
                    continue
                static(self.m, model, x, y, zz, yaw, scale)
                self.hung.append((sd, fp, zz))
                return fp
            if not self._fits(fp, sd):
                continue
            static(self.m, model, x, y, zz, yaw, scale)
            self.used.append(fp[:4])
            self.standing.append((sd, fp))
            return fp
        return None

    standing = None
    hung = None

    def _hang_fits(self, fp, side, z, zlo=None):
        x1, y1, x2, y2, h = fp
        zlo = z if zlo is None else zlo
        axis = SIDES[side][1]
        a, b = (x1, x2) if axis == "x" else (y1, y2)
        lo, hi = (self.x1, self.x2) if axis == "x" else (self.y1, self.y2)
        if a < lo + 8 or b > hi - 8:
            return False
        for oa, ob, zlo, zhi in self.openings[side]:
            if not (b + 6 <= oa or ob + 6 <= a) and z < zhi + 4 and z + h > zlo - 4:
                return False
        for sd, f in self.standing:
            if sd == side:
                fa, fb = (f[0], f[2]) if axis == "x" else (f[1], f[3])
                if not (b + 4 <= fa or fb + 4 <= a) and self.z + f[4] > zlo - 6:
                    return False
        for sd, f, zz in self.hung:
            if sd == side:
                fa, fb = (f[0], f[2]) if axis == "x" else (f[1], f[3])
                if not (b + 16 <= fa or fb + 16 <= a):
                    return False
        return True

    def free(self, model, yaw=None, near=None, tries=60, scale=1.0, margin=12):
        """Stands a model somewhere on the floor clear of everything, near a
        point if given (a table in the middle)."""
        for k in range(tries):
            yw = yaw if yaw is not None else self.rnd.choice((0, 90, 180, 270))
            if near and k < tries // 2:
                x = near[0] + self.rnd.uniform(-1, 1) * k * 4
                y = near[1] + self.rnd.uniform(-1, 1) * k * 4
            else:
                x = self.rnd.uniform(self.x1 + 40, self.x2 - 40)
                y = self.rnd.uniform(self.y1 + 40, self.y2 - 40)
            fp = footprint(model, x, y, yw, scale)
            if fp and self._fits(fp, margin=margin):
                static(self.m, model, x, y, self.z, yw, scale)
                self.used.append(fp[:4])
                return (x, y, yw, fp)
        return None

    def table_with(self, table, chairs=(), on=(), near=None, yaw=None):
        """A table, chairs round it facing it, things on it."""
        got = self.free(table, yaw=yaw, near=near or ((self.x1 + self.x2) / 2, (self.y1 + self.y2) / 2), margin=56)
        if not got:
            return None
        x, y, yw, fp = got
        top = self.z + fp[4]
        self.tables.append((fp[0], fp[1], fp[2], fp[3], top))
        # chairs: at the table's sides, facing it, tucked a little under
        sides = [((fp[0] - 10, y), 0), ((fp[2] + 10, y), 180), ((x, fp[1] - 10), 90), ((x, fp[3] + 10), 270)]
        self.rnd.shuffle(sides)
        for chair, ((cx, cy), cyaw) in zip(chairs, sides):
            b = propbounds.bounds(chair)
            if not b:
                continue
            # the chair's front edge (its +x) at the table's edge
            d = b[1][0]
            ox, oy = cx - math.cos(math.radians(cyaw)) * d, cy - math.sin(math.radians(cyaw)) * d
            cfp = footprint(chair, ox, oy, cyaw)
            if cfp and self._fits(cfp, margin=2):
                static(self.m, chair, ox, oy, self.z, cyaw)
                self.used.append(cfp[:4])
        self.on_table(len(self.tables) - 1, on)
        return got

    def on_table(self, k, things):
        x1, y1, x2, y2, top = self.tables[k]
        put = []
        for thing in things:
            b = propbounds.bounds(thing)
            if not b:
                continue
            for _ in range(20):
                yaw = self.rnd.choice((0, 90, 180, 270))
                x = self.rnd.uniform(x1 + 6, x2 - 6)
                y = self.rnd.uniform(y1 + 6, y2 - 6)
                fp = footprint(thing, x, y, yaw)
                if fp[0] < x1 + 2 or fp[2] > x2 - 2 or fp[1] < y1 + 2 or fp[3] > y2 - 2:
                    continue
                if any(overlap(fp[:4], p, 2) for p in put):
                    continue
                static(self.m, thing, x, y, top, yaw)
                put.append(fp[:4])
                break

    def on_top_of(self, fp, things):
        """Things on a piece of furniture just stood (a lamp on a night stand)."""
        self.tables.append((fp[0], fp[1], fp[2], fp[3], self.z + fp[4]))
        self.on_table(len(self.tables) - 1, things)


# ---------------------------------------------------------------------------
# buildings


class Style:
    """What a building is made of. wall/inner/floor/ceil/trim/roof/under/gable
    are shader names; base is a base course's shader and base_h its height;
    storey_tile: the outside wall's tile is a storey high (timber frames)."""

    def __init__(self, wall, inner=None, floor=None, ceil=None, trim=None, base=None, base_h=0,
                 roof=None, under=None, gable=None, storey_tile=False, sill=None, door="mohcommon/doorold_1a",
                 door_edge="general_structure/interior_door1side", pane="general_structure/windowframe1",
                 window_decal=None, grille=None, icicles=False, inner_storey_tile=False):
        self.wall = wall
        self.inner = inner or wall
        self.floor = floor or wall
        self.ceil = ceil or self.inner
        self.trim = trim or wall
        self.base = base
        self.base_h = base_h
        self.roof = roof or wall
        self.under = under or self.ceil
        self.gable = gable or wall
        self.storey_tile = storey_tile
        self.inner_storey_tile = inner_storey_tile
        self.sill = sill or self.trim
        self.door = door
        self.door_edge = door_edge
        self.pane = pane
        self.window_decal = window_decal
        self.grille = grille
        self.icicles = icicles


def _pieces(lo, hi, zlo, zhi, holes):
    """A wall from lo to hi along it and zlo to zhi up, less its holes
    (a1, a2, z1, z2): the solid pieces (a1, a2, z1, z2)."""
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
        out += [(a, b, z1, z2) for z1, z2 in spans if z2 > z1]
    return out


class Building:
    """A building on a pad. Walls with doors and windows cut in them: each
    door a working door (func_rotatingdoor) in a frame, each window a pane of
    glass (func_window) with a sill. Storeys joined by a stair; a flat roof
    (with a parapet) or a gable roof (with a chimney); a room per storey,
    or two with a partition, furnished by `rooms`.

    doors: (side, at, width, height[, kind]): at is the middle along the
    side from its west/south end, a fraction (<= 1) or units; kind 'door'
    (the default: a working door), 'open' (a doorway), 'gate' (a wide one).
    windows: (width, height, sill, spacing) for every storey, or None.
    slits: (side, at, width, sill, top): openings without glass (bunkers).
    rooms: per storey, a room kind or (kind, kind) for two rooms."""

    def __init__(self, zone, x1, y1, x2, y2, z0, style, storeys=1, storey_h=160, thick=16,
                 roof="gable", slope=0.6, overhang=24, parapet=0, roof_t=16,
                 doors=(), windows=(48, 96, 36, 176), slits=(), rooms=None, broken=(),
                 lamp=None, light=12, light_color=(1, 0.9, 0.75), chimney=True, partition=None, glass=True,
                 decals=(), attic=True):
        self.zone = zone
        self.m = m = zone.m
        self.rnd = random.Random(hash((round(x1), round(y1))) & 0xffff)
        self.x1, self.y1, self.x2, self.y2, self.z0 = x1, y1, x2, y2, z0
        self.st = style
        self.T = thick
        self.floor_z = z0 + 8
        self.storeys = storeys
        self.storey_h = storey_h
        self.top = self.floor_z + storeys * storey_h
        self.glass = glass
        self.brushes = []
        self.door_list = []     # (side, a, b, z1, z2, kind)
        self.window_list = []   # (side, a, b, z1, z2, glazed)
        self._openings(doors, windows, slits)
        self.stair = None
        self._walls()
        self._floors()
        if roof == "gable":
            self._gable(slope, overhang, broken, chimney)
            if attic:
                # a ceiling under the roof (the lamp hangs from it)
                self.brushes.append(box6((x1, y1, self.top - 8), (x2, y2, self.top), T(style.trim, u0=x1, v0=self.top),
                                         nz=T(style.ceil, u0=x1, v0=y2), pz=T(style.under, u0=x1, v0=y2)))
            self.open_roof = not attic
        else:
            self.open_roof = False
            self._flat(roof_t, parapet)
        m.add(self.brushes)
        self._doors()
        self._windows()
        for d in decals:
            self.decal(*d)
        self.rooms = self._rooms(rooms or [], partition, lamp, light, light_color)

    # -- layout -------------------------------------------------------------

    def side_span(self, side):
        """(wall coordinate range across, along lo, along hi, inner lo, inner hi, outer face, inner face)"""
        x1, y1, x2, y2, T = self.x1, self.y1, self.x2, self.y2, self.T
        if side == "s":
            return (y1 - T, y1), x1 - T, x2 + T, x1, x2, y1 - T, y1
        if side == "n":
            return (y2, y2 + T), x1 - T, x2 + T, x1, x2, y2 + T, y2
        if side == "w":
            return (x1 - T, x1), y1, y2, y1, y2, x1 - T, x1
        return (x2, x2 + T), y1, y2, y1, y2, x2 + T, x2

    def _at(self, side, at):
        _, _, _, ilo, ihi, _, _ = self.side_span(side)
        return ilo + at * (ihi - ilo) if at <= 1 else ilo + at

    def _openings(self, doors, windows, slits):
        for d in doors:
            side, at, w, h = d[:4]
            kind = d[4] if len(d) > 4 else "door"
            c = self._at(side, at)
            self.door_list.append((side, c - w / 2, c + w / 2, self.floor_z, self.floor_z + h, kind))
        for side, at, w, sill, top in slits:
            c = self._at(side, at)
            self.window_list.append((side, c - w / 2, c + w / 2, self.floor_z + sill, self.floor_z + top, False))
        if windows:
            ww, wh, sill, gap = windows
            for side in "nsew":
                _, _, _, ilo, ihi, _, _ = self.side_span(side)
                n = int((ihi - ilo - 48) // gap)
                for k in range(self.storeys):
                    zb = self.floor_z + k * self.storey_h
                    for i in range(n):
                        c = ilo + (ihi - ilo) * (i + 0.5) / n
                        a, b = c - ww / 2, c + ww / 2
                        clash = False
                        for o in self.door_list + self.window_list:
                            if o[0] == side and not (b + 32 <= o[1] or a - 32 >= o[2]) and o[3] < zb + sill + wh and o[4] > zb + sill:
                                clash = True
                        # the stair runs up the west wall's first stretch
                        if side == "w" and self.storeys > 1 and a < self.y1 + 300:
                            clash = True
                        if not clash:
                            self.window_list.append((side, a, b, zb + sill, zb + sill + wh, True))

    def _walls(self):
        st = self.st
        for side in "nsew":
            (c1, c2), lo, hi, ilo, ihi, outer, inner = self.side_span(side)
            holes = [(o[1], o[2], o[3], o[4]) for o in self.door_list + self.window_list if o[0] == side]
            zlo = self.z0 - 64
            band = self.floor_z + st.base_h if st.base else None
            out_len = hi - lo
            sh = self.storey_h if st.storey_tile else None
            out_t = T(st.wall, u0=lo, v0=self.floor_z, length=out_len, height=sh, density=OUTSIDE_DENSITY)
            in_t = T(st.inner, u0=ilo, v0=self.floor_z, length=ihi - ilo, height=self.storey_h if st.inner_storey_tile else None)
            base_t = T(st.base, u0=lo, v0=self.floor_z, length=out_len, density=OUTSIDE_DENSITY) if st.base else None
            for a, b, z1, z2 in _pieces(lo, hi, zlo, self.top, holes):
                splits = [z1, z2]
                if band and z1 < band < z2:
                    splits = [z1, band, z2]
                for za, zb in zip(splits, splits[1:]):
                    outside = base_t if (band and zb <= band) else out_t
                    rev = T(st.trim, u0=a, v0=za)
                    sign = 1 if outer > inner else -1
                    o_face = ("p" if sign > 0 else "n") + ("y" if side in "ns" else "x")
                    i_face = ("n" if sign > 0 else "p") + ("y" if side in "ns" else "x")
                    faces = {o_face: outside, i_face: in_t}
                    # faces nobody sees take no lightmap: the bottom in the
                    # ground, and where a base course meets the wall above
                    if za == zlo or (band and za == band):
                        faces["nz"] = CAULK
                    if band and zb == band:
                        faces["pz"] = CAULK
                    if side in "ns":
                        self.brushes.append(box6((a, c1, za), (b, c2, zb), rev, **faces))
                    else:
                        self.brushes.append(box6((c1, a, za), (c2, b, zb), rev, **faces))

    def _floors(self):
        st = self.st
        x1, y1, x2, y2 = self.x1, self.y1, self.x2, self.y2
        ft = T(st.floor, u0=x1, v0=y2)
        ct = T(st.ceil, u0=x1, v0=y2)
        self.brushes.append(box6((x1, y1, self.z0 - 56), (x2, y2, self.floor_z), CAULK, pz=ft))
        if self.storeys < 2:
            return
        run, rise, width = 24, 16, 56
        steps = int(math.ceil(self.storey_h / rise))
        sy1 = y1 + 24
        sy2 = sy1 + steps * run
        self.stair = (x1, sy1 - 8, x1 + width, sy2 + 8)
        for k in range(1, self.storeys):
            z = self.floor_z + k * self.storey_h
            hx1, hy1, hx2, hy2 = self.stair
            for a, b, c, d in ((x1, y1, x2, hy1), (x1, hy2, x2, y2), (hx2, hy1, x2, hy2)):
                if c > a and d > b:
                    self.brushes.append(box6((a, b, z - 8), (c, d, z), T(st.trim, u0=a, v0=z), pz=ft, nz=ct))
            # a rail round the stairwell's open side upstairs
            self.brushes.append(box6((hx2, hy1, z), (hx2 + 4, hy2, z + 36), T(st.trim, u0=hy1, v0=z)))
            zb = z - self.storey_h
            tread = T(st.trim, u0=x1, v0=sy2)
            for i in range(steps):
                top = min(zb + (i + 1) * rise, z)
                self.brushes.append(box6((x1, sy1 + i * run, zb), (x1 + width, sy1 + (i + 1) * run, top),
                                         T(st.trim, u0=sy1, v0=zb), pz=tread))

    def _flat(self, t, parapet):
        st, T_ = self.st, self.T
        x1, y1, x2, y2, z = self.x1, self.y1, self.x2, self.y2, self.top
        self.brushes.append(box6((x1 - T_, y1 - T_, z), (x2 + T_, y2 + T_, z + t), T(st.trim, u0=x1, v0=z),
                                 pz=T(st.roof, u0=x1, v0=y2, density=OUTSIDE_DENSITY), nz=T(st.ceil, u0=x1, v0=y2)))
        if parapet:
            zt = z + t + parapet
            cap = T(st.sill, u0=x1, v0=y2)
            for a, b in (((x1 - T_, y1 - T_), (x2 + T_, y1)), ((x1 - T_, y2), (x2 + T_, y2 + T_)),
                         ((x1 - T_, y1), (x1, y2)), ((x2, y1), (x2 + T_, y2))):
                self.brushes.append(box6((a[0], a[1], z + t), (b[0], b[1], zt), T(st.wall, u0=a[0] if b[0] - a[0] > b[1] - a[1] else a[1], v0=self.floor_z), pz=cap))
            # a coping that stands proud of the wall
            for a, b in (((x1 - T_ - 3, y1 - T_ - 3), (x2 + T_ + 3, y1 - T_ + 4)), ((x1 - T_ - 3, y2 + T_ - 4), (x2 + T_ + 3, y2 + T_ + 3)),
                         ((x1 - T_ - 3, y1 - T_ + 4), (x1 - T_ + 4, y2 + T_ - 4)), ((x2 + T_ - 4, y1 - T_ + 4), (x2 + T_ + 3, y2 + T_ - 4))):
                self.brushes.append(box6((a[0], a[1], zt), (b[0], b[1], zt + 4), T(st.sill, u0=a[0], v0=zt)))

    def _gable(self, slope, ov, broken, chimney):
        st, T_ = self.st, self.T
        swap = (self.y2 - self.y1) > (self.x2 - self.x1)

        def P(u, v, z):
            return (v, u, z) if swap else (u, v, z)

        if swap:
            u1, u2, v1, v2 = self.y1, self.y2, self.x1, self.x2
        else:
            u1, u2, v1, v2 = self.x1, self.x2, self.y1, self.y2
        z = self.top
        vm = (v1 + v2) / 2
        rt = 10

        def under(v):
            return z + slope * min(v - (v1 - T_), (v2 + T_) - v)

        ua, ub = u1 - T_ - ov, u2 + T_ + ov
        roof_t = T(st.roof, u0=self.x1 - T_ - ov, v0=self.y2 + T_ + ov, rot=90 if swap else 0, density=OUTSIDE_DENSITY)
        under_t = T(st.under, u0=ua, v0=0)
        n = max(1, int((ub - ua) // 192))
        for side in (-1, 1):
            ve = v1 - T_ - ov if side < 0 else v2 + T_ + ov
            for i in range(n):
                pa = ua + (ub - ua) * i / n
                pb = ua + (ub - ua) * (i + 1) / n
                if (side, i) in broken:
                    continue
                pts = []
                for uu in (pa, pb):
                    for vv in (ve, vm):
                        zz = under(vv)
                        pts.append(P(uu, vv, zz))
                        pts.append(P(uu, vv, zz + rt))
                trim = T(st.trim, u0=pa, v0=z)
                faces = [((1, 3, 7), roof_t), ((0, 2, 6), under_t), ((0, 1, 5), trim), ((2, 3, 7), CAULK),
                         ((0, 1, 3), trim), ((4, 5, 7), trim)]
                self.brushes.append(hull(pts, faces))
        top = under(vm)
        gt = T(st.gable, u0=v1 - T_, v0=self.floor_z, height=self.storey_h if st.storey_tile else None, density=OUTSIDE_DENSITY)
        gi = T(st.inner, u0=v1, v0=self.floor_z)
        for ga, gb, outer in ((u1 - T_, u1, "a"), (u2, u2 + T_, "b")):
            pts = []
            for uu in (ga, gb):
                pts += [P(uu, v1 - T_, z), P(uu, v2 + T_, z), P(uu, vm, top)]
            out_i, in_i = ((0, 1, 2), (3, 4, 5)) if outer == "a" else ((3, 4, 5), (0, 1, 2))
            self.brushes.append(hull(pts, [(out_i, gt), (in_i, gi), ((0, 1, 4), CAULK), ((0, 2, 5), CAULK), ((1, 2, 5), CAULK)]))
        if chimney:
            # a brick chimney through the roof near one gable, up past the ridge
            cu = u2 - 48 if (self.rnd.random() < 0.5) else u1 + 24
            cv = vm + (20 if self.rnd.random() < 0.5 else -44)
            zb = under(cv) - 8
            zt = top + rt + 40
            brick = "algiers/jh_brick3"
            a, b = P(cu, cv, zb), P(cu + 24, cv + 24, zt)
            mn = (min(a[0], b[0]), min(a[1], b[1]), zb)
            mx = (max(a[0], b[0]), max(a[1], b[1]), zt)
            self.brushes.append(box6(mn, mx, T(brick, u0=mn[0], v0=zb), pz=T("general_structure/jh_conc512b")))
            self.brushes.append(box6((mn[0] - 2, mn[1] - 2, zt), (mx[0] + 2, mx[1] + 2, zt + 4), T("general_structure/jh_conc512b")))
        if st.icicles:
            # icicles under both eaves
            for side in (-1, 1):
                ve = v1 - T_ - ov if side < 0 else v2 + T_ + ov
                ze = under(ve)
                for i in range(int((ub - ua) // 96)):
                    pa = ua + 8 + i * 96 + self.rnd.uniform(0, 20)
                    w = self.rnd.uniform(48, 80)
                    if pa + w > ub - 4:
                        break
                    a = P(pa, ve + side * 0.5, ze - 40)
                    b = P(pa + w, ve + side * 1.5, ze)
                    mn = (min(a[0], b[0]), min(a[1], b[1]), ze - 40)
                    mx = (max(a[0], b[0]), max(a[1], b[1]), ze - 1)
                    face = ("p" if side > 0 else "n") + ("x" if swap else "y")
                    t = fitted("central_europe_winter/icecicles", mn[1] if swap else mn[0], ze - 40, w, 39)
                    self.brushes.append(box6(mn, mx, NODRAW, **{face: t}))

    # -- doors and windows ----------------------------------------------------

    def _doors(self):
        st = self.st
        for side, a, b, z1, z2, kind in self.door_list:
            (c1, c2), _, _, _, _, outer, inner = self.side_span(side)
            mid = (c1 + c2) / 2
            self._frame(side, a, b, z1, z2, outer, inner, sill=False)
            if kind != "door":
                continue
            w, h = b - a, z2 - z1
            face_t = fitted(st.door, a, z1, w, h)
            edge = T(st.door_edge)
            e = self.m.ent("func_rotatingdoor", wait=-1, angle=0 if side in "ew" else 90)
            if side in "ns":
                e.brushes.append(box6((a + 1, mid - 2, z1 + 1), (b - 1, mid + 2, z2 - 1), edge, py=face_t, ny=face_t, detail=False))
                e.brushes.append(box6((a - 3, mid - 3, (z1 + z2) / 2 - 3), (a + 3, mid + 3, (z1 + z2) / 2 + 3), ORIGIN, detail=False))
            else:
                e.brushes.append(box6((mid - 2, a + 1, z1 + 1), (mid + 2, b - 1, z2 - 1), edge, px=face_t, nx=face_t, detail=False))
                e.brushes.append(box6((mid - 3, a - 3, (z1 + z2) / 2 - 3), (mid + 3, a + 3, (z1 + z2) / 2 + 3), ORIGIN, detail=False))

    def _frame(self, side, a, b, z1, z2, outer, inner, sill=True, w=6, depth=2):
        """Boards round an opening, standing proud of the wall on both faces,
        and the opening's reveals in the trim."""
        st = self.st
        trim = st.trim
        out = []
        for face, sign in ((outer, 1 if outer > inner else -1), (inner, 1 if inner > outer else -1)):
            f1, f2 = sorted((face, face + sign * depth))
            pieces = [(a - w, a, z1, z2 + w), (b, b + w, z1, z2 + w), (a, b, z2, z2 + w)]
            if sill and face == outer:
                pieces.append((a - w - 4, b + w + 4, z1 - 4, z1))
            for pa, pb, pz1, pz2 in pieces:
                t = T(trim, u0=pa, v0=pz1)
                if side in "ns":
                    out.append(box6((pa, f1, pz1), (pb, f2, pz2), t))
                else:
                    out.append(box6((f1, pa, pz1), (f2, pb, pz2), t))
        # the outside sill sticks out further, to stand a window's line of shadow
        if sill:
            f1, f2 = sorted((outer, outer + (1 if outer > inner else -1) * 6))
            t = T(st.sill, u0=a, v0=z1)
            if side in "ns":
                out.append(box6((a - 8, f1, z1 - 6), (b + 8, f2, z1 - 2), t))
            else:
                out.append(box6((f1, a - 8, z1 - 6), (f2, b + 8, z1 - 2), t))
        self.m.add(out)

    def _windows(self):
        st = self.st
        for side, a, b, z1, z2, glazed in self.window_list:
            (c1, c2), _, _, _, _, outer, inner = self.side_span(side)
            mid = (c1 + c2) / 2
            if not glazed:
                continue
            self._frame(side, a, b, z1, z2, outer, inner, sill=True, w=4)
            if self.glass:
                pane = fitted(st.pane, a, z1, b - a, z2 - z1)
                e = self.m.ent("func_window", spawnflags=1, health=50)
                if side in "ns":
                    e.brushes.append(box6((a, mid - 1, z1), (b, mid + 1, z2), NODRAW, py=pane, ny=pane, detail=False))
                else:
                    e.brushes.append(box6((mid - 1, a, z1), (mid + 1, b, z2), NODRAW, px=pane, nx=pane, detail=False))
            if st.grille:
                # wrought iron over the window, outside
                (nx, ny), _ = SIDES[side]
                self.m.add(wall_decal(st.grille, side, outer, a - 4, b + 4, z1 - 4, z2 + 4, off=1.5))

    def decal(self, shader, side, at, z, w, h, inside=False):
        """A decal (a poster, a sign) on a wall, outside (or in)."""
        (c1, c2), _, _, _, _, outer, inner = self.side_span(side)
        c = self._at(side, at)
        face = inner if inside else outer
        look = side if not inside else {"n": "s", "s": "n", "e": "w", "w": "e"}[side]
        self.m.add(wall_decal(shader, look, face, c - w / 2, c + w / 2, self.floor_z + z, self.floor_z + z + h))

    # -- rooms ----------------------------------------------------------------

    def _rooms(self, kinds, partition, lamp, light, color):
        rooms = []
        for k in range(self.storeys):
            z = self.floor_z + k * self.storey_h
            h = self.storey_h - (8 if (k + 1 < self.storeys or not self.open_roof) else 0)
            kind = kinds[k] if k < len(kinds) else None
            parts = [(self.x1, self.y1, self.x2, self.y2)]
            if isinstance(kind, tuple) and partition:
                parts = self._partition(z, h)
            kinds_here = kind if isinstance(kind, tuple) else (kind,)
            for (rx1, ry1, rx2, ry2), rk in zip(parts, kinds_here + (kinds_here[-1],) * 2):
                r = Room(self.m, rx1, ry1, rx2, ry2, z, h, self.rnd)
                r.standing, r.hung = [], []
                self._mark(r, k)
                if lamp:
                    static(self.m, lamp, (rx1 + rx2) / 2, (ry1 + ry2) / 2, z + h - lamp_drop(lamp))
                area = (rx2 - rx1) * (ry2 - ry1)
                self.m.ent("light", origin=((rx1 + rx2) / 2, (ry1 + ry2) / 2, z + h - 44),
                           light=round(max(13, min(28, light * 1.6 * area / 60000)) * LIGHT_SCALE), _color=color)
                if rk and rk in ROOM_KITS:
                    ROOM_KITS[rk](r)
                rooms.append(r)
        return rooms

    def _partition(self, z, h):
        """Two rooms: a wall across the long axis, a doorway in it."""
        st = self.st
        x1, y1, x2, y2 = self.x1, self.y1, self.x2, self.y2
        it = T(st.inner, v0=z)
        if (x2 - x1) >= (y2 - y1):
            xm = round(x1 + (x2 - x1) * 0.55)
            dy = y1 + (y2 - y1) * 0.7
            for a, b, za, zb in _pieces(y1, y2, z, z + h, [(dy - 28, dy + 28, z, z + 104)]):
                self.brushes2([box6((xm - 4, a, za), (xm + 4, b, zb), it)])
            self._door_in(("w", xm, dy - 28, dy + 28, z))
            return [(x1, y1, xm - 4, y2), (xm + 4, y1, x2, y2)]
        ym = round(y1 + (y2 - y1) * 0.55)
        dx = x1 + (x2 - x1) * 0.7
        for a, b, za, zb in _pieces(x1, x2, z, z + h, [(dx - 28, dx + 28, z, z + 104)]):
            self.brushes2([box6((a, ym - 4, za), (b, ym + 4, zb), it)])
        self._door_in(("s", ym, dx - 28, dx + 28, z))
        return [(x1, y1, x2, ym - 4), (x1, ym + 4, x2, y2)]

    def brushes2(self, bs):
        self.m.add(bs)

    _inner_doors = None

    def _door_in(self, d):
        if self._inner_doors is None:
            self._inner_doors = []
        self._inner_doors.append(d)

    def _mark(self, r, storey):
        """Tells a room what it mustn't block: its doors' swing, windows, the stair."""
        z = r.z
        for side, a, b, z1, z2, kind in self.door_list:
            if abs(z1 - z) > 4:
                continue
            w = b - a
            if side in "ns":
                y = r.y1 if side == "s" else r.y2
                r.block(a - 8, y - w - 16, b + 8, y + w + 16)
            else:
                x = r.x1 if side == "w" else r.x2
                r.block(x - w - 16, a - 8, x + w + 16, b + 8)
            r.openings[side].append((a, b, z1, z2))
        for side, a, b, z1, z2, glazed in self.window_list:
            if z <= z1 < z + r.h:
                r.openings[side].append((a, b, z1, z2))
        for d in self._inner_doors or []:
            side, at, a, b, dz = d
            if abs(dz - z) > 4:
                continue
            if side == "w":
                r.block(at - 72, a - 8, at + 72, b + 8)
                for sd in "ew":
                    r.openings[sd].append((a, b, dz, dz + 104))
            else:
                r.block(a - 8, at - 72, b + 8, at + 72)
                for sd in "ns":
                    r.openings[sd].append((a, b, dz, dz + 104))
        if self.stair:
            hx1, hy1, hx2, hy2 = self.stair
            # the stair, and room to step on and off it
            r.block(hx1, hy1 - 48, hx2 + 40, hy2 + 56)
            r.openings["w"].append((hy1 - 48, hy2 + 56, z, z + r.h))


def lamp_drop(model):
    b = propbounds.bounds(model)
    return (b[1][2] if b else 8) + 2


# ---------------------------------------------------------------------------
# what goes in a room, by kind


def _maybe(r, p):
    return r.rnd.random() < p


def kit_living(r):
    r.table_with("furniture/table.tik", ["furniture/woodchair.tik", "furniture/simplechair.tik", "furniture/woodchair.tik"],
                 ["static/pitcher.tik", "static/bottle_large.tik", "static/whiteplate.tik", "static/whiteplate.tik"])
    for m in ("static/dresser.tik", "static/bookcase.tik", "static/grandfathers_clock.tik", "static/halfcouch.tik"):
        fp = r.against(m)
        if fp and m == "static/dresser.tik":
            r.on_top_of(fp, ["static/jug.tik", "miscobj/radio_civilian.tik"])
    if _maybe(r, 0.6):
        r.against("static/fireplaceset.tik")
    r.against("furniture/armchairposh.tik")
    for p in ("static/flowerpainting.tik", "static/oldladypainting.tik", "static/static_calendar.tik"):
        r.against(p, gap=0.5, z=r.z + 60, hang=True)


def kit_bedroom(r):
    fp = r.against(r.rnd.choice(["static/bigbed.tik", "static/single_bed.tik"]))
    ns = r.against("static/night_stand.tik")
    if ns:
        r.on_top_of(ns, ["lights/tablelamp.tik"])
    for m in ("static/wardrobe.tik", "static/vanity.tik", "static/hopechest.tik", "static/coatrack.tik"):
        r.against(m)
    r.against("static/widepainting.tik", gap=0.5, z=r.z + 64, hang=True)


def kit_office(r):
    d = r.against(r.rnd.choice(["static/metaldesk.tik", "static/large_desk.tik"]))
    if d:
        r.on_top_of(d, ["static/typewriter.tik", "miscobj/phone_squarebase.tik", "miscobj/book_closed_thick_lavender.tik"])
    for m in ("static/bigfilecabinet.tik", "static/bigfilecabinet.tik", "static/smallfilecabinet.tik",
              "static/bookcase.tik", "static/static_radiostation1.tik"):
        r.against(m)
    r.table_with("static/square_table.tik", ["furniture/bunkerchair.tik", "furniture/bunkerchair.tik"],
                 ["miscobj/radio_military.tik", "static/metal_mug.tik"])
    r.against("static/static_calendar.tik", gap=0.5, z=r.z + 60, hang=True)
    r.against("static/static_clock1.tik", gap=0.5, z=r.z + 96, hang=True)


def kit_barracks(r):
    for _ in range(3):
        r.against("static/bunkbed.tik")
    for _ in range(3):
        r.against("static/locker.tik")
    r.against("static/bunkerstove.tik")
    r.table_with("static/bunkertable.tik", ["furniture/bunkerchair.tik", "furniture/simplestool.tik"],
                 ["static/messkit.tik", "static/metal_mug.tik", "static/static_germanhelmet_wehrmact.tik"])


def kit_bunker(r):
    r.against("static/bunkershelves.tik")
    r.against("static/bunkerbench.tik")
    r.table_with("static/bunkertable.tik", ["furniture/bunkerchair.tik"], ["static/static_radio2.tik", "static/metal_mug.tik"])
    for m in ("static/nazi_crate.tik", "static/mg42ammoboxwbelt.tik", "static/50cal_crate.tik"):
        r.free(m, margin=8)


def kit_workshop(r):
    w = r.against("static/worktable.tik")
    if w:
        r.on_top_of(w, ["static/metal_mug.tik", "static/pots.tik"])
    for m in ("static/static_cablespool_full.tik", "static/nazi_crate.tik", "static/nazi_crate.tik", "static/exp_crate1.tik",
              "static/woodbucket.tik", "static/bunkershelves.tik"):
        if m.endswith("shelves.tik"):
            r.against(m)
        else:
            r.free(m, margin=10)


def kit_garage(r):
    for m in ("static/bunkershelves.tik", "static/worktable.tik", "static/static_powerbox1.tik"):
        r.against(m)
    for m in ("static/static_cablespool_full.tik", "static/nazi_crate.tik", "static/nazi_crate.tik", "static/88shell.tik",
              "static/88shell.tik", "static/88shell.tik"):
        r.free(m, margin=10)


def kit_hut(r):
    r.against("static/cot.tik")
    r.against("static/bunkerstove.tik")
    r.against("static/locker.tik")
    r.against("static/coatrack.tik")
    r.table_with("static/round_table.tik", ["furniture/simplestool.tik"], ["static/german_lantern.tik", "static/metal_mug.tik"])


def kit_desert(r):
    r.table_with("static/round_table.tik", ["furniture/simplestool.tik", "furniture/simplestool.tik"],
                 ["static/pitcher.tik", "static/dish.tik"])
    r.against(r.rnd.choice(["static/single_bed.tik", "static/cot.tik"]))
    for m in ("static/wicker_basket_2.tik", "static/wicker_basket_3.tik",
              "static/jug.tik", "static/wicker_basket_1.tik", "static/basket1.tik"):
        r.against(m, gap=4)
    r.against("static/cabinet_dark.tik")


def kit_kitchen(r):
    w = r.against("static/worktable.tik")
    if w:
        r.on_top_of(w, ["static/pots.tik", "static/fryingpan.tik", "static/bottle_large.tik"])
    r.against("static/bunkerstove.tik")
    r.against("static/cabinet_dark.tik")
    r.against("static/woodbucket.tik", gap=4)
    r.table_with("static/square_table.tik", ["furniture/woodchair.tik", "furniture/simplestool.tik"],
                 ["static/whiteplate.tik", "static/jug.tik"])


def kit_barn(r):
    r.against("static/haystack.tik", gap=8)
    r.against("static/haystack.tik", gap=8)
    r.against("static/winecasks.tik")
    r.free("static/cart.tik", margin=24)
    for m in ("static/flourbag-stack.tik", "static/woodbucket.tik", "static/pitchfork.tik", "static/shovel.tik"):
        r.against(m, gap=3)


def kit_hall(r):
    d = r.against("static/large_desk.tik")
    if d:
        r.on_top_of(d, ["lights/tablelamp.tik", "miscobj/book_closed_thick_lavender.tik", "miscobj/phone_squarebase.tik"])
    for m in ("static/bookcase.tik", "static/bookcase.tik", "static/piano.tik", "static/loveseat.tik",
              "static/grandfathers_clock.tik", "furniture/armchairposh.tik"):
        r.against(m)
    r.against("static/widepainting.tik", gap=0.5, z=r.z + 72, hang=True)
    r.against("static/oldladypainting.tik", gap=0.5, z=r.z + 64, hang=True)


ROOM_KITS = {
    "living": kit_living, "bedroom": kit_bedroom, "office": kit_office, "barracks": kit_barracks,
    "bunker": kit_bunker, "workshop": kit_workshop, "garage": kit_garage, "hut": kit_hut,
    "desert": kit_desert, "kitchen": kit_kitchen, "barn": kit_barn, "hall": kit_hall,
}
