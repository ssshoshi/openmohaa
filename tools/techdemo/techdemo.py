"""m0_techdemo: a map to look at and measure the fork's features on.

    plaza (north, flat, under the sky)        field (south, hills, under the sky)
    +-----------+  +---------+---------+      grass for the ground cover, trees,
    | lighting  |  | physics |  range  |      and a pond the water program draws
    |   hall    |  |  room   | (gore)  |      and bodies float in
    |           |  +---------+---------+
    |           |   population  gallery
    +-----------+      yard     (materials)

The lighting hall is dark and full of coloured lamps among pillars, for the
realtime lights and their shadows. The physics room is full of props the
physics can knock about. The range has passive soldiers and gas barrels at
the far end, to shoot and blow up (gore, ragdolls). The gallery's walls are
textures with material maps, lit at a grazing angle (normal and parallax
maps). The yard gets a fixed crowd of soldiers (the cost of a populated
scene). Its script (m0_techdemo.scr) spawns them, and with `techdemo_bench 1`
flies a camera along the same path every time for benchmarks.

Coordinates are the map's units (an inch each); +y is north.
"""

import math
import random

from mapgen import Map, Tex, box, heightfield, prism, smooth_noise, wall_with_door, hollow

NAME = "m0_techdemo"

# the world is a box of sky this big inside
HALF = 3584
SKY_TOP = 2048
FLOOR_LOW = -512

FLOOR = 8  # building floors are this far above the plaza
WALL = 16

T_SKY = "sky/mohday1"
T_CAULK = "common/caulk"
T_COBBLE = Tex("central_europe/small_cobble", 0.5)
T_GRASS = Tex("misc_outside/nu_grass_set2", 0.5)
T_MUD = Tex("misc_outside/nu_earth_set2", 0.5)
T_WATER = Tex("misc_outside/pond", 0.5)
T_CEIL = Tex("blasted/dday_bunker_wall1", 0.5)

# the gallery's panels, west to east, north side then south side: textures
# that have material maps (normal, height, specular) in the materials pak
GALLERY = [
    "algiers/jh_brickwall1",
    "general_structure/jh_adobewall1b",
    "general_structure/jh_rockwinwall1",
    "general_structure/jh_woodwall2a",
    "interior/jh_stonetile1",
    "interior/marblefoyer",
    "general_industrial/jh_rivplate1a",
    "norway/seacrete",
    "interior/gildwall1",
    "central_europe/church_interior_wall1",
    "algiers/plasterwal2",
    "blasted/dday_bunker_wall1",
]


def building(m, x1, y1, x2, y2, height, wall, floor, ceil, doors=()):
    """A room standing on the plaza: raised floor, four walls, a roof.
    `doors` are (side, centre along the wall, width, height); side is one of
    north, south, east, west. Returns the inside floor height."""
    z0 = FLOOR
    z1 = FLOOR + height
    m.add(box((x1, y1, 0), (x2, y2, z0), floor, sides=T_CAULK, bottom=T_CAULK))
    m.add(box((x1 - WALL, y1 - WALL, z1), (x2 + WALL, y2 + WALL, z1 + WALL), ceil, top=T_CAULK, sides=T_CAULK))
    by_side = {}
    for side, c, w, h in doors:
        by_side.setdefault(side, []).append((c - w / 2, c + w / 2, z0 + h))
    for side in ("north", "south", "east", "west"):
        if side in ("north", "south"):
            at = y2 if side == "north" else y1 - WALL
            lo, hi, axis = x1 - WALL, x2 + WALL, "y"
        else:
            at = x2 if side == "east" else x1 - WALL
            lo, hi, axis = y1, y2, "x"
        cuts = sorted(by_side.get(side, []))
        if not cuts:
            if axis == "y":
                m.add(box((lo, at, 0), (hi, at + WALL, z1), wall))
            else:
                m.add(box((at, lo, 0), (at + WALL, hi, z1), wall))
            continue
        # the wall in pieces between the doors, and a lintel over each
        start = lo
        for dlo, dhi, top in cuts:
            m.add(wall_with_door(axis, at, start, dhi, 0, z1, dlo, dhi, top, wall, WALL))
            start = dhi
        if hi > start:
            if axis == "y":
                m.add(box((start, at, 0), (hi, at + WALL, z1), wall))
            else:
                m.add(box((at, start, 0), (at + WALL, hi, z1), wall))
    return z0


