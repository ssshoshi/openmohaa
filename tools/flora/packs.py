"""The bocage's birch and common tree from modelled tree packs (tools/flora/flora.py).

flora.py grows the birch and the common tree (a beech) itself. When the player
has tree packs of their own (meshes with their textures, not ours to publish:
read where the player keeps them and never committed), those two are made from
them instead, as pines.py does for the winter trees:

    OPM_TREES=<folder> tools/flora/flora.py --out DIR

The folder (default "D:\\Medal of Honor\\models\\Trees" as WSL sees it) holds

- Birch/Birch Pack.glb (and its textures/): a row of birches of every age, as
  four materials over the whole row: mesh_2 the white bark (tiling), mesh_3 the
  leaf cards (a grey atlas the engine it was made for tints: we paint it green,
  its twigs brown), mesh_0 and mesh_1 cards of knotted white and dark bark laid
  on the stems (alpha tested). Vertex colours are wind masks, not colour.
- Common Trees/source/Trees collection.fbx (and its textures/): EZ-Tree trees,
  each its own pair of objects, "Tree EZTreeN.<name> branches" and "... leaves",
  their textures embedded. Blender turns it into glTF once (remodel.via_blender).

A birch is found in its row by its trunk, and every other piece goes to the
tree it touches (pines.trees' way); a common tree is picked by its name. Then,
as for the pines: z up, wound clockwise from outside (the game culls GL_FRONT),
stood on the origin and scaled to the game model it replaces (its spread let
out or taken in a little toward the original's), the stem drawn down below the
origin for slopes, the bark simplified, half the inner leaf cards dropped, and
a far model of the stem and the crown's outer cards made larger (r_floraLod).
"""

import math
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components
from scipy.spatial import cKDTree

import paint
import pines

DEFAULT = "/mnt/d/Medal of Honor/models/Trees"
Image.MAX_IMAGE_PIXELS = None

# which tree of a pack becomes which game model. birch: the trees of the row counted
# left to right (-z first: see trees()); common: the object's name.
#   stretch: how far its spread may be let out or taken in toward the original's
#   sink: how far its stem reaches below the origin (the stock trees go 20-70 down)
#   far: (every how many of the crown's outer cards the far model keeps, how much larger),
#        or far_lod: the pack's own simpler model of it for far off (the bushes have them)
#   thin: drop half the cards deep inside the crown (a tree's; a bush has few to spare)
#   base: for a model flora.py has no kind of its own for: the kind whose size and bone
#         it takes, and replaces: the game models it becomes
PACKS = {
    # the tallest of the row: a straight white stem, its crown high and narrow
    "birch": dict(pack="birch", tree=5, lod=900.0, sink=64.0, stretch=(0.7, 1.25),
                  far=(2, 1.6)),  # (its cards are slim sprays: more of them, grown less)
    # a young ash (pinnate leaves: the pack's other trees of that size have oak leaves, and
    # the oak is a kind of its own): a slim stem up into a loose crown, as in every hedge
    "beech": dict(pack="common", tree="Medium002", lod=650.0, sink=40.0, stretch=(0.75, 1.25),
                  far=(3, 1.9)),
    # the oak: a broad full crown of oak leaves on a short stem; the maps set it from 0.6 to 2.1
    # times its size, so from 1.2 (SWAPS) it is a grown oak's model, not a young one's blown up
    "oak": dict(pack="common", tree="Medium010", lod=1100.0, sink=64.0, stretch=(0.75, 1.4),
                far=(3, 1.9)),
    "oak_large": dict(pack="common", tree="Large009", lod=1100.0, sink=64.0, stretch=(0.75, 1.4),
                      far=(3, 1.9), base="oak", replaces=["tree_oak_large"], tex="oak"),
    # the bushes, from the bush pack (big, medium and small, five of each), each matched to
    # the shape of the stock bush it replaces
    "hazel": dict(pack="bushes", tree="Bush_big_004", far_lod=2, lod=450.0, sink=12.0,
                  stretch=(0.8, 1.6), thin=False),  # bush_full: big and round
    "hazel_hw": dict(pack="bushes", tree="Bush_big_002", far_lod=2, lod=450.0, sink=12.0,
                     stretch=(0.8, 1.6), thin=False, base="hazel",
                     replaces=["bush_full_heavyweather"]),  # (bush_full's copy for stormy maps)
    "buckthorn": dict(pack="bushes", tree="Bush_big_005", far_lod=2, lod=450.0, sink=12.0,
                      stretch=(0.75, 1.3), thin=False),  # tall and narrow
    "hornbeam": dict(pack="bushes", tree="Bush_medium_005", far_lod=2, lod=350.0, sink=8.0,
                     stretch=(0.8, 1.3), thin=False, replaces=["bush_sparseshrub"]),
    "sapling": dict(pack="bushes", tree="Bush_medium_002", far_lod=2, lod=350.0, sink=8.0,
                    stretch=(0.8, 1.3), thin=False, base="hornbeam",
                    replaces=["bush_sappling"]),  # (it shared sparseshrub's model: now one of its own)
    "blackthorn": dict(pack="bushes", tree="Bush_small_003", far_lod=2, lod=280.0, sink=6.0,
                       stretch=(0.8, 1.5), thin=False),  # bush_regularbush: low and wide
    # bush_regularbush is mostly set 3 to 10 times its size (m3l2, m3l3, m5l2b: a shrub mass
    # 250 units tall), where a small bush blown up is a giant one: from 2 (SWAPS) a big bush
    "blackthorn_large": dict(pack="bushes", tree="Bush_big_003", far_lod=2, lod=900.0, sink=6.0,
                             stretch=(0.8, 1.6), thin=False, base="blackthorn",
                             replaces=["bush_regularbush_large"]),
}


