"""The biomes' ground: a heightfield of brush prisms, its textures at the
retail size, and decals laid on it that blend one ground into another.

A brush face has one texture, so where two grounds meet there is a line
along the cells' edges. The retail levels hide those lines with transition
tiles and decals; here a cell takes its zone's ground (or rock where it is
steep, or a road's verge) and decals on top of it do the blending:

- splats: soft-edged patches of a second ground (dirt in grass, rough snow
  in smooth), each a picture laid once over a few cells, its edge noisy and
  faded (alpha), so that it has no line;
- along a rock face, splats of the ground over the rock's edge, so the cliff
  rises out of the ground rather than meeting it at a cell's edge;
- roads: a strip along each road, its edges faded, over the verge.

Each decal is the cells' own triangles lifted a unit, drawn with a shader of
the tech demo's own (polygonOffset, blended, lit by its own lightmap: the
retail decals' way) and not solid. The pictures are made when the map is
built, from the game's own textures (make_textures), so nothing retail is in
the repository: what is, is how to make them.

The ground's heights get a unit or two of noise in the middle of the band:
the compiler merges coplanar faces of a flat stretch into big ones it then
can't always triangulate, and drops them (holes in the ground).
"""

import math
import random

import texscale
from kit import NODRAW, T, overlay_tri, tex_rot
from mapgen import Tex, prism

SPLAT_PX = 512
ROAD_PX = 512


class Overlays:
    """The pictures the decals need and their shaders, collected while the
    map is laid out and made when it is built."""

    def __init__(self):
        self.images = {}   # name -> spec

    def splat(self, material, world, variant):
        # names stay short: the compilers keep 64 characters of a shader's
        name = "opm_td/sp_%s_%d" % (material.split("/")[-1], variant)
        self.images[name] = {"kind": "splat", "src": material, "world": world, "seed": variant}
        return name

    def road(self, material, width, across, along):
        name = "opm_td/rd_%s_%d" % (material.split("/")[-1], width)
        self.images[name] = {"kind": "road", "src": material, "width": width, "across": across, "along": along}
        return name

    def shader_text(self):
        out = ["// made by tools/techdemo with the map: the decals that blend its grounds\n"]
        for name, spec in sorted(self.images.items()):
            ext = "tga"
            mapkw = "clampmap" if spec["kind"] == "splat" else "map"
            out.append("""textures/%s
{
	qer_editorimage textures/%s.%s
	surfaceparm nonsolid
	surfaceparm trans
	surfaceparm nomarks
	polygonOffset
	{
		%s textures/%s.%s
		blendFunc blend
	nextbundle
		map $lightmap
	}
}
""" % (name, name, ext, mapkw, name, ext))
        return "\n".join(out)


OVERLAYS = Overlays()


def heightfield_tris(x0, y0, step, hs):
    """{(i, j): [two triangles]} of a heightfield, split as mapgen's is."""
    out = {}
    for j in range(len(hs) - 1):
        for i in range(len(hs[0]) - 1):
            def P(ii, jj):
                return (x0 + ii * step, y0 + jj * step, hs[jj][ii])
            a, b, c, d = P(i, j), P(i + 1, j), P(i + 1, j + 1), P(i, j + 1)
            if abs(a[2] - c[2]) <= abs(b[2] - d[2]):
                out[(i, j)] = ((a, b, c), (a, c, d))
            else:
                out[(i, j)] = ((a, b, d), (b, c, d))
    return out


def ground_brushes(tris, texfor, zbottom):
    out = []
    for (i, j), pair in tris.items():
        t = texfor(i, j)
        for tri in pair:
            out.append(prism(tri, zbottom, t, detail=True))
    return out


def jitter(hs, seed, amount=1.5, keep=lambda i, j: False):
    """A unit or so of noise on each height, but where keep(i, j) (the edges
    another piece of ground or a floor meets)."""
    rnd = random.Random(seed)
    for j, row in enumerate(hs):
        for i in range(len(row)):
            d = rnd.uniform(-amount, amount)
            if not keep(i, j):
                row[i] = round(row[i] + d, 2)
    return hs


