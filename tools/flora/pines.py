"""The winter woods' trees from a collection of modelled pines (tools/flora/flora.py).

The game's winter trees are grown as spruces by flora.py. When the player has a
pine collection of their own (OBJ meshes with their textures, not ours to
publish: they are read where the player keeps them and never committed), they
are made from it instead:

    OPM_PINES=<folder> tools/flora/flora.py --out DIR

The folder (default: "D:\\Medal of Honor\\models\\Realistic Pine Conifer Tree
Collection" as WSL sees it) holds model_0.obj .. model_6.obj, one a material
over the whole row of trees standing side by side: model_0/1 the bark (tiling
bark_albedo), model_2..5 the needle cards (branch atlas, its alpha apart in
branch_alpha), model_6 the root flares (trunks atlas). Each OBJ is triangle
strips written out as triangles, half of them degenerate: those go.

The row is taken apart into its trees: every trunk standing on the ground is
one, and each other piece goes to the tree it touches, grown out from the
trunks piece by piece. A tree is then turned z up, wound the way the game's
renderers cull (clockwise from outside), stood at the model's origin, scaled
to the height of the game model it replaces (its spread let out or taken in a
little toward the original's), and slimmed for the game:

- its bark simplified (meshoptimizer, which keeps a subset of the vertexes, so
  the bark's mapping is untouched), the needle cards' bends too;
- half the needle cards deep inside its crown dropped (never seen, drawn all
  the same, in every shadow map too), as flora.py does for its conifers;
- the cards that lie facing the sky showing a copy of the atlas with snow
  lying on the needles (paint._snow_load), as the spruces' do;
- and a far model: its stem only, and one in three of the cards round the
  crown's edge, larger (r_floraLod).

The wind and the snow on the bark come from the engine, as for every
opm_flora tree (the static models named tree_*, shaders opm_flora/*_bark*).
"""

import hashlib
import math
import os
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components
from scipy.spatial import cKDTree

import paint

DEFAULT = "/mnt/d/Medal of Honor/models/Realistic Pine Conifer Tree Collection"
MAX_VERTS, MAX_TRIS = 1000, 2000
BARK_PARTS, CARD_PARTS, ROOT_PARTS = (0, 1), (2, 3, 4, 5), (6,)

# which tree of the row (counted from the left, -x, as the trees are found) becomes
# which game model: the young ones are bushy cones, the grown ones a tall bare stem
# under a crown, the dead ones snags
PINES = {
    "spruce_tall": dict(tree=8, lod=900.0),    # 20 m, two tiers of crown
    "spruce_fill": dict(tree=7, lod=900.0),    # 19 m, full to low down
    "spruce_mid": dict(tree=5, lod=800.0),     # 14 m, bushy
    "spruce_small": dict(tree=3, lod=650.0),   # 9 m, a young cone
    # the bare stems of tall ones whose crowns are overhead, out of sight (1465 of them on
    # m6l1a): a trunk only, from a dying one and a dead one
    "stem_thin": dict(tree=14, lod=700.0, bare=True),
    "stem_thick": dict(tree=12, lod=700.0, bare=True),
}


def source():
    path = os.environ.get("OPM_PINES", DEFAULT)
    return path if os.path.exists(os.path.join(path, "model_0.obj")) else None


def _meshopt():
    """meshoptimizer, installed once into a venv beside the downloads if need be."""
    try:
        import meshoptimizer
        return meshoptimizer
    except ImportError:
        pass
    venv = os.path.join(paint.CACHE, "..", "flora-venv")
    py = os.path.join(venv, "bin", "python")
    if not os.path.exists(py):
        subprocess.run([sys.executable, "-m", "venv", venv], check=True)
    site = subprocess.run([py, "-c", "import site; print(site.getsitepackages()[0])"], capture_output=True,
                          text=True, check=True).stdout.strip()
    if not os.path.exists(os.path.join(site, "meshoptimizer")):
        subprocess.run([py, "-m", "pip", "install", "-q", "meshoptimizer"], check=True)
    sys.path.append(site)
    import meshoptimizer
    return meshoptimizer