# the models shown in place of another from a scale up (models/static/opm_swaps.txt, read
# by the renderers: renderercommon/tr_modelswaps.c): (game model, scale, pack model)
SWAPS = [("tree_oak", 1.2, "oak_large"), ("bush_regularbush", 2.0, "blackthorn_large")]


def swaps_text(made):
    """opm_swaps.txt for the pack models that were made (names in `made`)."""
    lines = ["// OPM (tools/flora/packs.py): a static model shown as another from a scale up",
             "// model            at least   instead"]
    for game, scale, name in SWAPS:
        if name in made:
            for other in PACKS[name].get("replaces", []):
                lines.append("%-18s %-10g %s" % (game + ".tik", scale, other + ".tik"))
    return "\n".join(lines) + "\n" if len(lines) > 2 else ""


def root():
    return os.environ.get("OPM_TREES", DEFAULT)


def _birch_file():
    return os.path.join(root(), "Birch", "Birch Pack.glb")


def _common_file():
    return os.path.join(root(), "Common Trees", "source", "Trees collection.fbx")


def _bushes_file():
    # (beside the trees' folder, or in it)
    for d in (os.path.dirname(os.path.normpath(root())), root()):
        f = os.path.join(d, "Bushes", "source", "Bush models pack.fbx")
        if os.path.exists(f):
            return f
    return os.path.join(root(), "Bushes", "source", "Bush models pack.fbx")


def _file(pack):
    return {"birch": _birch_file, "common": _common_file, "bushes": _bushes_file}[pack]()


def available(name):
    cfg = PACKS.get(name)
    return bool(cfg) and os.path.exists(_file(cfg["pack"]))


def extra(species):
    """The models made only from packs (no kind among flora.py's species): name -> config."""
    return {n: c for n, c in PACKS.items() if n not in species and available(n)}


def _remodel():
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "weapons"))
    import remodel
    return remodel


# --------------------------------------------------------------------------- reading

class Part:
    """One material of a tree: kind (bark, knots, leaves), positions (file space, y up),
    normals, uvs (t down the image), triangles, and its maps (name -> PIL image)."""

    def __init__(self, kind, pos, nrm, uv, tris, maps, tex):
        self.kind, self.pos, self.nrm, self.uv, self.tris, self.maps, self.tex = kind, pos, nrm, uv, tris, maps, tex

    def subset(self, tris):
        return Part(self.kind, self.pos, self.nrm, self.uv, tris, self.maps, self.tex)


_loaded = {}