def splat_brushes(tris, x0, y0, step, cx, cy, size, angle, shader, lift=1.0):
    """A splat: the cells' triangles under a size-wide square at cx cy,
    turned by angle, lifted, drawn with the splat's picture laid once."""
    # the picture is a disc (its alpha is nothing past half its width), so
    # only the triangles that reach into the disc are needed
    r = size * 0.49
    i0, i1 = int((cx - r - x0) // step), int((cx + r - x0) // step)
    j0, j1 = int((cy - r - y0) // step), int((cy + r - y0) // step)
    t = tex_rot(shader, cx, cy, angle, size / SPLAT_PX, size / SPLAT_PX, SPLAT_PX, SPLAT_PX)
    out = []
    for j in range(j0, j1 + 1):
        for i in range(i0, i1 + 1):
            for tri in tris.get((i, j), ()):
                if _tri_disc(tri, cx, cy, r):
                    out.append(overlay_tri(tri, t, lift=lift))
    return out


def _tri_disc(tri, cx, cy, r):
    """Whether a triangle reaches into a disc (seen from above)."""
    if any(math.hypot(p[0] - cx, p[1] - cy) < r for p in tri):
        return True
    for a, b in ((tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])):
        dx, dy = b[0] - a[0], b[1] - a[1]
        L2 = dx * dx + dy * dy or 1
        t = max(0, min(1, ((cx - a[0]) * dx + (cy - a[1]) * dy) / L2))
        if math.hypot(a[0] + t * dx - cx, a[1] + t * dy - cy) < r:
            return True
    # the disc's middle inside the triangle
    (ax, ay, _), (bx, by, _), (qx, qy, _) = tri
    d1 = (cx - bx) * (ay - by) - (ax - bx) * (cy - by)
    d2 = (cx - qx) * (by - qy) - (bx - qx) * (cy - qy)
    d3 = (cx - ax) * (qy - ay) - (qx - ax) * (cy - ay)
    return not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0))