# --------------------------------------------------------------------------- reading

def _read_obj(path):
    V, T, N, F = [], [], [], []
    with open(path) as f:
        for line in f:
            if line.startswith("v "):
                V.append(line.split()[1:4])
            elif line.startswith("vt "):
                T.append(line.split()[1:3])
            elif line.startswith("vn "):
                N.append(line.split()[1:4])
            elif line.startswith("f "):
                F.append([c.split("/")[0] for c in line.split()[1:4]])
    V, T, N = np.array(V, np.float64), np.array(T, np.float64), np.array(N, np.float64)
    f = np.array(F, np.int64) - 1  # (the files index vertex, uv and normal alike)
    a = np.cross(V[f[:, 1]] - V[f[:, 0]], V[f[:, 2]] - V[f[:, 0]])
    ok = (f[:, 0] != f[:, 1]) & (f[:, 1] != f[:, 2]) & (f[:, 0] != f[:, 2]) & (np.linalg.norm(a, axis=1) > 2e-10)
    return V, T, N, f[ok]


def load(src):
    """{part: (positions, uvs, normals, triangles)}, cached beside the downloads."""
    key = hashlib.sha1("".join("%s:%d:%d" % (n, os.path.getsize(os.path.join(src, n)),
                                             int(os.path.getmtime(os.path.join(src, n))))
                               for n in sorted(os.listdir(src)) if n.endswith(".obj")).encode()).hexdigest()[:12]
    cache = os.path.join(paint.CACHE, "pines_%s.npz" % key)
    if os.path.exists(cache):
        z = np.load(cache)
        return {i: (z["V%d" % i], z["T%d" % i], z["N%d" % i], z["F%d" % i]) for i in range(7)}
    parts = {i: _read_obj(os.path.join(src, "model_%d.obj" % i)) for i in range(7)}
    os.makedirs(paint.CACHE, exist_ok=True)
    np.savez(cache, **{"%s%d" % (k, i): parts[i][j] for i in parts for j, k in enumerate("VTNF")})
    return parts


def pieces(parts):
    """[(part, triangle indexes)]: each part's connected pieces."""
    out = []
    for i, (V, T, N, f) in parts.items():
        n = len(V)
        r = np.concatenate([f[:, 0], f[:, 1]])
        c = np.concatenate([f[:, 1], f[:, 2]])
        _, lab = connected_components(coo_matrix((np.ones(len(r)), (r, c)), shape=(n, n)), directed=False)
        fl = lab[f[:, 0]]
        order = np.argsort(fl, kind="stable")
        for grp in np.split(order, np.flatnonzero(np.diff(fl[order])) + 1):
            out.append((i, grp))
    return out