def load(pack):
    """[Part] of the whole pack, each a material over all its trees."""
    if pack in _loaded:
        return _loaded[pack]
    rm = _remodel()
    out = []
    if pack == "birch":
        kinds = {"mesh_0": ("knots", "knots"), "mesh_1": ("knots", "knots_dark"),
                 "mesh_2": ("bark", "bark"), "mesh_3": ("leaves", "leaves")}
        for p in rm.read_glb(_birch_file()):
            kind, tex = kinds[p.material]
            out.append((p.name, Part(kind, p.pos, p.normal, p.uv, p.tris, p.maps, tex)))
    elif pack == "bushes":
        # "Bush_big_004_LOD0" etc.: its textures are not linked in the file, they lie beside it
        tex = os.path.join(os.path.dirname(os.path.dirname(_bushes_file())), "textures")
        img = lambda n: Image.open(os.path.join(tex, n))  # noqa: E731
        maps = {"leaves": {"base": img("Brunch_clusters_RGBA.png"), "normal": img("Brunch_clusters_Normal.png")},
                "bark": {"base": img("Bush_bark_basecolor.png"), "normal": img("Bush_bark_normal.png"),
                         "ao": img("Bush_bark_ambientocclusion.png")}}
        for p in rm.read_glb(rm.via_blender([_bushes_file()], "flora-bushes")):
            if "_LOD" not in p.name:
                continue  # (the billboards, the ground and the man for scale)
            kind = "leaves" if p.material.startswith("Brunch") else "bark"
            out.append((p.name, Part(kind, p.pos, p.normal, p.uv, p.tris, maps[kind], kind)))
    else:
        glb = rm.via_blender([_common_file()], "flora-common")
        for p in rm.read_glb(glb):
            kind = "leaves" if p.material.startswith("leaves") else "bark"
            # "Tree EZTree0.Medium010" -> "Medium010"
            out.append((p.name.split(".", 1)[1].split()[0], Part(kind, p.pos, p.normal, p.uv, p.tris, p.maps, kind)))
    _loaded[pack] = out
    return out


def _pieces(part):
    """part's triangles grouped into connected pieces."""
    n, f = len(part.pos), part.tris
    r = np.concatenate([f[:, 0], f[:, 1]])
    c = np.concatenate([f[:, 1], f[:, 2]])
    _, lab = connected_components(coo_matrix((np.ones(len(r)), (r, c)), shape=(n, n)), directed=False)
    fl = lab[f[:, 0]]
    order = np.argsort(fl, kind="stable")
    return np.split(order, np.flatnonzero(np.diff(fl[order])) + 1)