# MOHlight's values suit retail rooms a few hundred units across; these rooms
# are bigger, so every lamp is turned up by this much
LIGHT_SCALE = 15


def light(m, origin, value, color=(1, 1, 1), spot=None, **keys):
    e = m.ent("light", origin=origin, light=value * LIGHT_SCALE, _color=color, **keys)
    if spot:
        pitch, yaw, angle = spot
        e.set("angles", (pitch, yaw, 0)).set("spot_angle", angle)
    return e


def static(m, model, origin, yaw=0, scale=None):
    cls = "static_" + model.split("/")[-1].replace(".tik", "").replace("-", "_")
    e = m.ent(cls, model=model, origin=origin, angle=yaw)
    if scale:
        e.set("scale", scale)
    return e


def barrel(m, x, y, z, kind="gas", r=14, h=44):
    """A func_barrel: an eight-sided drum of brushes, as the retail maps' are."""
    e = m.ent("func_barrel", barreltype=kind)
    sides = 8
    faces = []
    top = (x, y, z + h)
    pts = [(x + r * math.cos(2 * math.pi * (i + 0.5) / sides), y + r * math.sin(2 * math.pi * (i + 0.5) / sides)) for i in range(sides)]
    from mapgen import Brush
    t_side = Tex("barrel/benzinbarrel", 0.25)
    t_top = Tex("barrel/benzinbarrel_top", 0.25)
    faces.append((((x, y, z + h), (x + 1, y, z + h), (x, y + 1, z + h)), t_top))
    faces.append((((x, y, z), (x + 1, y, z), (x, y + 1, z)), t_top))
    for i in range(sides):
        a, b = pts[i], pts[(i + 1) % sides]
        faces.append((((a[0], a[1], z), (b[0], b[1], z), (b[0], b[1], z + h)), t_side))
    e.brushes.append(Brush(faces, (x, y, z + h / 2)))
    return e


# ---------------------------------------------------------------------------
# the field: hills, and a pond in a hollow

FIELD_STEP = 128
POND = (1536, -1792, 704)  # x, y, radius
POND_DEPTH = 210
WATER_Z = -40


def field_heights():
    nx = HALF * 2 // FIELD_STEP + 1
    ny = HALF // FIELD_STEP + 1
    noise = smooth_noise(nx, ny, seed=7, octaves=3, scale=150)
    hs = []
    for j in range(ny):
        y = -HALF + j * FIELD_STEP
        row = []
        for i in range(nx):
            x = -HALF + i * FIELD_STEP
            # flat where it meets the plaza, rising to the hills
            edge = min(1.0, -y / 768.0)
            edge = edge * edge * (3 - 2 * edge)
            h = noise[j][i] * edge
            d = math.hypot(x - POND[0], y - POND[1]) / POND[2]
            if d < 1:
                h -= POND_DEPTH * (1 - d * d) ** 1.5
            row.append(round(h))
        hs.append(row)
    return hs


def field_height_at(hs, x, y):
    fx = (x + HALF) / FIELD_STEP
    fy = (y + HALF) / FIELD_STEP
    i, j = int(fx), int(fy)
    i = max(0, min(i, len(hs[0]) - 2))
    j = max(0, min(j, len(hs) - 2))
    tx, ty = fx - i, fy - j
    a = hs[j][i] * (1 - tx) + hs[j][i + 1] * tx
    b = hs[j + 1][i] * (1 - tx) + hs[j + 1][i + 1] * tx
    return a * (1 - ty) + b * ty


def build_field(m):
    hs = field_heights()

    def tex_for(i, j):
        low = min(hs[j][i], hs[j][i + 1], hs[j + 1][i], hs[j + 1][i + 1])
        return T_MUD if low < WATER_Z - 10 else T_GRASS

    m.add(heightfield(-HALF, -HALF, FIELD_STEP, hs, tex_for, FLOOR_LOW + 64))

    # the pond: one water brush over the hollow; the ground outside the
    # hollow rises through it, so its edge is the shoreline
    px, py, pr = POND
    m.add(box((px - pr, py - pr, FLOOR_LOW + 80), (px + pr, py + pr, WATER_Z), T_WATER))

    rnd = random.Random(11)
    trees = ["static/tree_oak.tik", "static/tree_commontree.tik", "static/tree_birch.tik"]
    bushes = ["static/bush_full.tik", "static/bush_regularbush.tik", "static/bush_sparseshrub.tik"]
    placed = 0
    while placed < 26:
        x = rnd.uniform(-HALF + 384, HALF - 384)
        y = rnd.uniform(-HALF + 384, -640)
        if math.hypot(x - px, y - py) < pr + 160:
            continue
        model = trees[placed % len(trees)] if placed < 16 else bushes[placed % len(bushes)]
        static(m, model, (round(x), round(y), round(field_height_at(hs, x, y)) - 4), yaw=rnd.randrange(360))
        placed += 1
    return hs


