"""Writing MOH:AA .map files from Python.

A .map is text: entities, each a set of key/value pairs and, for the world
and brush entities, convex brushes. A brush is a list of planes, each given as
three points and the shader drawn on it (brush_primit 0, the format
MOHRadiant writes and Q3map reads):

    ( x y z ) ( x y z ) ( x y z ) shader xoff yoff rot xscale yscale contents flags value

The three points go round clockwise seen from outside the brush, so the
plane's normal points out of it. Here a brush is given by its corners and
faces and the winding is worked out from the brush's centre, so callers never
think about it.
"""

import math


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _num(v):
    v = round(v, 3)
    return str(int(v)) if v == int(v) else ("%.3f" % v).rstrip("0")


class Tex:
    """A shader and how it lies on a face."""

    def __init__(self, shader, scale=0.5, xoff=0, yoff=0, rot=0, sx=None, sy=None):
        self.shader = shader
        self.sx = sx if sx is not None else scale
        self.sy = sy if sy is not None else scale
        self.xoff, self.yoff, self.rot = xoff, yoff, rot

    def text(self):
        return "%s %s %s %s %s %s 0 0 0" % (
            self.shader, _num(self.xoff), _num(self.yoff), _num(self.rot), _num(self.sx), _num(self.sy))


def tex(t):
    return t if isinstance(t, Tex) else Tex(t)


class Brush:
    """A convex brush: faces as (three points on the plane, texture)."""

    def __init__(self, faces, center, detail=False):
        self.detail = detail
        self.faces = []
        for pts, t in faces:
            a, b, c = pts
            n = _cross(_sub(a, b), _sub(c, b))
            if _dot(n, n) < 1e-9:
                raise ValueError("degenerate face %r" % (pts,))
            # q3map's normal is (a - b) x (c - b): it must point away from the centre
            if _dot(n, _sub(center, b)) > 0:
                a, c = c, a
            self.faces.append(((a, b, c), tex(t)))

    def text(self):
        out = ["{"]
        for pts, t in self.faces:
            line = " ".join("( %s %s %s )" % tuple(_num(v) for v in p) for p in pts) + " " + t.text()
            # detail brushes don't split the vis tree (as MOHRadiant marks them)
            out.append(line + (" +surfaceparm detail" if self.detail else ""))
        out.append("}")
        return "\n".join(out)


def box(mins, maxs, t, top=None, bottom=None, sides=None):
    """An axis-aligned box. `top`, `bottom`, `sides` override `t` there."""
    x1, y1, z1 = mins
    x2, y2, z2 = maxs
    if x1 >= x2 or y1 >= y2 or z1 >= z2:
        raise ValueError("empty box %r %r" % (mins, maxs))
    s = sides or t
    center = ((x1 + x2) / 2, (y1 + y2) / 2, (z1 + z2) / 2)
    faces = [
        (((x1, y1, z2), (x2, y1, z2), (x2, y2, z2)), top or t),
        (((x1, y1, z1), (x2, y1, z1), (x2, y2, z1)), bottom or t),
        (((x1, y1, z1), (x1, y2, z1), (x1, y2, z2)), s),
        (((x2, y1, z1), (x2, y2, z1), (x2, y2, z2)), s),
        (((x1, y1, z1), (x2, y1, z1), (x2, y1, z2)), s),
        (((x1, y2, z1), (x2, y2, z1), (x2, y2, z2)), s),
    ]
    return Brush(faces, center)


def prism(tri, zbottom, t, bottom=None, sides=None, detail=False):
    """A brush under a sloped triangle: the triangle (three xyz points) on top,
    vertical sides, and a flat bottom at `zbottom`. Ground is made of these."""
    p0, p1, p2 = tri
    s = sides or "common/caulk"
    cx = (p0[0] + p1[0] + p2[0]) / 3
    cy = (p0[1] + p1[1] + p2[1]) / 3
    cz = (min(p[2] for p in tri) + zbottom) / 2
    center = (cx, cy, cz)
    faces = [((p0, p1, p2), t), (((p0[0], p0[1], zbottom), (p1[0], p1[1], zbottom), (p2[0], p2[1], zbottom)), bottom or s)]
    for a, b in ((p0, p1), (p1, p2), (p2, p0)):
        faces.append((((a[0], a[1], zbottom), (b[0], b[1], zbottom), (b[0], b[1], zbottom + 64)), s))
    return Brush(faces, center, detail)