def trees(parts):
    """The row taken apart: [(foot (x, z), [(part, triangle indexes)])], left to right."""
    ps = pieces(parts)
    verts = []
    for i, grp in ps:
        V, _, _, f = parts[i]
        verts.append(V[np.unique(f[grp])])
    # the trunks: bark standing on the ground and reaching up
    feet = []
    owner = np.full(len(ps), -1)
    for k, ((i, grp), v) in enumerate(zip(ps, verts)):
        if i in BARK_PARTS and np.ptp(v[:, 1]) > 1.5 and v[:, 1].min() < 4.0:
            foot = v[v[:, 1] < v[:, 1].min() + 0.5][:, [0, 2]].mean(0)
            near = [t for t, p in enumerate(feet) if np.linalg.norm(p - foot) < 1.0]
            if near:
                owner[k] = near[0]
            else:
                owner[k] = len(feet)
                feet.append(foot)
    # every other piece to the tree it touches, the nearest first
    for reach in (0.02, 0.05, 0.1, 0.2, 0.4, 0.8, 1.6, 3.2, 6.4):
        while True:
            have = np.flatnonzero(owner >= 0)
            pts = np.concatenate([verts[k][::max(1, len(verts[k]) // 400)] for k in have])
            who = np.concatenate([np.full(len(verts[k][::max(1, len(verts[k]) // 400)]), owner[k]) for k in have])
            kd = cKDTree(pts)
            added = 0
            for k in np.flatnonzero(owner < 0):
                d, j = kd.query(verts[k][::max(1, len(verts[k]) // 24)])
                if d.min() < reach:
                    owner[k] = who[j[np.argmin(d)]]
                    added += 1
            if not added:
                break
    order = np.argsort([p[0] for p in feet])
    return [(feet[t], [ps[k] for k in np.flatnonzero(owner == t)]) for t in order]


# --------------------------------------------------------------------------- shaping

class Mesh:
    """Triangles in surfaces the engine takes (1000 vertexes, 2000 triangles each)."""

    def __init__(self, name):
        self.name = name
        self.surfaces = []

    def add(self, pos, nrm, uv, tris):
        """Whole pieces to a surface while they fit; a piece too big is cut."""
        tris = np.asarray(tris)
        if not len(tris):
            return
        used = np.unique(tris)
        if len(used) > MAX_VERTS or len(tris) > MAX_TRIS:
            # (cut where its vertexes fill a surface: its triangles run in strips, so
            # neighbours share them and the cuts are few)
            start, seen = 0, set()
            for j, tri in enumerate(tris.tolist()):
                new = seen | set(tri)
                if len(new) > MAX_VERTS - 3 or j - start >= MAX_TRIS:
                    self.add(pos, nrm, uv, tris[start:j])
                    start, new = j, set(tri)
                seen = new
            self.add(pos, nrm, uv, tris[start:])
            return
        if not self.surfaces or len(self.surfaces[-1][0]) + len(used) > MAX_VERTS \
                or len(self.surfaces[-1][1]) + len(tris) > MAX_TRIS:
            self.surfaces.append(([], []))
        v, t = self.surfaces[-1]
        remap = {int(o): len(v) + n for n, o in enumerate(used)}
        v.extend((pos[o], nrm[o], (float(uv[o][0]), float(uv[o][1]))) for o in used)
        t.extend((remap[int(a)], remap[int(b)], remap[int(c)]) for a, b, c in tris)

    def count(self):
        return sum(len(t) for _, t in self.surfaces)


def simplify(mo, pos, tris, error, prune=False):
    """tris (n, 3) simplified to within error (model units), the vertexes a subset."""
    if not len(tris):
        return tris
    idx = tris.astype(np.uint32).ravel()
    out = np.zeros_like(idx)
    opts = mo.SIMPLIFY_ERROR_ABSOLUTE | (mo.SIMPLIFY_PRUNE if prune else 0)
    n = mo.simplify(out, idx, pos.astype(np.float32), target_index_count=0, target_error=error, options=opts)
    return out[:n].reshape(-1, 3).astype(np.int64)


def axis_tube(pts, height, rings=8, sides=5, ground=0.0):
    """(positions, normals, uvs, triangles): a tube through the middle of the points, as wide
    as they reach round it at each height, the bark mapped once round it."""
    z0, z1 = pts[:, 2].min(), pts[:, 2].max()
    pos, nrm, uv, tris = [], [], [], []
    low = z0  # (measured from where the bark starts; drawn down to the ground)
    along = 0.0
    last = None
    for r in range(rings + 1):
        z = z0 + (z1 - z0) * r / rings
        near = pts[np.abs(pts[:, 2] - z) < (z1 - z0) / rings]
        if r == 0:
            z = min(low, ground)
        c = near[:, :2].mean(0) if len(near) else (last if last is not None else np.zeros(2))
        rad = np.percentile(np.hypot(*(near[:, :2] - c).T), 70) if len(near) > 3 else 1.0
        if r == rings:
            rad *= 0.3
        if last is not None:
            along += math.hypot(z1 - z0, 0) / rings
        last = c
        for j in range(sides + 1):
            a = 2 * math.pi * j / sides
            out = np.array([math.cos(a), math.sin(a), 0.0])
            pos.append(np.array([c[0], c[1], z]) + out * rad)
            nrm.append(out)
            uv.append((j / sides, -along / (2 * math.pi * max(rad, 1.0) * 2)))
    for r in range(rings):
        for j in range(sides):
            a, b = r * (sides + 1) + j, (r + 1) * (sides + 1) + j
            tris += [(a, b, a + 1), (a + 1, b, b + 1)]
    return np.array(pos), np.array(nrm), np.array(uv), np.array(tris)


def build(parts, tree, height, radius, lod, bare=False):
    """The meshes of one tree of the row: bark, bark_root, leaves, snow and their far ones
    (a bare one: its stem only, and far off a tube)."""
    mo = _meshopt()
    foot, ps = tree
    if bare:  # the stem itself: the bark close round its axis, from the ground to the top
        top = max(parts[i][0][np.unique(parts[i][3][g])][:, 1].max() for i, g in ps)
        def on_axis(i, g):
            # (an upright piece is the stem, however far it leans: a thin one's whole
            # length is one piece that strays well off the foot's line by its top)
            v = parts[i][0][np.unique(parts[i][3][g])]
            if np.ptp(v[:, 1]) > 1.5 * max(np.ptp(v[:, 0]), np.ptp(v[:, 2])):
                return True
            return np.percentile(np.hypot(v[:, 0] - foot[0], v[:, 2] - foot[1]), 90) < 0.03 * top
        ps = [(i, g) for i, g in ps if i not in CARD_PARTS and len(g) >= 40 and on_axis(i, g)]
    # z up, clockwise: file (x, y, z) -> model (x, -z, y), each triangle turned round
    allv = np.concatenate([parts[i][0][np.unique(parts[i][3][g])] for i, g in ps])
    top = allv[:, 1].max()
    k = height / top
    spread = np.percentile(np.hypot(allv[:, 0] - foot[0], allv[:, 2] - foot[1]), 99.5) * k
    stretch = float(np.clip(radius / max(spread, 1e-6), 0.8, 1.25))
    scale = np.array([k * stretch, k * stretch, k])

    def place(i):
        V, T, N, f = parts[i]
        p = np.stack([V[:, 0] - foot[0], -(V[:, 2] - foot[1]), V[:, 1]], 1) * scale
        n = np.stack([N[:, 0], -N[:, 2], N[:, 1]], 1) / scale * k
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
        uv = np.stack([T[:, 0], 1.0 - T[:, 1]], 1)
        return p, n, uv

    placed = {i: place(i) for i in {i for i, _ in ps}}
    bark, root, leaves, snow = Mesh("bark"), Mesh("bark_root"), Mesh("leaves"), Mesh("snow")
    bark_far, leaves_far, snow_far = Mesh("bark_far"), Mesh("leaves_far"), Mesh("snow_far")
    centre = np.array([0.0, 0.0, height * 0.6])
    grain = height / 900.0  # (errors are given for a tree 900 units tall)

    # the bark and the roots: simplified, the stem alone for far off 
    for i in BARK_PARTS + ROOT_PARTS:
        grps = [g for j, g in ps if j == i]
        if not grps:
            continue
        p, n, uv = placed[i]
        f = parts[i][3]
        tris = f[np.concatenate(grps)][:, [0, 2, 1]]
        near = simplify(mo, p, tris, 1.5 * grain)
        (root if i in ROOT_PARTS else bark).add(p, n, uv, near)
        if i in ROOT_PARTS and not bare:  # (the stem's foot, which the long pieces start above)
            bark_far.add(p, n, uv, simplify(mo, p, tris, 6.0 * grain))
        if i in BARK_PARTS and not bare:
            stems = [g for g in grps if np.ptp(p[np.unique(f[g])][:, 2]) > height * 0.1]
            if stems:
                bark_far.add(p, n, uv, simplify(mo, p, f[np.concatenate(stems)][:, [0, 2, 1]], 6.0 * grain))
    if bare:
        # far off, a bare stem is a five-sided tube up its axis (its bark comes in many
        # short pieces that do not simplify)
        pts = np.concatenate([placed[i][0][np.unique(parts[i][3][g])] for i, g in ps if i in BARK_PARTS])
        bark_far.add(*axis_tube(pts, height, ground=-0.02 * height))

    # the needle cards: each piece one card
    cards = []
    for i, g in ps:
        if i not in CARD_PARTS or bare:
            continue
        p, n, uv = placed[i]
        tris = parts[i][3][g][:, [0, 2, 1]]
        used = np.unique(tris)
        c = p[used].mean(0)
        a = np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]])
        up = abs(a.sum(0)[2]) / max(np.linalg.norm(a.sum(0)), 1e-9)
        cards.append((i, tris, used, c, up, np.linalg.norm(a, axis=1).sum() * 0.5))
    if cards:
        cz = np.array([c[3][2] for c in cards])
        cr = np.array([math.hypot(c[3][0], c[3][1]) for c in cards])
        bins = np.clip(((cz - cz.min()) / max(np.ptp(cz), 1e-6) * 12).astype(int), 0, 11)
        reach = np.array([cr[bins == b].max() if (bins == b).any() else 1.0 for b in range(12)])
        outer = cr / np.maximum(reach[bins], 1e-6)
        for n_, (i, tris, used, c, up, area) in enumerate(cards):
            if outer[n_] < 0.5 and n_ % 2:
                continue
            p, nrm, uv = placed[i]
            # lit as a rounded crown, facing up rather than down (they are seen from both sides)
            nn = nrm[used] * (1.0 if (nrm[used][:, 2].mean() >= 0) else -1.0)
            out = p[used] - centre
            out /= np.maximum(np.linalg.norm(out, axis=1, keepdims=True), 1e-9)
            nn = nn * 0.65 + out * 0.35
            nrm[used] = nn / np.maximum(np.linalg.norm(nn, axis=1, keepdims=True), 1e-9)
            (snow if up > 0.55 else leaves).add(p, nrm, uv, simplify(mo, p, tris, 2.0 * grain))
            # far off: one in three of those round the crown's edge, larger, drawn in a
            # little so the crown keeps its outline
            if outer[n_] > 0.6 and n_ % 3 == 0:
                big = p.copy()
                grow = 2.0
                size = math.sqrt(area)
                inward = np.array([c[0], c[1], 0.0])
                inward = inward / max(np.linalg.norm(inward), 1e-9) * size * (grow - 1) * 0.4
                big[used] = c + (p[used] - c) * grow - inward
                (snow_far if up > 0.55 else leaves_far).add(big, nrm, uv, simplify(mo, big, tris, 6.0 * grain))
    meshes = [bark, root, leaves, snow]
    if lod:
        meshes += [bark_far, leaves_far, snow_far]
    return [m for m in meshes if m.count()]


# --------------------------------------------------------------------------- textures

def snow_load(rgb, alpha, nrm, needles, P, rng):
    """Snow lying on the needle tufts of an atlas whose sprays are about P pixels across:
    the needles coated white where the tufts are thick, the gaps filled only at their
    dense hearts, broken up unevenly, so the needles' own strands still show round it
    (filled whole, each tuft became a ball of cotton wool)."""
    S = alpha.shape[0]
    dense = ndimage.gaussian_filter(needles / 255.0, P * 0.012)
    broken = ndimage.gaussian_filter(rng.random((S, S)).astype(np.float32), P * 0.02)
    broken = (broken - broken.mean()) / max(broken.std(), 1e-6)
    fine = ndimage.gaussian_filter(rng.random((S, S)).astype(np.float32), P * 0.003)
    fine = (fine - fine.mean()) / max(fine.std(), 1e-6)
    on = needles > 64
    if not on.any():
        return
    lay = dense + 0.10 * broken + 0.04 * fine
    coat = np.clip((lay - np.percentile(lay[on], 35)) / 0.10, 0.0, 1.0) * (needles / 255.0)
    heart = np.clip((lay - np.percentile(lay[on], 80)) / 0.05, 0.0, 1.0)
    heart = np.where(heart * (0.75 + 0.25 * np.clip(fine, -1, 1)) > 0.55, heart, 0.0)
    m = np.maximum(coat * 0.9, heart)
    depth = np.clip((lay - np.percentile(lay[on], 60)) / 0.15, 0.0, 1.0)
    snow = np.array((222.0, 228.0, 240.0), np.float32)[None, None, :] * (0.78 + 0.22 * depth)[..., None]
    rgb[:] = rgb * (1 - m[..., None]) + snow * m[..., None]
    alpha[:] = np.maximum(alpha, heart * 255.0)
    nrm[:] = nrm * (1 - m[..., None]) + np.array((127.5, 127.5, 255.0), np.float32) * m[..., None]


def _img(src, name):
    return Image.open(os.path.join(src, name))


def textures(src, out, tex="pine"):
    """The pines' textures, at sizes the game can take: the needles (and a copy with snow
    lying on them), the bark and the root flares, each with its normal map."""
    os.makedirs(out, exist_ok=True)
    S = 2048
    col = np.asarray(_img(src, "branch_albedo_jpg-branch_alpha.png").convert("RGB").resize((S, S), Image.LANCZOS),
                     np.float32)
    alpha = np.asarray(_img(src, "branch_alpha.png").convert("L").resize((S, S), Image.LANCZOS), np.float32)
    nrm = np.asarray(_img(src, "branch_normal.png").convert("RGB").resize((S, S), Image.LANCZOS), np.float32)
    col = col * np.array((0.86, 0.90, 0.86), np.float32)  # (a cold grey-green in the winter light)
    Image.fromarray(np.dstack([col, alpha]).clip(0, 255).astype(np.uint8), "RGBA").save(
        os.path.join(out, tex + "_leaves.tga"))
    Image.fromarray(nrm.clip(0, 255).astype(np.uint8), "RGB").resize((1024, 1024), Image.LANCZOS).save(
        os.path.join(out, tex + "_leaves_n.tga"))
    # snow on the needle tufts, not on the bare twigs: where the picture is green
    green = (col[..., 1] > col[..., 0] * 1.02) & (alpha > 64)
    needles = alpha * ndimage.binary_dilation(green, iterations=3)
    rgb, a, n = col.copy(), alpha.copy(), nrm.copy()
    snow_load(rgb, a, n, needles, 680, np.random.default_rng(71))
    rgb = paint.bleed(rgb, a, 8)
    Image.fromarray(np.dstack([rgb, a]).clip(0, 255).astype(np.uint8), "RGBA").save(
        os.path.join(out, tex + "_snow.tga"))
    Image.fromarray(n.clip(0, 255).astype(np.uint8), "RGB").resize((1024, 1024), Image.LANCZOS).save(
        os.path.join(out, tex + "_snow_n.tga"))
    for name, fn, size in (("bark", "bark", (512, 1024)), ("bark_root", "trunks", (1024, 1024))):
        _img(src, fn + "_albedo.png").convert("RGB").resize(size, Image.LANCZOS).save(
            os.path.join(out, "%s_%s.tga" % (tex, name)))
        _img(src, fn + "_normal.png").convert("RGB").resize(size, Image.LANCZOS).save(
            os.path.join(out, "%s_%s_n.tga" % (tex, name)))


def shader_text(name, meshes, lod, tex="pine"):
    out = ""
    for m in meshes:
        far = m.name.endswith("_far")
        kind = m.name[:-4] if far else m.name
        img = "textures/opm/flora/%s_%s.tga" % (tex, kind)
        fade = ""
        if lod:
            fade = "\t\talphaGen %s %d %d\n" % ("oneMinusDistFade" if far else "distFade", lod, lod * 0.25)
        if kind in ("leaves", "snow"):
            out += """opm_flora/%(n)s_%(m)s
{
	qer_editorimage %(img)s
	cull none
	surfaceparm foliage
	{
		map %(img)s
		depthWrite
		alphaFunc GE128
		rgbGen vertex
%(fade)s	}
}
""" % {"n": name, "m": m.name, "img": img, "fade": fade}
        else:
            out += """opm_flora/%(n)s_%(m)s
{
	qer_editorimage %(img)s
	{
		map %(img)s
		rgbGen vertex
%(fade)s	}
}
""" % {"n": name, "m": m.name, "img": img, "fade": fade}
    return out