# ---------------------------------------------------------------------------
# the plaza and its buildings


def build_plaza(m):
    m.add(box((-HALF, 0, FLOOR_LOW + 64), (HALF, HALF, 0), T_CAULK, top=T_COBBLE))


HALL = (-3072, 512, -1024, 2560, 384)
# how far static/lightbulb_on_wire.tik reaches up from its bulb, less a little
# so that the wire goes into the ceiling it hangs from
HALL_WIRE = 195


def build_hall(m):
    x1, y1, x2, y2, h = HALL
    wall = Tex("blasted/dday_bunker_wall1", 0.5)
    z0 = building(m, x1, y1, x2, y2, h, wall, Tex("algiers/fort_floor_cobbleflttile", 0.5),
                  T_CEIL, doors=[("east", 1536, 192, 224)])
    # pillars for the lights to throw shadows from
    for px in (-2560, -2048, -1536):
        for py in (1024, 1536, 2048):
            m.add(box((px - 40, py - 40, z0), (px + 40, py + 40, z0 + h), wall))
    # coloured lamps between the pillars, each a bulb on a long wire with its
    # light at the bulb: they hang from the ceiling and swing when hit
    colors = [(1, 0.55, 0.25), (0.35, 0.55, 1), (1, 0.9, 0.7), (0.4, 1, 0.45), (1, 0.3, 0.3), (0.8, 0.5, 1)]
    n = 0
    bulb = z0 + h - HALL_WIRE
    for lx in (-2816, -2304, -1792, -1280):
        for ly in (768, 1280, 1792, 2304):
            c = colors[n % len(colors)]
            light(m, (lx, ly, bulb + 6), 24, c)
            static(m, "static/lightbulb_on_wire.tik", (lx, ly, bulb))
            n += 1
    # spots down onto the floor near the door, where people will walk
    for sy in (1152, 1920):
        light(m, (-1200, sy, z0 + h - 24), 60, (1, 0.95, 0.85), spot=(90, 0, 50))
    # crates to cast shadows
    for cx, cy, yaw in ((-2304, 1536, 15), (-1792, 1024, 40), (-2816, 2048, 0), (-1792, 2304, 70)):
        static(m, "static/indycrate.tik", (cx, cy, z0), yaw=yaw)


PHYS = (-512, 2048, 1536, 3328, 320)

# how high static/square_table.tik's top is
TABLE_TOP = 48