def birch_trees(parts):
    """The birch row taken apart: [[Part]] a tree, left to right along the row (z).
    A tree is a bark piece standing on the ground (the stems of the small ones too);
    every other piece goes to the tree it touches, the nearest first."""
    ps = []
    for _, part in parts:
        for grp in _pieces(part):
            ps.append((part, grp, part.pos[np.unique(part.tris[grp])]))
    feet, owner = [], np.full(len(ps), -1)
    for k, (part, grp, v) in enumerate(ps):
        if part.kind == "bark" and np.ptp(v[:, 1]) > 0.3 and v[:, 1].min() < 0.3:
            foot = v[v[:, 1] < v[:, 1].min() + 0.2][:, [0, 2]].mean(0)
            near = [t for t, p in enumerate(feet) if np.linalg.norm(p - foot) < 0.6]
            owner[k] = near[0] if near else len(feet)
            if not near:
                feet.append(foot)
    for reach in (0.01, 0.03, 0.1, 0.2, 0.4):  # (no further: a sapling's leaves stay its own)
        while True:
            have = np.flatnonzero(owner >= 0)
            step = [max(1, len(ps[k][2]) // 400) for k in have]
            pts = np.concatenate([ps[k][2][::s] for k, s in zip(have, step)])
            who = np.concatenate([np.full(len(ps[k][2][::s]), owner[k]) for k, s in zip(have, step)])
            kd = cKDTree(pts)
            added = 0
            for k in np.flatnonzero(owner < 0):
                d, j = kd.query(ps[k][2][::max(1, len(ps[k][2]) // 24)])
                if d.min() < reach:
                    owner[k] = who[j[np.argmin(d)]]
                    added += 1
            if not added:
                break
    trees = []
    for t in np.argsort([p[1] for p in feet]):
        mine = [ps[k] for k in np.flatnonzero(owner == t)]
        by = {}
        for part, grp, _ in mine:
            by.setdefault(id(part), (part, []))[1].append(grp)
        trees.append([part.subset(part.tris[np.concatenate(g)]) for part, g in by.values()])
    return trees


def tree_parts(name, lod=0):
    cfg = PACKS[name]
    parts = load(cfg["pack"])
    if cfg["pack"] == "birch":
        return birch_trees(parts)[cfg["tree"]]
    if cfg["pack"] == "bushes":
        return [p for n, p in parts if n == "%s_LOD%d" % (cfg["tree"], lod)]
    return [p for n, p in parts if n == cfg["tree"]]


def describe(pack):
    """What a pack holds, for choosing: index or name, height and spread."""
    parts = load(pack)
    trees = birch_trees(parts) if pack == "birch" else \
        [(n, [p for m, p in parts if m == n]) for n in dict.fromkeys(n for n, _ in parts)]
    for k, t in enumerate(trees):
        label, t = (k, t) if pack == "birch" else t
        v = np.concatenate([p.pos[np.unique(p.tris)] for p in t])
        foot = v[v[:, 1] < v[:, 1].min() + 0.2][:, [0, 2]].mean(0)
        spread = np.percentile(np.hypot(v[:, 0] - foot[0], v[:, 2] - foot[1]), 99.5)
        print("  %-10s height %6.2f  spread %5.2f  tris %s" % (
            label, np.ptp(v[:, 1]), spread, " ".join("%s %d" % (p.tex, len(p.tris)) for p in t)))


# --------------------------------------------------------------------------- shaping

def build(name, height, radius):
    """The meshes of one tree: bark, its knot cards, leaves, and their far ones."""
    cfg = PACKS[name]
    mo = pines._meshopt()
    parts = tree_parts(name)

    def stand(parts):
        """(base, foot): where it meets the ground, its stem's foot (a small bush's sprays
        are all it has)."""
        allv = np.concatenate([p.pos[np.unique(p.tris)] for p in parts])
        wood = [p.pos[np.unique(p.tris)] for p in parts if p.kind == "bark"]
        wood = np.concatenate(wood) if wood else allv
        base = wood[:, 1].min()
        return base, wood[wood[:, 1] < base + 0.02 * np.ptp(allv[:, 1])][:, [0, 2]].mean(0)

    allv = np.concatenate([p.pos[np.unique(p.tris)] for p in parts])
    base, foot = stand(parts)
    top = allv[:, 1].max() - base
    k = height / top
    spread = np.percentile(np.hypot(allv[:, 0] - foot[0], allv[:, 2] - foot[1]), 99.5) * k
    lo, hi = cfg["stretch"]
    stretch = float(np.clip(radius / max(spread, 1e-6), lo, hi))
    scale = np.array([k * stretch, k * stretch, k])
    sink, lod = cfg["sink"], cfg["lod"]

    def place(parts, base, foot):
        placed = []
        for p in parts:
            # z up: file (x, y, z) -> model (x, -z, y); clockwise from outside: each triangle turned
            pos = np.stack([p.pos[:, 0] - foot[0], -(p.pos[:, 2] - foot[1]), p.pos[:, 1] - base], 1) * scale
            n = np.stack([p.nrm[:, 0], -p.nrm[:, 2], p.nrm[:, 1]], 1) / scale * k
            n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
            tris = p.tris[:, [0, 2, 1]]
            if p.kind != "leaves":
                # the stem's foot drawn on down to `sink` under the origin, for slopes (the
                # renderer seats the tree on the lowest ground round it, R_SeatStaticFlora):
                # the band at its very bottom stretched, the rest as it is
                band = 0.015 * height
                z = pos[:, 2]
                low = z < band
                z[low] = -sink + (z[low] + 0.0) * (band + sink) / band
            placed.append((p, pos, n, tris))
        return placed

    placed = place(parts, base, foot)
    far_lod = cfg.get("far_lod")
    if far_lod:  # (the pack's own far model, made the same size, stood at its own foot)
        fparts = tree_parts(name, far_lod)
        placed += [(p, pos, n, tris, True) for p, pos, n, tris in place(fparts, *stand(fparts))]
    placed = [x if len(x) == 5 else x + (False,) for x in placed]

    bark, leaves = pines.Mesh("bark"), pines.Mesh("leaves")
    knots = {}  # (white and dark knot cards: an image each, so a surface each)
    bark_far, leaves_far = pines.Mesh("bark_far"), pines.Mesh("leaves_far")
    grain = height / 900.0  # (errors are given for a tree 900 units tall)
    centre = np.array([0.0, 0.0, height * 0.6])
    cards = []
    for p, pos, n, tris, far in placed:
        if far and p.kind == "bark":
            bark_far.add(pos, n, p.uv, tris)
        elif p.kind == "bark":
            bark.add(pos, n, p.uv, pines.simplify(mo, pos, tris, 1.0 * grain))
            # far off: the stem and the limbs, coarser (the twigs go)
            grps = [g for g in _pieces(p.subset(tris)) if not far_lod
                    and np.ptp(pos[np.unique(tris[g])][:, 2]) > height * 0.08]
            if grps:
                bark_far.add(pos, n, p.uv, pines.simplify(mo, pos, tris[np.concatenate(grps)], 5.0 * grain, prune=True))
        elif p.kind == "knots":
            knots.setdefault(p.tex, pines.Mesh("bark_" + p.tex)).add(pos, n, p.uv, tris)
        else:
            for g in _pieces(p.subset(tris)):
                t = tris[g]
                used = np.unique(t)
                a = np.cross(pos[t[:, 1]] - pos[t[:, 0]], pos[t[:, 2]] - pos[t[:, 0]])
                cards.append((p, pos, n, t, used, pos[used].mean(0), np.linalg.norm(a, axis=1).sum() * 0.5, far))
    if cards:
        cz = np.array([c[5][2] for c in cards])
        cr = np.array([math.hypot(c[5][0], c[5][1]) for c in cards])
        bins = np.clip(((cz - cz.min()) / max(np.ptp(cz), 1e-6) * 12).astype(int), 0, 11)
        reach = np.array([cr[bins == b].max() if (bins == b).any() else 1.0 for b in range(12)])
        outer = cr / np.maximum(reach[bins], 1e-6)
        for i, (p, pos, nrm, t, used, c, area, far) in enumerate(cards):
            if outer[i] < 0.5 and i % 2 and cfg.get("thin", True) and not far:  # (half the cards deep inside the crown: never seen)
                continue
            # lit as a rounded crown, facing up rather than down (they are seen from both sides)
            nn = nrm[used] * (1.0 if nrm[used][:, 2].mean() >= 0 else -1.0)
            out = pos[used] - centre
            out /= np.maximum(np.linalg.norm(out, axis=1, keepdims=True), 1e-9)
            nn = nn * 0.6 + out * 0.4
            nrm[used] = nn / np.maximum(np.linalg.norm(nn, axis=1, keepdims=True), 1e-9)
            if far:
                leaves_far.add(pos, nrm, p.uv, t)
                continue
            leaves.add(pos, nrm, p.uv, pines.simplify(mo, pos, t, 1.5 * grain))
            if far_lod:
                continue
            # far off: one in three of those round the crown's edge, larger, drawn in a
            # little so the crown keeps its outline
            every, grow = cfg["far"]  # noqa: F841
            if outer[i] > 0.55 and i % every == 0:
                big = pos.copy()
                inward = np.array([c[0], c[1], 0.0])
                inward = inward / max(np.linalg.norm(inward), 1e-9) * math.sqrt(area) * (grow - 1) * 0.4
                big[used] = c + (pos[used] - c) * grow - inward
                leaves_far.add(big, nrm, p.uv, pines.simplify(mo, big, t, 5.0 * grain))
    meshes = [bark] + [knots[t] for t in sorted(knots)] + [leaves]
    if lod:
        meshes += [bark_far, leaves_far]
    return [m for m in meshes if m.count()]


# --------------------------------------------------------------------------- textures

def _green_down(img):
    """img (RGB) with its green pointing down the image, as GL2's tangent space has it."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "weapons"))
    from garand import normal_map_green_down
    if normal_map_green_down(img):
        return img
    r, g, b = img.split()
    return Image.merge("RGB", (r, g.point(lambda v: 255 - v), b))


def _with_ao(base, ao, amount=0.5):
    """base (RGB float array) darkened by an occlusion map, amount of the way."""
    a = np.asarray(ao.convert("L").resize(base.shape[1::-1], Image.LANCZOS), np.float32) / 255.0
    return base * (1.0 - amount + amount * a)[..., None]


def _save(arr, mode, path, size=None):
    im = Image.fromarray(arr.clip(0, 255).astype(np.uint8), mode)
    if size and im.size != size:
        im = im.resize(size, Image.LANCZOS)
    im.save(path)


def _birch_leaves(maps, S=2048):
    """The birch's grey leaf atlas painted: the leaves a summer green (the grey's own shading
    kept), the twigs between them a dark brown-grey."""
    src = maps["base"].convert("RGBA").resize((S, S), Image.LANCZOS)
    rgba = np.asarray(src, np.float32)
    grey, alpha = rgba[..., :3].mean(2), rgba[..., 3]
    solid = alpha > 128
    shade = grey / max(np.median(grey[solid]), 1.0)
    # the twigs and branches: smooth flat grey (75-88, varying by 1.5-3), where the leaves
    # are lighter or veined (85-100, varying by 4-12)
    w = solid.astype(np.float32)
    sig = S / 512.0
    norm = np.maximum(ndimage.gaussian_filter(w, sig), 1e-3)
    mean = ndimage.gaussian_filter(grey * w, sig) / norm
    var = np.maximum(ndimage.gaussian_filter(grey * grey * w, sig) / norm - mean * mean, 0.0)
    sd = np.sqrt(var)
    wood = (((mean < 92.0) & (sd < 3.6)) | ((mean < 81.0) & (sd < 4.5))) & solid
    wood = ndimage.binary_opening(wood, iterations=2)
    leafy = 1.0 - ndimage.gaussian_filter(wood.astype(np.float32), 1.5)
    rng = np.random.default_rng(31)
    vary = ndimage.gaussian_filter(rng.random((S, S)).astype(np.float32), S / 64)
    vary = (vary - vary.mean()) / max(vary.std(), 1e-6)
    green = np.array((92.0, 122.0, 50.0), np.float32) + np.array((10.0, 6.0, -2.0), np.float32) * vary[..., None]
    twig = np.array((74.0, 62.0, 54.0), np.float32)
    col = (green * leafy[..., None] + twig * (1 - leafy[..., None])) * shade[..., None]
    if "ao" in maps:
        col = _with_ao(col, maps["ao"], 0.4)
    col = paint.bleed(col, alpha, 8)
    return np.dstack([col, alpha])


def _far_leaves(rgba):
    """The atlas for far off: each spray's leaves closed up into a solid clump. Thin sprays
    shrink away under alpha testing as the image's mips are averaged (a distant birch
    stood bare), so the far cards show what the crown's mass looks like from there.
    Only the leaves are clumped (green ones): bare twigs closed up became brown sheets."""
    S = rgba.shape[0]
    rgb = rgba[..., :3]
    solid = rgba[..., 3] > 128
    leaf = solid & (rgb[..., 1] > rgb[..., 0] * 1.08)
    r = max(2, S // 90)
    clump = ndimage.binary_closing(leaf, iterations=r, border_value=0)
    clump = ndimage.binary_fill_holes(clump)
    out = rgba.copy()
    out[..., 3] = np.where(clump | solid, 255.0, ndimage.gaussian_filter(clump.astype(np.float32), 1.0) * 255.0)
    # (the new parts the leaves' own colour round them, a little darker: the crown's inside)
    w = ndimage.gaussian_filter(leaf.astype(np.float32), r)
    fill = np.stack([ndimage.gaussian_filter(rgb[..., c] * leaf, r) for c in range(3)], -1) / np.maximum(w, 1e-4)[..., None]
    out[..., :3] = np.where(solid[..., None], rgb, fill * 0.8)
    return out


def texname(name):
    """Whose textures a model shows: the bushes all share the bush pack's."""
    return PACKS[name].get("tex") or ("bushpack" if PACKS[name]["pack"] == "bushes" else name)


def textures(name, out):
    """The tree's textures beside its shaders: <name>_<tex>.tga and _n.tga for each part."""
    os.makedirs(out, exist_ok=True)
    done = set()
    for p in tree_parts(name):
        if p.tex in done:
            continue
        done.add(p.tex)
        stem = os.path.join(out, "%s_%s" % (texname(name), p.tex))
        if os.path.exists(stem + ".tga"):
            continue  # (another of the pack's bushes wrote them)
        base = p.maps["base"]
        if p.kind == "leaves":
            if PACKS[name]["pack"] == "birch":
                rgba = _birch_leaves(p.maps)
            else:
                S = 1024
                rgba = np.asarray(base.convert("RGBA").resize((S, S), Image.LANCZOS), np.float32).copy()
                if PACKS[name]["pack"] == "bushes":  # (a darker green than the trees': as light as theirs)
                    rgba[..., :3] *= 1.25
                rgba[..., :3] = paint.bleed(rgba[..., :3], rgba[..., 3], 8)
            _save(rgba, "RGBA", stem + ".tga")
            _save(_far_leaves(rgba), "RGBA", stem + "_far.tga", (512, 512))
            Image.new("RGB", (64, 64), (128, 128, 255)).save(stem + "_far_n.tga")
            size = (1024, 1024)
        elif p.kind == "knots":
            rgba = np.asarray(base.convert("RGBA"), np.float32).copy()
            if "ao" in p.maps:
                rgba[..., :3] = _with_ao(rgba[..., :3], p.maps["ao"])
            rgba[..., :3] = paint.bleed(rgba[..., :3], rgba[..., 3], 8)
            size = (512, 1024) if base.size[1] > base.size[0] else (1024, 1024)
            _save(rgba, "RGBA", stem + ".tga", size)
        else:
            rgb = np.asarray(base.convert("RGB"), np.float32)
            if "ao" in p.maps:
                rgb = _with_ao(rgb, p.maps["ao"])
            size = base.size if max(base.size) <= 1024 else (512, 1024) if base.size[1] > base.size[0] else (512, 512)
            _save(rgb, "RGB", stem + ".tga", size)
        if "normal" in p.maps:
            _green_down(p.maps["normal"].convert("RGB")).resize(size, Image.LANCZOS).save(stem + "_n.tga")
        else:  # (flat: GL2 looks for <image>_n, and an old one of that name may be about)
            Image.new("RGB", (64, 64), (128, 128, 255)).save(stem + "_n.tga")


def shader_text(name, meshes, lod):
    out = ""
    for m in meshes:
        far = m.name.endswith("_far")
        kind = m.name[:-4] if far else m.name
        # (the far leaves have their own atlas, the sprays clumped: _far_leaves)
        img = "textures/opm/flora/%s_%s.tga" % (texname(name), kind[5:] if kind.startswith("bark_knots")
                                                 else m.name if kind == "leaves" else kind)
        fade = ""
        if lod:
            fade = "\t\talphaGen %s %d %d\n" % ("oneMinusDistFade" if far else "distFade", lod, lod * 0.25)
        if kind == "leaves":
            head, stage = "\tcull none\n\tsurfaceparm foliage\n", "\t\tdepthWrite\n\t\talphaFunc GE128\n"
        elif kind.startswith("bark_knots"):
            # the knotted patches laid on the stem: one-sided like the bark (a tree's
            # two-sided surfaces sway as leaves), cut out
            head, stage = "", "\t\tdepthWrite\n\t\talphaFunc GE128\n"
        else:
            head, stage = "", ""
        out += """opm_flora/%(n)s_%(m)s
{
\tqer_editorimage %(img)s
%(head)s\t{
\t\tmap %(img)s
%(stage)s\t\trgbGen vertex
%(fade)s\t}
}
""" % {"n": name, "m": m.name, "img": img, "fade": fade, "head": head, "stage": stage}
    return out