def hollow(mins, maxs, wall, floor=None, ceil=None, open_sides=(), thick=16):
    """A sealed room: six slabs `thick` wide around the inside box mins..maxs.
    `open_sides` names sides left out (e.g. 'top' for a courtyard under the
    sky, which then needs its own sky slab)."""
    x1, y1, z1 = mins
    x2, y2, z2 = maxs
    T = thick
    out = []
    if "bottom" not in open_sides:
        out.append(box((x1 - T, y1 - T, z1 - T), (x2 + T, y2 + T, z1), floor or wall, sides="common/caulk", bottom="common/caulk"))
    if "top" not in open_sides:
        out.append(box((x1 - T, y1 - T, z2), (x2 + T, y2 + T, z2 + T), ceil or wall, sides="common/caulk", top="common/caulk"))
    walls = {
        "west": ((x1 - T, y1, z1), (x1, y2, z2)),
        "east": ((x2, y1, z1), (x2 + T, y2, z2)),
        "south": ((x1 - T, y1 - T, z1), (x2 + T, y1, z2)),
        "north": ((x1 - T, y2, z1), (x2 + T, y2 + T, z2)),
    }
    for name, (a, b) in walls.items():
        if name not in open_sides:
            out.append(box(a, b, wall))
    return out


def wall_with_door(axis, at, lo, hi, z1, z2, door_lo, door_hi, door_top, t, thick=16):
    """A wall across `axis` ('x' = a wall at x=at running along y) from lo to
    hi, with a doorway from door_lo to door_hi up to door_top."""
    out = []
    spans = [(lo, door_lo, z1, z2), (door_hi, hi, z1, z2), (door_lo, door_hi, door_top, z2)]
    for a, b, za, zb in spans:
        if b - a <= 0 or zb - za <= 0:
            continue
        if axis == "x":
            out.append(box((at, a, za), (at + thick, b, zb), t))
        else:
            out.append(box((a, at, za), (b, at + thick, zb), t))
    return out


class Entity:
    def __init__(self, classname, **keys):
        self.keys = {"classname": classname}
        for k, v in keys.items():
            self.keys[k.replace("__", "#").replace("_S_", "$")] = v
        self.brushes = []

    def set(self, k, v):
        self.keys[k] = v
        return self

    def text(self, num):
        out = ["// entity %d" % num, "{"]
        for k, v in self.keys.items():
            if isinstance(v, (tuple, list)):
                v = " ".join(_num(x) for x in v)
            elif isinstance(v, float):
                v = _num(v)
            out.append('"%s" "%s"' % (k, v))
        for i, b in enumerate(self.brushes):
            out.append("// brush %d" % i)
            out.append(b.text())
        out.append("}")
        return "\n".join(out)


class Map:
    def __init__(self, **world):
        self.world = Entity("worldspawn", **world)
        self.ents = [self.world]

    def add(self, *brushes):
        for b in brushes:
            if isinstance(b, (list, tuple)):
                self.add(*b)
            else:
                self.world.brushes.append(b)

    def ent(self, classname, **keys):
        e = Entity(classname, **keys)
        self.ents.append(e)
        return e

    def text(self):
        return "\n".join(e.text(i) for i, e in enumerate(self.ents)) + "\n"


def heightfield(x0, y0, step, heights, t, zbottom):
    """Ground over a grid: heights[j][i] is the height at (x0 + i*step,
    y0 + j*step); each cell is two prisms down to `zbottom`. `t` is a texture
    or a function (i, j) -> texture for the cell. The prisms are detail: they
    don't seal the map (something under them must) and vis ignores them, which
    takes a full vis from many minutes to seconds."""
    out = []
    for j in range(len(heights) - 1):
        for i in range(len(heights[0]) - 1):
            def P(ii, jj):
                return (x0 + ii * step, y0 + jj * step, heights[jj][ii])
            a, b, c, d = P(i, j), P(i + 1, j), P(i + 1, j + 1), P(i, j + 1)
            tt = t(i, j) if callable(t) else t
            # split along the diagonal that keeps the cell flatter
            if abs(a[2] - c[2]) <= abs(b[2] - d[2]):
                tris = ((a, b, c), (a, c, d))
            else:
                tris = ((a, b, d), (b, c, d))
            for tri in tris:
                out.append(prism(tri, zbottom, tt, detail=True))
    return out


def smooth_noise(nx, ny, seed, octaves=3, scale=1.0):
    """Value noise in 0..1 on an nx by ny grid, deterministic."""
    import random
    rnd = random.Random(seed)
    out = [[0.0] * nx for _ in range(ny)]
    amp, total = 1.0, 0.0
    cells = 2
    for _ in range(octaves):
        g = [[rnd.random() for _ in range(cells + 2)] for _ in range(cells + 2)]
        for j in range(ny):
            for i in range(nx):
                fx = i / max(nx - 1, 1) * cells
                fy = j / max(ny - 1, 1) * cells
                ix, iy = int(fx), int(fy)
                tx, ty = fx - ix, fy - iy
                tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
                v = (g[iy][ix] * (1 - tx) + g[iy][ix + 1] * tx) * (1 - ty) + (g[iy + 1][ix] * (1 - tx) + g[iy + 1][ix + 1] * tx) * ty
                out[j][i] += v * amp
        total += amp
        amp *= 0.5
        cells *= 2
    return [[v / total * scale for v in row] for row in out]


def yaw_vec(yaw):
    r = math.radians(yaw)
    return math.cos(r), math.sin(r)