def build_physics(m):
    x1, y1, x2, y2, h = PHYS
    z0 = building(m, x1, y1, x2, y2, h, Tex("algiers/plasterwal2", 0.5), Tex("central_europe/flrwood2", 0.5),
                  T_CEIL, doors=[("south", 512, 256, 256)])
    for lx in (0, 512, 1024):
        for ly in (2432, 2944):
            light(m, (lx, ly, z0 + 190), 60, (1, 0.92, 0.8))
            static(m, "static/lightbulb_caged.tik", (lx, ly, z0 + h - 4))
    # a ramp to roll and slide things down
    ramp = Tex("central_europe/flrwood2", 0.5)
    for ya, yb in ((2200, 2600),):
        tri1 = ((1100, ya, z0), (1500, ya, z0), (1500, yb, z0 + 160))
        tri2 = ((1100, ya, z0), (1500, yb, z0 + 160), (1100, yb, z0 + 160))
        m.add(prism(tri1, z0, ramp, sides=ramp), prism(tri2, z0, ramp, sides=ramp))
        m.add(box((1100, yb, z0), (1500, yb + 200, z0 + 160), ramp))
    # stacks of crates
    for i, (cx, cy) in enumerate(((-256, 2944), (-96, 2944), (-176, 2944), (256, 3136))):
        for k in range(3 if i < 3 else 2):
            static(m, "static/exp_crate1.tik", (cx, cy, z0 + k * 40), yaw=(i * 17 + k * 9) % 30)
    # a pyramid of small crates
    for row in range(4):
        for k in range(4 - row):
            static(m, "static/fragcrate1.tik", (560 + k * 44 + row * 22, 3136, z0 + row * 30), yaw=0)
    # tables with bottles and buckets on them, chairs round them
    for tx, ty in ((256, 2432), (768, 2560), (128, 2752)):
        static(m, "static/square_table.tik", (tx, ty, z0))
        for k, model in enumerate(("static/bottle_large.tik", "static/bottle_small.tik", "static/jug.tik", "static/bucket.tik")):
            static(m, model, (tx - 18 + (k % 2) * 36, ty - 12 + (k // 2) * 24, z0 + TABLE_TOP), yaw=k * 40)
        for k, (ox, oy, yaw) in enumerate(((0, -48, 90), (0, 48, 270), (-48, 0, 0), (48, 0, 180))):
            static(m, "furniture/woodchair.tik" if k % 2 else "furniture/simplechair.tik", (tx + ox, ty + oy, z0), yaw=yaw)
    for k in range(6):
        static(m, "static/bucket.tik", (1300 + (k % 3) * 40, 2700 + (k // 3) * 40, z0 + 160))
    # a lantern on a table of its own, alight: a prop that carries its light
    # (knock it over and the light, and its shadows, go with it)
    static(m, "static/square_table.tik", (900, 2850, z0))
    static(m, "static/german_lantern.tik", (900, 2850, z0 + TABLE_TOP))
    light(m, (900, 2850, z0 + TABLE_TOP + 12), 10, (1, 0.7, 0.4))


GALLERY_BOX = (1792, 512, 3328, 1024, 256)


def build_gallery(m):
    x1, y1, x2, y2, h = GALLERY_BOX
    z0 = FLOOR
    z1 = z0 + h
    m.add(box((x1, y1, 0), (x2, y2, z0), Tex("interior/marblefoyer", 0.5), sides=T_CAULK, bottom=T_CAULK))
    m.add(box((x1 - WALL, y1 - WALL, z1), (x2 + WALL, y2 + WALL, z1 + WALL), Tex("blasted/dday_bunker_wall1", 0.5), top=T_CAULK, sides=T_CAULK))
    m.add(wall_with_door("x", x1 - WALL, y1 - WALL, y2 + WALL, 0, z1, 688, 848, z0 + 208, Tex("blasted/dday_bunker_wall1", 0.5)))
    m.add(box((x2, y1 - WALL, 0), (x2 + WALL, y2 + WALL, z1), Tex("blasted/dday_bunker_wall1", 0.5)))
    # panels: the north and south walls in 256 unit pieces, a texture each,
    # and a lamp near the top of each for light raking down it
    span = (x2 - x1) // 6
    for k in range(6):
        a, b = x1 + k * span, x1 + (k + 1) * span
        n_tex = Tex(GALLERY[k], 0.5)
        s_tex = Tex(GALLERY[6 + k], 0.5)
        m.add(box((a, y2, 0), (b, y2 + WALL, z1), n_tex))
        m.add(box((a, y1 - WALL, 0), (b, y1, z1), s_tex))
        mid = (a + b) / 2
        light(m, (mid, y2 - 20, z1 - 16), 12, (1, 0.95, 0.88))
        light(m, (mid, y1 + 20, z1 - 16), 12, (1, 0.95, 0.88))


RANGE = (1792, 1280, 3328, 3328, 320)


def build_range(m):
    x1, y1, x2, y2, h = RANGE
    z0 = building(m, x1, y1, x2, y2, h, Tex("algiers/jh_brickwall1", 0.5), Tex("algiers/fort_floor_outside", 0.5),
                  T_CEIL, doors=[("west", 1536, 192, 224)])
    for ly in (1536, 2176, 2816, 3136):
        light(m, (2560, ly, z0 + 190), 55, (1, 0.93, 0.82))
        static(m, "static/lightbulb_caged.tik", (2560, ly, z0 + h - 4))
    # sandbags in front of the targets and gas barrels among them
    for sx in (2048, 2400, 2752, 3104):
        static(m, "static/sandbag_longsegment.tik", (sx, 2752, z0))
    for bx, by in ((2176, 3040), (2560, 2976), (2944, 3072), (3200, 2944), (1984, 2944)):
        barrel(m, bx, by, z0)
    # a shooting line
    m.add(box((1856, 1664, z0), (3264, 1680, z0 + 40), Tex("general_structure/jh_woodwall2a", 0.5)))


# ---------------------------------------------------------------------------


def path_nodes(m, x1, y1, x2, y2, z, step=256):
    for x in range(x1, x2 + 1, step):
        for y in range(y1, y2 + 1, step):
            m.ent("info_pathnode", origin=(x, y, z))


# the benchmark's path: where the camera goes, what it faces (pitch yaw),
# and the time to the next point in seconds
BENCH_PATH = [
    ((256, 256, 120), (5, 90)),
    ((256, 1200, 140), (10, 60)),       # over the crowd in the yard
    ((256, 1900, 120), (0, 90)),
    ((512, 2200, 100), (5, 70)),        # into the physics room
    ((900, 2800, 110), (10, 20)),
    ((256, 3000, 110), (5, 200)),
    ((512, 2200, 100), (0, 270)),
    ((-512, 1536, 110), (0, 180)),      # across to the hall
    ((-1100, 1536, 110), (0, 180)),
    ((-1792, 1280, 120), (5, 150)),     # among the pillars and lamps
    ((-2816, 1536, 110), (5, 90)),
    ((-2304, 2304, 120), (10, 0)),
    ((-1100, 1536, 110), (0, 0)),
    ((1200, 1536, 110), (0, 0)),        # the range from the shooting line
    ((2560, 1800, 100), (0, 90)),
    ((2560, 2500, 100), (5, 90)),
    ((2000, 1536, 110), (0, 270)),
    ((1500, 768, 110), (0, 0)),         # the gallery's walls, raking light
    ((2560, 768, 90), (0, 60)),
    ((3150, 768, 90), (0, 270)),
    ((1400, 768, 140), (10, 270)),
    ((1500, -400, 260), (15, 250)),     # out over the field and the pond
    ((1536, -1500, 140), (20, 270)),
    ((600, -2600, 180), (10, 30)),
    ((-1500, -2000, 220), (5, 60)),
    ((-800, -300, 200), (5, 45)),
    ((256, 256, 120), (5, 90)),
]


# how fast the benchmark camera goes, in units a second
BENCH_SPEED = 256


def bench_path(m):
    """The path's nodes. The camera's spline takes a unit of time from a node
    to the next, times the node's speed, so each node's speed is set from the
    distance to the next one: the camera goes at BENCH_SPEED all the way.
    Returns how long a lap takes, in seconds."""
    n = len(BENCH_PATH)
    total = 0.0
    for i, (o, (p, y)) in enumerate(BENCH_PATH):
        e = m.ent("info_splinepath", origin=o, angles=(p, y, 0), targetname="bench%d" % i)
        if i + 1 < n:
            d = math.dist(o, BENCH_PATH[i + 1][0])
            total += d
            e.set("target", "bench%d" % (i + 1))
            e.set("speed", round(BENCH_SPEED / d, 4))
    return total / BENCH_SPEED


def bench_script(lap):
    """maps/m0_techdemo_bench.scr, made with the map: how long a lap is."""
    return "// made by tools/techdemo/techdemo.py with the map\n\nlap:\nend %.1f\n" % lap


def build():
    m = Map(
        message="OpenMoHAA tech demo",
        suncolor=(70, 64, 52),
        sunlight=(255, 243, 210),
        sundirection=(325, 150, 0),
        sundiffuse=1.3,
        ambientlight=(7, 7, 8),
        farplane_color=(0.66, 0.72, 0.8),
        lightmapdensity=16,
        overbright="all",
    )
    # the sky all round
    m.add(hollow((-HALF, -HALF, FLOOR_LOW), (HALF, HALF, SKY_TOP), T_SKY, floor=T_CAULK))
    build_plaza(m)
    hs = build_field(m)
    build_hall(m)
    build_physics(m)
    build_gallery(m)
    build_range(m)

    # the yard, for the crowd, and pathnodes wherever people stand
    path_nodes(m, -512, 512, 1280, 1792, 24)
    path_nodes(m, 2048, 1536, 3072, 3072, 32)
    path_nodes(m, -2816, 768, -1280, 2304, 32, step=512)

    m.ent("info_player_start", origin=(256, 256, 24), angle=90)
    m.bench_lap = bench_path(m)
    return m


if __name__ == "__main__":
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else NAME + ".map"
    with open(out, "w") as f:
        f.write(build().text())
    print(out)