def road_brushes(tris, x0, y0, step, pts, half, shader, across, along, lift=1.5):
    """A road's strip: every triangle near the line, drawn with the road's
    picture turned along its nearest stretch (across: the picture's width in
    units, wide enough that it never repeats across a triangle; along: its
    length)."""
    reach = half + step * 1.5
    out = []
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    i0, i1 = int((min(xs) - reach - x0) // step), int((max(xs) + reach - x0) // step)
    j0, j1 = int((min(ys) - reach - y0) // step), int((max(ys) + reach - y0) // step)
    for j in range(j0, j1 + 1):
        for i in range(i0, i1 + 1):
            for tri in tris.get((i, j), ()):
                cx = sum(p[0] for p in tri) / 3
                cy = sum(p[1] for p in tri) / 3
                best = None
                for a, b in zip(pts, pts[1:]):
                    dx, dy = b[0] - a[0], b[1] - a[1]
                    L2 = dx * dx + dy * dy or 1
                    tt = max(0, min(1, ((cx - a[0]) * dx + (cy - a[1]) * dy) / L2))
                    d = math.hypot(cx - a[0] - tt * dx, cy - a[1] - tt * dy)
                    if best is None or d < best[0]:
                        best = (d, a, b)
                if best[0] > half + step * 0.75:
                    continue
                d, a, b = best
                ang = math.degrees(math.atan2(b[1] - a[1], b[0] - a[0])) + 90
                t = tex_rot(shader, a[0], a[1], ang, across / ROAD_PX, along / ROAD_PX, ROAD_PX, ROAD_PX)
                out.append(overlay_tri(tri, t, lift=lift))
    return out


# ---------------------------------------------------------------------------
# making the pictures (when the map is built, from the game's paks)


def make_textures(read_image):
    """{path in the pak: bytes} for every decal picture the layout asked for.
    read_image(shader) gives the retail texture's picture (a PIL image)."""
    import io

    import numpy as np
    from PIL import Image

    def tileable_noise(n, cells, seed, octaves=4):
        rnd = np.random.RandomState(seed)
        out = np.zeros((n, n))
        amp, total = 1.0, 0.0
        for o in range(octaves):
            c = cells * (2 ** o)
            g = rnd.rand(c, c)
            # bicubic-ish upsampling with wrap: smoothstep between grid points
            xs = np.arange(n) * c / n
            i0 = np.floor(xs).astype(int)
            f = xs - i0
            f = f * f * (3 - 2 * f)
            i1 = (i0 + 1) % c
            i0 = i0 % c
            row = g[i0][:, i0] * (1 - f)[None, :] + g[i0][:, i1] * f[None, :]
            row2 = g[i1][:, i0] * (1 - f)[None, :] + g[i1][:, i1] * f[None, :]
            out += (row * (1 - f)[:, None] + row2 * f[:, None]) * amp
            total += amp
            amp *= 0.5
        return out / total

    def sample(src, world_per_px, n_w, n_h, angle=0.0):
        """The retail texture as the map draws it, at world_per_px units a pixel."""
        img = np.asarray(src.convert("RGB")).astype(np.float32)
        h, w = img.shape[:2]
        # the repeat the map draws it at: the retail scale over the retail
        # picture's size (a texture pack's picture is bigger, the repeat the same)
        sx, sy, rw, rh = texscale.scale(spec_src)
        rep_x, rep_y = rw * sx, rh * sy
        ys, xs = np.mgrid[0:n_h, 0:n_w].astype(np.float32)
        u = (xs * world_per_px / rep_x * w) % w
        v = (ys * world_per_px / rep_y * h) % h
        # bilinear
        u0 = np.floor(u).astype(int)
        v0 = np.floor(v).astype(int)
        fu = (u - u0)[..., None]
        fv = (v - v0)[..., None]
        u1 = (u0 + 1) % w
        v1 = (v0 + 1) % h
        return (img[v0, u0] * (1 - fu) * (1 - fv) + img[v0, u1] * fu * (1 - fv) +
                img[v1, u0] * (1 - fu) * fv + img[v1, u1] * fu * fv)

    out = {}
    for name, spec in OVERLAYS.images.items():
        spec_src = spec["src"]
        src = read_image(spec_src)
        if src is None:
            print("  no picture for", spec_src)
            continue
        if spec["kind"] == "splat":
            n = SPLAT_PX
            rgb = sample(src, spec["world"] / n, n, n)
            ys, xs = np.mgrid[0:n, 0:n] / (n - 1) - 0.5
            r = np.sqrt(xs * xs + ys * ys) / 0.5
            noise = tileable_noise(n, 3, 7 + spec["seed"] * 13)
            fine = tileable_noise(n, 12, 101 + spec["seed"])
            a = (1 - r) * 1.15 + (noise - 0.5) * 1.1 + (fine - 0.5) * 0.35
            a = np.clip((a - 0.15) / 0.45, 0, 1)
            a = a * a * (3 - 2 * a)
            a[r > 0.97] = 0
            a *= np.clip((0.97 - r) / 0.08, 0, 1)
        else:
            n = ROAD_PX
            per = spec["across"] / n
            rgb = sample(src, per, n, n)
            ys, xs = np.mgrid[0:n, 0:n]
            dx = np.abs((xs + 0.5) / n - 0.5) * spec["across"]      # units from the middle
            wob = tileable_noise(n, 6, 37)
            edge = spec["width"] / 2 + (wob - 0.5) * 40
            a = np.clip((edge - dx) / 28 + 0.5, 0, 1)
            a = a * a * (3 - 2 * a)
            a *= 0.92
        img = np.dstack([np.clip(rgb, 0, 255), a * 255]).astype(np.uint8)
        buf = io.BytesIO()
        Image.fromarray(img, "RGBA").save(buf, "TGA", compression="tga_rle")
        out["textures/%s.tga" % name] = buf.getvalue()
    out["scripts/opm_techdemo.shader"] = OVERLAYS.shader_text().encode("latin-1")
    return out
