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
from mapgen import Patch, prism

SPLAT_PX = 512
DECAL_DENSITY = 32
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
        """A road's picture: across units wide (the strip), the road width
        of them solid and the rest faded out; along units long."""
        name = "opm_td/rd_%s_%d" % (material.split("/")[-1], width)
        self.images[name] = {"kind": "road", "src": material, "width": width, "across": across, "along": along}
        return name

    # Lit from a lightmap of their own like the retail decals (the texture,
    # then the lightmap in the same stage: nextbundle). They were "rgbGen
    # vertex", but MOHlight leaves a patch's vertex colours white, so every
    # splat and road strip drew at full bright. The ground under them is lit
    # at kit.GROUND_DENSITY; they take the same.
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
	surfacedensity %d
	polygonOffset
	{
		%s textures/%s.%s
		blendFunc blend
	nextbundle
		map $lightmap
	}
}
""" % (name, name, ext, DECAL_DENSITY, mapkw, name, ext))
        return "\n".join(out)


OVERLAYS = Overlays()


class Ground:
    """A heightfield as brush prisms, flat stretches merged into bigger cells.

    The grid's cells (step wide) are merged into blocks of 2, 4 cells a side
    where the ground in a block lies within tol of the plane its corners
    make and the block is all one texture: that halves the prisms (the BSP
    has room for 131072 brush sides, the compiler adds bevels to each). Where
    a small cell meets a big one, the small one's corner on the big one's
    edge is put on that edge, so they meet without a crack (the compiler
    adds the point to the big face). Corners no bigger block shares get a
    unit or so of noise: flat stretches merged by the compiler came out with
    holes. keep(i, j): grid points left as they are (edges other ground meets)."""

    def __init__(self, x0, y0, step, hs, texfor, keep=lambda i, j: False, seed=1, tol=3.0, maxk=4, noise=1.5):
        self.x0, self.y0, self.step = x0, y0, step
        nx, ny = len(hs[0]), len(hs)
        self.nx, self.ny = nx, ny
        hs = [list(r) for r in hs]
        tex = {(i, j): texfor(i, j) for j in range(ny - 1) for i in range(nx - 1)}
        name = {k: v.shader for k, v in tex.items()}
        owner = {}
        blocks = []
        for k in (maxk, 2, 1):
            for bj in range(0, ny - 1, k):
                for bi in range(0, nx - 1, k):
                    if bi + k > nx - 1 or bj + k > ny - 1:
                        continue
                    if any((bi + a, bj + b) in owner for a in range(k) for b in range(k)):
                        continue
                    if k > 1 and not self._flat(hs, bi, bj, k, tol, name):
                        continue
                    blocks.append((bi, bj, k))
                    for a in range(k):
                        for b in range(k):
                            owner[(bi + a, bj + b)] = len(blocks) - 1
        self.blocks = blocks
        # which grid points are a block's corner, which lie on a bigger block's edge
        corner = set()
        for bi, bj, k in blocks:
            corner.update({(bi, bj), (bi + k, bj), (bi, bj + k), (bi + k, bj + k)})
        rnd = random.Random(seed)
        for (i, j) in sorted(corner):
            if not keep(i, j) and not self._on_big_edge(blocks, owner, i, j):
                hs[j][i] = round(hs[j][i] + rnd.uniform(-noise, noise), 2)
        for bi, bj, k in blocks:
            if k == 1:
                continue
            for e in range(1, k):
                for (i, j), (a, b) in (((bi + e, bj), ((bi, bj), (bi + k, bj))), ((bi + e, bj + k), ((bi, bj + k), (bi + k, bj + k))),
                                       ((bi, bj + e), ((bi, bj), (bi, bj + k))), ((bi + k, bj + e), ((bi + k, bj), (bi + k, bj + k)))):
                    if (i, j) in corner:
                        hs[j][i] = round(hs[a[1]][a[0]] + (hs[b[1]][b[0]] - hs[a[1]][a[0]]) * e / k, 3)
        self.hs = hs
        self.tris = []      # (triangle, texture)
        self.cell = {}      # (i, j) -> the triangles over that cell
        for bi, bj, k in blocks:
            def P(ii, jj):
                return (x0 + ii * step, y0 + jj * step, hs[jj][ii])
            a, b, c, d = P(bi, bj), P(bi + k, bj), P(bi + k, bj + k), P(bi, bj + k)
            pair = ((a, b, c), (a, c, d)) if abs(a[2] - c[2]) <= abs(b[2] - d[2]) else ((a, b, d), (b, c, d))
            for tri in pair:
                self.tris.append((tri, tex[(bi, bj)]))
            for u in range(k):
                for v in range(k):
                    self.cell[(bi + u, bj + v)] = pair

    @staticmethod
    def _flat(hs, bi, bj, k, tol, name):
        n0 = name[(bi, bj)]
        if any(name[(bi + a, bj + b)] != n0 for a in range(k) for b in range(k)):
            return False
        h00, h10, h01, h11 = hs[bj][bi], hs[bj][bi + k], hs[bj + k][bi], hs[bj + k][bi + k]
        if abs(h00 + h11 - h10 - h01) > tol:
            return False
        for b in range(k + 1):
            for a in range(k + 1):
                fx, fy = a / k, b / k
                plane = h00 + (h10 - h00) * fx + (h01 - h00) * fy
                if abs(hs[bj + b][bi + a] - plane) > tol:
                    return False
        return True

    @staticmethod
    def _on_big_edge(blocks, owner, i, j):
        for di in (-1, 0):
            for dj in (-1, 0):
                o = owner.get((i + di, j + dj))
                if o is None:
                    continue
                bi, bj, k = blocks[o]
                if k > 1 and not ((i - bi) % k == 0 and (j - bj) % k == 0):
                    return True
        return False

    def brushes(self, zbottom):
        return [prism(tri, zbottom, t, detail=True) for tri, t in self.tris]

    def at(self, x, y):
        i = max(0, min(int((x - self.x0) // self.step), self.nx - 2))
        j = max(0, min(int((y - self.y0) // self.step), self.ny - 2))
        for a, b, c in self.cell[(i, j)]:
            d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
            if abs(d) < 1e-9:
                continue
            l1 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d
            l2 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d
            l3 = 1 - l1 - l2
            if min(l1, l2, l3) >= -1e-6:
                return l1 * a[2] + l2 * b[2] + l3 * c[2]
        return self.hs[j][i]

    def slope(self, i, j):
        hs = self.hs
        c = [hs[j][i], hs[j][i + 1], hs[j + 1][i], hs[j + 1][i + 1]]
        return (max(c) - min(c)) / self.step, sum(c) / 4


# decals: patch meshes laid a little over the ground, their control points
# every DECAL_GRID units on it


DECAL_GRID = 32


def splat_patch(at, cx, cy, size, angle, shader, lift=1.5):
    """A splat: a square patch over the ground at cx cy, its picture (a
    disc, faded at its edge) laid once, turned by angle."""
    n = max(2, int(math.ceil(size / DECAL_GRID / 2))) * 2      # intervals, even
    a = math.radians(angle)
    ca, sa = math.cos(a), math.sin(a)
    rows = []
    for ii in range(n + 1):
        row = []
        for jj in range(n + 1):
            x = cx + (ii / n - 0.5) * size
            y = cy + (jj / n - 0.5) * size
            dx, dy = x - cx, y - cy
            s_ = 0.5 + (dx * ca + dy * sa) / size
            t_ = 0.5 - (-dx * sa + dy * ca) / size
            row.append((x, y, at(x, y) + lift, s_, t_))
        rows.append(row)
    return Patch(shader, rows)


def road_patches(at, pts, width, shader, along, lift=2.0):
    """A road: a strip of patch along each stretch of its line, the road's
    picture across the strip's width (its edges faded) and repeating along."""
    out = []
    dist = 0.0
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        L = math.hypot(bx - ax, by - ay)
        if L < 1:
            continue
        ux, uy = (bx - ax) / L, (by - ay) / L
        nx_, ny_ = -uy, ux
        # a little past each end, so that stretches overlap at a bend
        start, end = -width * 0.25, L + width * 0.25
        chunk = DECAL_GRID * 30
        p = start
        while p < end - 1:
            q = min(end, p + chunk)
            n = max(2, int(math.ceil((q - p) / DECAL_GRID / 2)) * 2)
            rows = []
            for ii in range(n + 1):
                d = p + (q - p) * ii / n
                row = []
                for jj, f in enumerate((-0.5, -0.25, 0, 0.25, 0.5)):
                    x = ax + ux * d + nx_ * f * width
                    y = ay + uy * d + ny_ * f * width
                    row.append((x, y, at(x, y) + lift, f + 0.5, (dist + d) / along))
                rows.append(row)
            out.append(Patch(shader, rows))
            p = q
        dist += L
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
