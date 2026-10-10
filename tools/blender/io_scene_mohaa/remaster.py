"""Remastering game models: new geometry on the original's skeleton, UVs and textures.

The new shape comes from a generated mesh (TRELLIS, run outside Blender from renders made
here) or from any mesh an artist drops in. It carries no rig and no UVs, so everything else
is taken from the model it replaces:

  1. prepare: the model is imported, its arms lifted away from the body (a generator fuses
     limbs that touch the torso), and rendered with its textures from around;
  2. build: a candidate mesh is aligned to the posed model (rotation, scale, offset), cleaned
     and decimated to a budget, and cut into the original's surfaces: every new face goes to
     the surface nearest it, and takes its UVs from the same UV island of that surface, so the
     game's textures and normal maps (yours, upscaled) fit it. Weights and facial morphs are
     interpolated from the same surface, then the mesh is moved back from the lifted pose to
     the rest pose by inverting the skinning;
  3. every surface is checked against the one it replaces: how closely it follows its shape
     (both ways) and how much its edges stretch over the model's animations. A surface that
     fails is smoothed instead (subdivided, with sharp edges kept), and kept as it was when
     it cannot be.

The engine's limits are kept: at most 1000 vertices and 2000 triangles per surface (the
exporter splits bigger ones, under the same name, so the game's own .tik still shades them)
and 32 surfaces per model (MAX_MODEL_SURFACES: each entity's surface flags are sent in a
fixed array), so the budget shrinks until the split surfaces fit.

Props need no new shapes, only rounder ones: mode "subdivide" skips the candidates and rounds
every surface as the fallback does (smoothed_copy), with the same checks.

Coordinates here are armature space, which is raw model units (the importer puts the unit
scale on the armature object).
"""

import json
import math
import os

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

from . import anim as animutil
from .mohaa import skd

MAX_SURFACES = 32
PIECE_VERTS = skd.MAX_VERTS * 0.9  # a split repeats the vertices on its cuts


# ----------------------------------------------------------------------------- scene helpers

def model_meshes(arm):
    bpy.context.view_layer.update()  # objects linked since the last update are not listed before it
    return [o for o in bpy.data.objects if o.type == "MESH" and o.parent == arm and "mohaa_surface" in o
            and o.name in bpy.context.view_layer.objects]


def _arm_space(arm, obj):
    return arm.matrix_world.inverted() @ obj.matrix_world


def posed_geometry(arm, obj):
    """(vertices N x 3 in armature space as the scene shows them, triangles T x 3, loop
    indices T x 3) of a mesh object."""
    dg = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(dg)
    me = ev.to_mesh()
    try:
        co = np.empty(len(me.vertices) * 3, dtype=np.float64)
        me.vertices.foreach_get("co", co)
        co = co.reshape(-1, 3)
    finally:
        ev.to_mesh_clear()
    m = np.array(_arm_space(arm, obj))
    co = co @ m[:3, :3].T + m[:3, 3]
    me = obj.data
    me.calc_loop_triangles()
    tris = np.empty(len(me.loop_triangles) * 3, dtype=np.int64)
    loops = np.empty(len(me.loop_triangles) * 3, dtype=np.int64)
    me.loop_triangles.foreach_get("vertices", tris)
    me.loop_triangles.foreach_get("loops", loops)
    return co, tris.reshape(-1, 3), loops.reshape(-1, 3)


def bounds(points):
    lo, hi = points.min(0), points.max(0)
    return lo, hi, float(np.linalg.norm(hi - lo))


def clear_pose(arm):
    for pb in arm.pose.bones:
        pb.matrix_basis = Matrix.Identity(4)
    bpy.context.view_layer.update()


def spread_pose(arm, angle=30.0):
    """Lift the upper arms away from the body (rest pose otherwise). Model space faces +X
    with +Y to its left. Returns the names of the bones moved."""
    if arm.animation_data is not None:
        arm.animation_data.action = None
    clear_pose(arm)
    meshes = model_meshes(arm)
    if not meshes or angle <= 0.0:
        return []
    pts = np.concatenate([posed_geometry(arm, o)[0] for o in meshes])
    center_y = float((pts[:, 1].min() + pts[:, 1].max()) / 2.0)
    moved = []
    for pb in arm.pose.bones:
        if "mohaa_proxy_of" in pb.bone:
            continue
        if "upperarm" not in pb.name.lower().replace(" ", "").replace("_", ""):
            continue
        child = next((c for c in pb.children if "mohaa_proxy_of" not in c.bone), None)
        if child is None:
            continue
        head = pb.matrix.translation.copy()
        d = child.matrix.translation - head
        side = head.y - center_y
        if abs(side) < 1e-3 or d.length < 1e-6:
            continue
        axis = d.cross(Vector((0.0, math.copysign(1.0, side), 0.0)))
        if axis.length < 1e-6:
            continue
        axis.normalize()
        rot = Matrix.Rotation(math.radians(angle), 4, axis)
        pb.matrix = Matrix.Translation(head) @ rot @ Matrix.Translation(-head) @ pb.matrix
        bpy.context.view_layer.update()
        moved.append(pb.name)
    return moved


# ----------------------------------------------------------------------------- rendering

def render_views(objects, out_dir, size=1024, yaws=(0.0, 90.0, 180.0, 270.0), elevation=10.0, prefix="view"):
    """Textured renders of objects on a transparent background, the camera circling the
    model (yaw 0 looks at its front). Returns the image paths."""
    scene = bpy.context.scene
    hidden = []
    for o in scene.objects:
        if o.type in ("MESH", "CURVE") and o not in objects and not o.hide_render:
            o.hide_render = True
            hidden.append(o)
    for o in objects:
        o.hide_render = False
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    pts = []
    for o in objects:
        ev = o.evaluated_get(dg)
        for c in ev.bound_box:
            pts.append(tuple(o.matrix_world @ Vector(c)))
    lo, hi, _ = bounds(np.array(pts))
    center = Vector(((lo + hi) / 2.0).tolist())
    radius = float(np.linalg.norm(hi - lo)) / 2.0 or 1.0
    cam_data = bpy.data.cameras.new("remaster_cam")
    cam_data.lens_unit = "FOV"
    cam_data.angle = math.radians(40.0)
    cam = bpy.data.objects.new("remaster_cam", cam_data)
    scene.collection.objects.link(cam)
    dist = radius / math.sin(cam_data.angle / 2.0) * 1.05
    cam_data.clip_start = dist / 1000.0
    cam_data.clip_end = dist * 10.0
    scene.camera = cam
    scene.render.engine = "BLENDER_WORKBENCH"
    shading = scene.display.shading
    shading.light = "STUDIO"
    shading.color_type = "TEXTURE"
    shading.show_backface_culling = False
    scene.render.film_transparent = True
    scene.render.resolution_x = scene.render.resolution_y = size
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    os.makedirs(out_dir, exist_ok=True)
    paths = []
    try:
        for i, yaw in enumerate(yaws):
            y, e = math.radians(yaw), math.radians(elevation)
            cam.location = center + Vector((math.cos(y) * math.cos(e), math.sin(y) * math.cos(e), math.sin(e))) * dist
            cam.rotation_euler = (center - cam.location).to_track_quat("-Z", "Y").to_euler()
            path = os.path.join(out_dir, "%s_%02d.png" % (prefix, i))
            scene.render.filepath = path
            bpy.ops.render.render(write_still=True)
            paths.append(path)
    finally:
        bpy.data.objects.remove(cam, do_unlink=True)
        bpy.data.cameras.remove(cam_data)
        for o in hidden:
            o.hide_render = False
    return paths


def contact_sheet(rows, out_path):
    """One PNG of rows of equally sized images (row 0 at the top)."""
    imgs = [[bpy.data.images.load(p, check_existing=False) for p in row] for row in rows]
    w, h = imgs[0][0].size
    cols = max(len(r) for r in imgs)
    sheet = np.zeros((h * len(imgs), w * cols, 4), dtype=np.float32)
    sheet[..., :3] = 0.18
    sheet[..., 3] = 1.0
    for ri, row in enumerate(imgs):
        for ci, im in enumerate(row):
            px = np.empty(w * h * 4, dtype=np.float32)
            im.pixels.foreach_get(px)
            px = px.reshape(h, w, 4)
            a = px[..., 3:4]
            y0 = (len(imgs) - 1 - ri) * h  # Blender images start at the bottom row
            region = sheet[y0:y0 + h, ci * w:(ci + 1) * w]
            region[..., :3] = px[..., :3] * a + region[..., :3] * (1.0 - a)
    out = bpy.data.images.new("remaster_sheet", w * cols, h * len(imgs), alpha=True)
    out.pixels.foreach_set(sheet.ravel())
    out.filepath_raw = out_path
    out.file_format = "PNG"
    out.save()
    for row in imgs:
        for im in row:
            bpy.data.images.remove(im)
    bpy.data.images.remove(out)
    return out_path


# ----------------------------------------------------------------------------- geometry

def sample_surface(verts, tris, n, rng):
    a, b, c = verts[tris[:, 0]], verts[tris[:, 1]], verts[tris[:, 2]]
    area = 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1)
    total = area.sum()
    if total <= 0.0 or not len(tris):
        return verts[:1].repeat(n, 0)
    idx = rng.choice(len(tris), n, p=area / total)
    r1 = np.sqrt(rng.random(n))[:, None]
    r2 = rng.random(n)[:, None]
    return (1.0 - r1) * a[idx] + r1 * (1.0 - r2) * b[idx] + r1 * r2 * c[idx]


def _nearest(src, dst, chunk=2048):
    """For every src point, (distance, index) of the nearest dst point (brute force)."""
    best_d = np.empty(len(src))
    best_i = np.empty(len(src), dtype=np.int64)
    dst2 = (dst * dst).sum(1)
    for s in range(0, len(src), chunk):
        p = src[s:s + chunk]
        d2 = (p * p).sum(1)[:, None] - 2.0 * p @ dst.T + dst2[None, :]
        i = d2.argmin(1)
        best_i[s:s + chunk] = i
        best_d[s:s + chunk] = np.sqrt(np.maximum(d2[np.arange(len(p)), i], 0.0))
    return best_d, best_i


def _similarity(src, dst):
    """(s, R, t) minimising |s R src + t - dst| (Umeyama)."""
    mu_s, mu_d = src.mean(0), dst.mean(0)
    xs, xd = src - mu_s, dst - mu_d
    u, sv, vt = np.linalg.svd(xd.T @ xs / len(src))
    d = np.eye(3)
    if np.linalg.det(u) * np.linalg.det(vt) < 0.0:
        d[2, 2] = -1.0
    r = u @ d @ vt
    var = (xs * xs).sum() / len(src)
    s = float(np.trace(np.diag(sv) @ d) / var) if var > 0.0 else 1.0
    return s, r, mu_d - s * r @ mu_s


def _rotations24():
    out = []
    for perm in ((0, 1, 2), (1, 2, 0), (2, 0, 1), (1, 0, 2), (0, 2, 1), (2, 1, 0)):
        for signs in ((1, 1, 1), (1, 1, -1), (1, -1, 1), (1, -1, -1), (-1, 1, 1), (-1, 1, -1), (-1, -1, 1),
                      (-1, -1, -1)):
            m = np.zeros((3, 3))
            for i in range(3):
                m[i, perm[i]] = signs[i]
            if np.linalg.det(m) > 0.0:
                out.append(m)
    return out


def _icp(p, q, s, r, t, iterations, trim=0.9):
    """Similarity ICP of points q onto p, matching both ways; returns (s, R, t, score)."""
    score = float("inf")
    for _ in range(iterations):
        qt = s * q @ r.T + t
        d1, i1 = _nearest(qt, p)
        d2, i2 = _nearest(p, qt)
        k1 = d1 <= np.quantile(d1, trim)
        k2 = d2 <= np.quantile(d2, trim)
        src = np.concatenate([q[k1], q[i2[k2]]])
        dst = np.concatenate([p[i1[k1]], p[k2]])
        s, r, t = _similarity(src, dst)
        score = float(d1[k1].mean() + d2[k2].mean())
    return s, r, t, score


def align(target_verts, target_tris, verts, tris, seed=0, log=print):
    """The similarity transform (s, R, t) that lays mesh (verts, tris) over the target, its
    starting rotation searched over the 24 axis-aligned ones."""
    rng = np.random.default_rng(seed)
    p = sample_surface(target_verts, target_tris, 1500, rng)
    q = sample_surface(verts, tris, 1500, rng)
    cp, cq = p.mean(0), q.mean(0)
    scale = math.sqrt(((p - cp) ** 2).sum(1).mean() / max(((q - cq) ** 2).sum(1).mean(), 1e-12))
    tried = []
    for r0 in _rotations24():
        t0 = cp - scale * r0 @ cq
        tried.append(_icp(p, q, scale, r0, t0, 6))
    tried.sort(key=lambda x: x[3])
    s, r, t, coarse = tried[0]
    log("  alignment: best of 24 starts %.3f (next %.3f)" % (coarse, tried[1][3]))
    p = sample_surface(target_verts, target_tris, 6000, rng)
    q = sample_surface(verts, tris, 6000, rng)
    s, r, t, fine = _icp(p, q, s, r, t, 25, trim=0.95)
    return s, r, t, fine


def _components(n_verts, tris):
    parent = np.arange(n_verts)

    def find(a):
        root = a
        while parent[root] != root:
            root = parent[root]
        while parent[a] != root:
            parent[a], a = root, parent[a]
        return root

    for a, b, c in tris:
        ra, rb, rc = find(a), find(b), find(c)
        parent[rb] = ra
        parent[find(rc)] = ra
    return np.array([find(i) for i in range(n_verts)])


def clean(verts, tris, min_part=0.01):
    """Weld coincident vertices, drop degenerate faces and loose bits under min_part of the
    faces (generators leave floaters)."""
    _, _, diag = bounds(verts)
    key = np.round(verts / max(diag * 1e-6, 1e-9)).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    inverse = inverse.ravel()
    verts = verts[first]
    tris = inverse[tris]
    tris = tris[(tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])]
    comp = _components(len(verts), tris)
    face_comp = comp[tris[:, 0]]
    ids, counts = np.unique(face_comp, return_counts=True)
    keep = set(ids[counts >= max(1, int(min_part * len(tris)))].tolist())
    tris = tris[np.isin(face_comp, list(keep))]
    used = np.unique(tris)
    remap = np.full(len(verts), -1)
    remap[used] = np.arange(len(used))
    return verts[used], remap[tris]


def decimate(verts, tris, target_tris):
    """Collapse decimation in Blender down to about target_tris triangles."""
    if len(tris) <= target_tris:
        return verts, tris
    me = bpy.data.meshes.new("remaster_decimate")
    me.from_pydata(verts.tolist(), [], tris.tolist())
    obj = bpy.data.objects.new("remaster_decimate", me)
    bpy.context.scene.collection.objects.link(obj)
    mod = obj.modifiers.new("Decimate", "DECIMATE")
    mod.ratio = max(target_tris / len(tris), 1e-4)
    mod.use_collapse_triangulate = True
    dg = bpy.context.evaluated_depsgraph_get()
    out = bpy.data.meshes.new_from_object(obj.evaluated_get(dg), depsgraph=dg)
    bpy.data.objects.remove(obj, do_unlink=True)
    bpy.data.meshes.remove(me)
    out.calc_loop_triangles()
    v = np.empty(len(out.vertices) * 3)
    out.vertices.foreach_get("co", v)
    t = np.empty(len(out.loop_triangles) * 3, dtype=np.int64)
    out.loop_triangles.foreach_get("vertices", t)
    bpy.data.meshes.remove(out)
    return v.reshape(-1, 3), t.reshape(-1, 3)


def read_mesh(path):
    """(vertices, triangles) of an .obj, .ply, .glb/.gltf, .fbx or .stl file, in its own axes
    (Z up). Every mesh in the file is joined."""
    ext = os.path.splitext(path)[1].lower()
    before = set(bpy.data.objects)
    if ext == ".obj":
        bpy.ops.wm.obj_import(filepath=path, forward_axis="Y", up_axis="Z")
    elif ext == ".ply":
        bpy.ops.wm.ply_import(filepath=path, forward_axis="Y", up_axis="Z")
    elif ext == ".stl":
        bpy.ops.wm.stl_import(filepath=path, forward_axis="Y", up_axis="Z")
    elif ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=path)
    else:
        raise ValueError("cannot read %s" % path)
    new = [o for o in bpy.data.objects if o not in before]
    vs, ts, base = [], [], 0
    dg = bpy.context.evaluated_depsgraph_get()
    for o in new:
        if o.type != "MESH":
            continue
        me = o.evaluated_get(dg).to_mesh()
        me.calc_loop_triangles()
        v = np.empty(len(me.vertices) * 3)
        me.vertices.foreach_get("co", v)
        v = v.reshape(-1, 3)
        m = np.array(o.matrix_world)
        vs.append(v @ m[:3, :3].T + m[:3, 3])
        t = np.empty(len(me.loop_triangles) * 3, dtype=np.int64)
        me.loop_triangles.foreach_get("vertices", t)
        ts.append(t.reshape(-1, 3) + base)
        base += len(v)
        o.evaluated_get(dg).to_mesh_clear()
    for o in new:
        data = o.data if o.type == "MESH" else None
        bpy.data.objects.remove(o, do_unlink=True)
        if data is not None and data.users == 0:
            bpy.data.meshes.remove(data)
    if not vs:
        raise ValueError("%s holds no mesh" % path)
    return np.concatenate(vs), np.concatenate(ts)


# ----------------------------------------------------------------------------- the original

class Source:
    """The surfaces of an imported model, posed, with everything a new mesh takes from them."""

    def __init__(self, arm, keep=()):
        self.arm = arm
        self.surfaces = []
        verts, tris, owner = [], [], []
        base = 0
        for obj in model_meshes(arm):
            co, t, loops = posed_geometry(arm, obj)
            me = obj.data
            uv = np.zeros((len(me.loops), 2))
            if me.uv_layers.active is not None:
                flat = np.empty(len(me.loops) * 2)
                me.uv_layers.active.data.foreach_get("uv", flat)
                uv = flat.reshape(-1, 2)
            groups = [g.name for g in obj.vertex_groups]
            w = np.zeros((len(me.vertices), max(len(groups), 1)))
            for v in me.vertices:
                for g in v.groups:
                    w[v.index, g.group] = g.weight
            rel = np.array(_arm_space(arm, obj))[:3, :3]
            keys = []
            if me.shape_keys is not None and len(me.shape_keys.key_blocks) > 1:
                ref = me.shape_keys.reference_key
                rco = np.empty(len(me.vertices) * 3)
                ref.data.foreach_get("co", rco)
                for kb in me.shape_keys.key_blocks:
                    if kb == ref:
                        continue
                    kco = np.empty(len(me.vertices) * 3)
                    kb.data.foreach_get("co", kco)
                    keys.append((kb.name, (kco - rco).reshape(-1, 3) @ rel.T))
            s = {"obj": obj, "name": obj["mohaa_surface"], "skd": obj.get("mohaa_skd", ""), "verts": co,
                 "tris": t, "loops": loops, "uv": uv, "groups": groups, "weights": w, "keys": keys,
                 "keep": any(_match(obj["mohaa_surface"], k) for k in keep), "base": base}
            s["islands"] = _uv_islands(t, loops, uv)
            self.surfaces.append(s)
            verts.append(co)
            tris.append(t + base)
            owner.append(np.full(len(t), len(self.surfaces) - 1))
            base += len(co)
        self.verts = np.concatenate(verts)
        self.tris = np.concatenate(tris)
        self.owner = np.concatenate(owner)
        self.bvh = BVHTree.FromPolygons(self.verts.tolist(), self.tris.tolist())
        self._island_bvh = {}
        self._surface_bvh = {}

    def surface_bvh(self, si):
        if si not in self._surface_bvh:
            s = self.surfaces[si]
            self._surface_bvh[si] = BVHTree.FromPolygons(s["verts"].tolist(), s["tris"].tolist())
        return self._surface_bvh[si]

    def island_bvh(self, si, island):
        key = (si, island)
        if key not in self._island_bvh:
            s = self.surfaces[si]
            idx = np.nonzero(s["islands"] == island)[0]
            self._island_bvh[key] = (BVHTree.FromPolygons(s["verts"].tolist(), s["tris"][idx].tolist()), idx)
        return self._island_bvh[key]


def _match(name, pattern):
    import fnmatch
    return fnmatch.fnmatch(name.lower(), pattern.lower())


def _uv_islands(tris, loops, uv):
    """An island number per triangle: triangles meet on an island when they share an edge
    whose two corners have the same UVs on both sides."""
    parent = list(range(len(tris)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    edges = {}
    for ti in range(len(tris)):
        for k in range(3):
            a, b = int(tris[ti][k]), int(tris[ti][(k + 1) % 3])
            ua = tuple(np.round(uv[loops[ti][k]], 5))
            ub = tuple(np.round(uv[loops[ti][(k + 1) % 3]], 5))
            key = (a, b) if a < b else (b, a)
            corner = (ua, ub) if a < b else (ub, ua)
            for tj, other in edges.get(key, ()):
                if other == corner:
                    parent[find(ti)] = find(tj)
            edges.setdefault(key, []).append((ti, corner))
    return np.array([find(i) for i in range(len(tris))])


def _barycentric(p, a, b, c):
    v0, v1, v2 = b - a, c - a, p - a
    d00, d01, d11 = v0.dot(v0), v0.dot(v1), v1.dot(v1)
    d20, d21 = v2.dot(v0), v2.dot(v1)
    den = d00 * d11 - d01 * d01
    if abs(den) < 1e-18:
        return (1.0, 0.0, 0.0)
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    u = 1.0 - v - w
    # the nearest point lies on the triangle; clamp what rounding leaves outside
    bc = np.clip(np.array((u, v, w)), 0.0, 1.0)
    return tuple(bc / bc.sum())


def _on(s, tri_index, p):
    t = s["tris"][tri_index]
    return _barycentric(np.asarray(p), s["verts"][t[0]], s["verts"][t[1]], s["verts"][t[2]])


# ----------------------------------------------------------------------------- transfer

def transfer(source, verts, tris):
    """Cut a posed mesh (armature space) into the source's surfaces.
    Returns {surface index: {"verts", "tris", "uv" (per corner), "weights", "keys"}}."""
    owner = np.empty(len(tris), dtype=np.int64)
    island = np.empty(len(tris), dtype=np.int64)
    centroids = verts[tris].mean(1)
    tri_base = np.cumsum([0] + [len(s["tris"]) for s in source.surfaces])
    for fi, c in enumerate(centroids):
        _, _, ti, _ = source.bvh.find_nearest(Vector(c))
        ti = ti or 0
        si = int(source.owner[ti])
        owner[fi] = si
        island[fi] = source.surfaces[si]["islands"][ti - tri_base[si]]
    out = {}
    for si in np.unique(owner):
        s = source.surfaces[si]
        if s["keep"]:
            continue
        faces = tris[owner == si]
        isl = island[owner == si]
        used = np.unique(faces)
        remap = {int(v): i for i, v in enumerate(used)}
        sv = verts[used]
        st = np.vectorize(remap.get)(faces) if len(faces) else faces
        # vertex attributes from anywhere on the surface: weights and morph deltas
        bvh = source.surface_bvh(si)
        w = np.zeros((len(sv), s["weights"].shape[1]))
        keys = [np.zeros((len(sv), 3)) for _ in s["keys"]]
        for i, p in enumerate(sv):
            loc, _, ti, _ = bvh.find_nearest(Vector(p))
            bc = _on(s, ti, loc)
            t = s["tris"][ti]
            w[i] = bc[0] * s["weights"][t[0]] + bc[1] * s["weights"][t[1]] + bc[2] * s["weights"][t[2]]
            for k, (_, d) in enumerate(s["keys"]):
                keys[k][i] = bc[0] * d[t[0]] + bc[1] * d[t[1]] + bc[2] * d[t[2]]
        # UVs per corner, from the face's own island so no face spans a seam
        uv = np.zeros((len(st), 3, 2))
        for fi, (face, il) in enumerate(zip(faces, isl)):
            ibvh, idx = source.island_bvh(si, il)
            for k in range(3):
                loc, _, j, _ = ibvh.find_nearest(Vector(verts[face[k]]))
                ti = idx[j]
                bc = _on(s, ti, loc)
                lp = s["loops"][ti]
                uv[fi, k] = bc[0] * s["uv"][lp[0]] + bc[1] * s["uv"][lp[1]] + bc[2] * s["uv"][lp[2]]
        out[int(si)] = {"verts": sv, "tris": st, "uv": uv, "weights": w, "keys": keys}
    return out


def estimate_pieces(n_tris_by_surface):
    """Surfaces the exporter will write for surfaces of these triangle counts."""
    total = 0
    for n in n_tris_by_surface:
        verts = n * 0.6  # closed meshes have about half as many vertices as faces, plus seams
        total += max(1, math.ceil(verts / PIECE_VERTS), math.ceil(n / skd.MAX_TRIS))
    return total


def deform_matrices(arm, names):
    """{group name: armature-space matrix taking rest positions to the current pose}."""
    out = {}
    for n in names:
        pb = arm.pose.bones.get(n)
        if pb is not None:
            out[n] = pb.matrix @ pb.bone.matrix_local.inverted()
    return out


def unpose(arm, verts, weights, groups):
    """Positions in the current pose back to the rest pose, by inverting linear skinning."""
    mats = deform_matrices(arm, groups)
    m = np.zeros((len(groups), 4, 4))
    for gi, g in enumerate(groups):
        m[gi] = np.array(mats.get(g, Matrix.Identity(4)))
    total = weights.sum(1, keepdims=True)
    wn = np.where(total > 0, weights / np.maximum(total, 1e-12), 0.0)
    blend = np.einsum("vg,gij->vij", wn, m)
    blend[total[:, 0] <= 0] = np.eye(4)
    h = np.concatenate([verts, np.ones((len(verts), 1))], 1)
    rest = np.linalg.solve(blend, h[:, :, None])[:, :, 0]
    return rest[:, :3]


def build_object(arm, s, data, rest_verts, name):
    """A mesh object for one new surface, rigged and shaded like the one it replaces."""
    orig = s["obj"]
    me = bpy.data.meshes.new(name)
    me.from_pydata(rest_verts.tolist(), [], data["tris"].tolist())
    uvl = me.uv_layers.new(name="UVMap")
    flat = np.zeros(len(me.loops) * 2)
    for poly in me.polygons:
        fi = poly.index
        for k, li in enumerate(poly.loop_indices):
            flat[li * 2:li * 2 + 2] = data["uv"][fi, k]
    uvl.data.foreach_set("uv", flat)
    for mat in orig.data.materials:
        me.materials.append(mat)
    bm = bmesh.new()
    bm.from_mesh(me)
    for f in bm.faces:
        f.smooth = True
    for e in bm.edges:
        if len(e.link_faces) == 2 and e.calc_face_angle(0.0) > math.radians(60.0):
            e.smooth = False
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new(name, me)
    for coll in orig.users_collection:
        coll.objects.link(obj)
    obj.parent = arm
    obj.matrix_world = arm.matrix_world.copy()
    for key in ("mohaa_surface", "mohaa_skd", "mohaa_flags"):
        if key in orig:
            obj[key] = orig[key]
    groups = [obj.vertex_groups.new(name=g) for g in s["groups"]]
    w = data["weights"]
    for gi, vg in enumerate(groups):
        col = w[:, gi]
        for vi in np.nonzero(col > 1e-4)[0]:
            vg.add([int(vi)], float(col[vi]), "REPLACE")
    if s["keys"]:
        obj.shape_key_add(name="Basis", from_mix=False)
        for (kname, _), delta in zip(s["keys"], data["keys"]):
            kb = obj.shape_key_add(name=kname, from_mix=False)
            co = (rest_verts + delta).ravel()
            kb.data.foreach_set("co", co)
            kb.value = 0.0
    mod = obj.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    obj["mohaa_remaster"] = "detail"
    return obj


HARD_ANGLE = 60.0     # faces meeting at more than this keep their edge creased
FOLD_ANGLE = 120.0    # ... and at more than this (a thin panel's rim) shaded sharp too
NORMAL_BREAK = 0.98   # ... and so do faces whose normals at a shared corner differ (cosine)
HOLD_RATIO = 2.0      # an edge with a face reaching further than this many times its length is held


def _weld(bm, dist):
    """Join the vertices the game format splits at every UV seam and normal break, so the
    surface is one connected mesh again (UVs stay per corner, so seams stay in the UVs)."""
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=dist)


def _span(face, edge):
    """How far a face reaches from one of its edges."""
    a, b = (v.co for v in edge.verts)
    d = b - a
    ln2 = max(d.length_squared, 1e-12)
    return max(((v.co - a) - d * ((v.co - a).dot(d) / ln2)).length for v in face.verts)


def _mark_hard_edges(bm, normal_layer):
    """Creased edges: open borders, steep folds, and the edges the original's normals break
    across; only the borders, breaks and folds back on themselves are shaded sharp (a steep
    fold the artist shaded smooth stays smooth). Everything else is meant to look curved and
    gets rounded, except across long faces: a face reaching far from an edge would bow out along its length (a
    straight-sided cylinder would turn into a barrel), so such an edge is creased too, but
    stays smooth shaded, the way a modeller adds a holding edge."""
    crease = bm.edges.layers.float.get("crease_edge") or bm.edges.layers.float.new("crease_edge")
    hard_cos = math.cos(math.radians(HARD_ANGLE))
    fold_cos = math.cos(math.radians(FOLD_ANGLE))
    for e in bm.edges:
        sharp = len(e.link_faces) != 2  # shaded sharp: open borders and the original's breaks
        steep = False
        if not sharp:
            f1, f2 = e.link_faces
            steep = f1.normal.dot(f2.normal) < hard_cos
            sharp = f1.normal.dot(f2.normal) < fold_cos
            if normal_layer is not None and not sharp:
                for v in e.verts:
                    n1 = next(l[normal_layer] for l in f1.loops if l.vert is v)
                    n2 = next(l[normal_layer] for l in f2.loops if l.vert is v)
                    if Vector(n1).dot(Vector(n2)) < NORMAL_BREAK:
                        sharp = True
                        break
            elif normal_layer is None:
                sharp = steep
        hold = not (sharp or steep) and max(_span(f, e) for f in e.link_faces) > HOLD_RATIO * e.calc_length()
        e[crease] = 1.0 if sharp or steep or hold else 0.0
        e.smooth = not sharp
    for f in bm.faces:
        f.smooth = True


def _subdivided(me, groups, levels, kind):
    """me evaluated through a Subdivision modifier of the given kind (SIMPLE or
    CATMULL_CLARK); the same topology and vertex order either way."""
    tmp = bpy.data.objects.new(me.name + "_tmp", me)
    bpy.context.scene.collection.objects.link(tmp)
    for g in groups:
        tmp.vertex_groups.new(name=g)
    mod = tmp.modifiers.new("Subdivision", "SUBSURF")
    mod.subdivision_type = kind
    mod.levels = mod.render_levels = levels
    mod.uv_smooth = "NONE"  # linear: every original face keeps its texture mapping
    mod.boundary_smooth = "PRESERVE_CORNERS"
    mod.use_creases = True
    dg = bpy.context.evaluated_depsgraph_get()
    out = bpy.data.meshes.new_from_object(tmp.evaluated_get(dg), preserve_all_data_layers=True, depsgraph=dg)
    bpy.data.objects.remove(tmp, do_unlink=True)
    return out


def _round(flat, smooth):
    """Move the linearly subdivided mesh `flat` to the Catmull-Clark one `smooth`, but only
    along its normals: sliding along the surface would drag the texture with it."""
    n = len(flat.vertices)
    lin = np.empty(n * 3)
    flat.vertices.foreach_get("co", lin)
    lin = lin.reshape(-1, 3)
    cc = np.empty(n * 3)
    smooth.vertices.foreach_get("co", cc)
    cc = cc.reshape(-1, 3)
    nrm = np.empty(n * 3)
    flat.vertex_normals.foreach_get("vector", nrm)
    nrm = nrm.reshape(-1, 3)
    lift = ((cc - lin) * nrm).sum(1)
    flat.vertices.foreach_set("co", (lin + nrm * lift[:, None]).ravel())
    flat.update()


def smoothed_copy(arm, s, name, levels=1):
    """The original surface rounded by subdivision (Catmull-Clark): welded into one mesh,
    triangles paired into quads, sharp edges kept (see _mark_hard_edges), and the texture
    kept in place (see _round). The rounded surface runs a little inside the original's
    corners and outside its flat faces. None for a surface with morph targets (its modifiers
    would not apply)."""
    orig = s["obj"]
    if orig.data.shape_keys is not None:
        return None
    me = orig.data.copy()
    me.name = name
    # the original's normals, per corner, to find which edges it shades smooth
    cn = np.empty(len(me.loops) * 3)
    me.corner_normals.foreach_get("vector", cn)
    attr = me.attributes.new("mohaa_normal", "FLOAT_VECTOR", "CORNER")
    attr.data.foreach_set("vector", cn)
    if "custom_normal" in me.attributes:  # it would carry the flat shading over: sharp edges decide now
        me.attributes.remove(me.attributes["custom_normal"])
    bm = bmesh.new()
    bm.from_mesh(me)
    lo, hi = np.array(bounds_of_bm(bm))
    _weld(bm, 1e-4 * max(float(np.linalg.norm(hi - lo)), 1e-6))
    bm.normal_update()
    normal_layer = bm.loops.layers.float_vector.get("mohaa_normal")
    _mark_hard_edges(bm, normal_layer)
    bmesh.ops.join_triangles(bm, faces=bm.faces[:], cmp_sharp=True, cmp_uvs=True, cmp_materials=True,
                             angle_face_threshold=math.radians(40.0), angle_shape_threshold=math.radians(40.0))
    bm.to_mesh(me)
    bm.free()
    me.attributes.remove(me.attributes["mohaa_normal"])
    groups = [g.name for g in orig.vertex_groups]
    new_me = _subdivided(me, groups, levels, "SIMPLE")
    smooth = _subdivided(me, groups, levels, "CATMULL_CLARK")
    bpy.data.meshes.remove(me)
    _round(new_me, smooth)
    bpy.data.meshes.remove(smooth)
    for a in [a.name for a in new_me.attributes if a.name.startswith("mohaa_collapse")]:
        new_me.attributes.remove(new_me.attributes[a])
    new_me.name = name
    obj = bpy.data.objects.new(name, new_me)
    for coll in orig.users_collection:
        coll.objects.link(obj)
    obj.parent = arm
    obj.matrix_world = orig.matrix_world.copy()
    for g in orig.vertex_groups:
        obj.vertex_groups.new(name=g.name)
    for key in ("mohaa_surface", "mohaa_skd", "mohaa_flags"):
        if key in orig:
            obj[key] = orig[key]
    mod = obj.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    obj["mohaa_remaster"] = "smoothed"
    return obj


def bounds_of_bm(bm):
    co = np.array([v.co[:] for v in bm.verts]) if bm.verts else np.zeros((1, 3))
    return co.min(0), co.max(0)


# ----------------------------------------------------------------------------- checks

def surface_distance(points, bvh):
    d = np.empty(len(points))
    for i, p in enumerate(points):
        hit = bvh.find_nearest(Vector(p))
        d[i] = hit[3] if hit[0] is not None else 1e9
    return d


def fit_metrics(source, si, verts, tris, rng):
    """95th percentile distances, original -> new (parts the new mesh lacks) and new ->
    original (parts it adds), in model units."""
    s = source.surfaces[si]
    new_bvh = BVHTree.FromPolygons(verts.tolist(), tris.tolist())
    a = surface_distance(sample_surface(s["verts"], s["tris"], 1500, rng), new_bvh)
    b = surface_distance(sample_surface(verts, tris, 1500, rng), source.surface_bvh(si))
    return float(np.quantile(a, 0.95)), float(np.quantile(b, 0.95))


def edge_stretch(obj, arm, rest_lengths=None):
    """|log(length / rest length)| of every edge, as the scene poses the object."""
    me = obj.data
    e = np.empty(len(me.edges) * 2, dtype=np.int64)
    me.edges.foreach_get("vertices", e)
    e = e.reshape(-1, 2)
    if rest_lengths is None:
        co = np.empty(len(me.vertices) * 3)
        me.vertices.foreach_get("co", co)
        co = co.reshape(-1, 3)
        return np.linalg.norm(co[e[:, 0]] - co[e[:, 1]], axis=1)
    co, _, _ = posed_geometry(arm, obj)
    ln = np.linalg.norm(co[e[:, 0]] - co[e[:, 1]], axis=1)
    ok = rest_lengths > 1e-6
    return np.abs(np.log(np.maximum(ln[ok], 1e-9) / rest_lengths[ok]))


def deformation_check(arm, pairs, actions, frames_per_action=5):
    """For each (original object, new object): the 99th percentile edge stretch of each over
    the actions' frames. Returns {new object name: (original p99, new p99)}."""
    scene = bpy.context.scene
    clear_pose(arm)
    rest = {o.name: edge_stretch(o, arm) for pair in pairs for o in pair}
    worst = {n.name: [0.0, 0.0] for _, n in pairs}
    for action in actions:
        animutil.assign(arm, action)
        start, end = action.frame_range
        for k in range(frames_per_action):
            f = start + (end - start) * k / max(frames_per_action - 1, 1)
            scene.frame_set(int(f), subframe=f - int(f))
            for o, n in pairs:
                a = edge_stretch(o, arm, rest[o.name])
                b = edge_stretch(n, arm, rest[n.name])
                w = worst[n.name]
                w[0] = max(w[0], float(np.quantile(a, 0.99)) if len(a) else 0.0)
                w[1] = max(w[1], float(np.quantile(b, 0.99)) if len(b) else 0.0)
    if arm.animation_data is not None:
        arm.animation_data.action = None
    clear_pose(arm)
    return {k: tuple(v) for k, v in worst.items()}


# ----------------------------------------------------------------------------- the whole job

class Options:
    def __init__(self, **kw):
        self.factor = 4.0           # target triangles, as a multiple of the original's
        self.max_tris = 30000       # for the whole model (skinning runs on the CPU)
        self.keep = []              # surface name patterns left as they are
        self.fit_tolerance = 0.03   # of the model's size, both ways
        self.stretch_margin = 1.5   # new p99 edge stretch may be this many times the original's
        self.stretch_slack = 0.05   # ... plus this
        self.smooth = True          # fall back to smoothing
        self.mode = "detail"        # detail: candidate meshes; subdivide: round every surface
        self.levels = 1             # subdivision levels (each about quadruples the triangles)
        self.seed = 0
        for k, v in kw.items():
            if not hasattr(self, k):
                raise TypeError("unknown option %s" % k)
            setattr(self, k, v)


def build(arm, candidates, options, log=print):
    """Replace the model's surfaces with the best candidate mesh where it passes the checks,
    and smoothed originals where it does not. Candidates are file paths; the scene must hold
    the model as prepare() left it (lifted pose, no action). Returns a report dict."""
    if options.mode == "subdivide":
        return subdivide(arm, options, log=log)
    rng = np.random.default_rng(options.seed)
    report = {"candidates": [], "surfaces": {}, "warnings": []}
    actions = [a for a in bpy.data.actions if a.get("mohaa_armature") == arm.name]
    source = Source(arm, options.keep)
    _, _, diag = bounds(source.verts)
    tol = options.fit_tolerance * diag
    orig_tris = sum(len(s["tris"]) for s in source.surfaces if not s["keep"])
    target = min(int(orig_tris * options.factor), options.max_tris)

    best = None
    for path in candidates:
        try:
            v, t = read_mesh(path)
        except (ValueError, RuntimeError) as e:
            report["candidates"].append({"path": path, "error": str(e)})
            continue
        v, t = clean(v, t)
        if len(t) > 60000:  # aligning and cutting need not see every generated face
            v, t = decimate(v, t, 60000)
        s, r, tr, score = align(source.verts, source.tris, v, t, seed=options.seed, log=log)
        v = s * v @ r.T + tr
        report["candidates"].append({"path": path, "faces": int(len(t)), "fit": score / diag})
        log("  %s: %d faces, alignment error %.4f of the model's size" % (os.path.basename(path), len(t), score / diag))
        if best is None or score < best[2]:
            best = (v, t, score, path)

    new_objects = {}
    cut = {}
    if best is not None:
        v, t, _, path = best
        report["chosen"] = path
        # the budget: smaller until the exporter's split surfaces fit the engine's 32
        kept_counts = [len(s["tris"]) for s in source.surfaces if s["keep"]]
        for attempt in range(8):
            dv, dt = decimate(v, t, target)
            cut = transfer(source, dv, dt)
            counts = {si: len(c["tris"]) for si, c in cut.items()}
            others = [len(s["tris"]) for si, s in enumerate(source.surfaces) if si not in counts and not s["keep"]]
            pieces = estimate_pieces(list(counts.values()) + others + kept_counts)
            if pieces <= MAX_SURFACES:
                break
            target = int(target * 0.75)
        else:
            report["warnings"].append("could not fit %d surfaces in the engine's %d" % (pieces, MAX_SURFACES))
        report["target_tris"] = int(target)
        log("  budget %d triangles, about %d surfaces once split" % (target, pieces))
        clear_pose(arm)
        posed = {}
        spread = json.loads(arm.get("mohaa_remaster_pose", "{}"))
        _apply_pose(arm, spread)
        for si, c in cut.items():
            s = source.surfaces[si]
            near, extra = fit_metrics(source, si, c["verts"], c["tris"], rng)
            rest = unpose(arm, c["verts"], c["weights"], s["groups"])
            name = "%s.remaster" % s["obj"].name
            posed[si] = (near, extra)
            new_objects[si] = build_object(arm, s, c, rest, name)
        clear_pose(arm)
    else:
        posed = {}

    # checks, surface by surface; failures fall back
    pairs = [(source.surfaces[si]["obj"], o) for si, o in new_objects.items()]
    stretch = deformation_check(arm, pairs, actions) if pairs and actions else {}
    final = {}
    for si, s in enumerate(source.surfaces):
        entry = {"skd": s["skd"], "original_tris": int(len(s["tris"]))}
        obj = new_objects.get(si)
        reason = None
        if s["keep"]:
            reason = "kept (--keep)"
        elif obj is None:
            reason = "no candidate face landed on it" if best is not None else "no candidate"
        else:
            near, extra = posed[si]
            entry.update({"missing_p95": near / diag, "extra_p95": extra / diag})
            so, sn = stretch.get(obj.name, (0.0, 0.0))
            entry.update({"stretch_original": so, "stretch_new": sn, "tris": int(len(obj.data.polygons))})
            if near > tol:
                reason = "misses parts of the original (%.3f of the model's size)" % (near / diag)
            elif extra > tol:
                reason = "adds parts the original lacks (%.3f of the model's size)" % (extra / diag)
            elif sn > so * options.stretch_margin + options.stretch_slack:
                reason = "stretches in animation (%.3f, the original %.3f)" % (sn, so)
        if reason is None:
            entry["result"] = "detail"
            final[si] = obj
        else:
            if obj is not None:
                bpy.data.objects.remove(obj, do_unlink=True)
            entry["reason"] = reason
            sm = smoothed_copy(arm, s, "%s.smooth" % s["obj"].name) if options.smooth and not s["keep"] else None
            if sm is not None:
                if actions:
                    so, sn = deformation_check(arm, [(s["obj"], sm)], actions)[sm.name]
                    if sn > so * options.stretch_margin + options.stretch_slack:
                        bpy.data.objects.remove(sm, do_unlink=True)
                        sm = None
                        entry["reason"] += "; smoothing stretches too"
            if sm is not None:
                entry["result"] = "smoothed"
                final[si] = sm
            else:
                entry["result"] = "original"
        report["surfaces"]["%s (%s)" % (s["name"], os.path.basename(s["skd"]))] = entry
        log("  %-28s %s%s" % (s["name"], entry["result"], (": " + entry["reason"]) if entry.get("reason") else ""))

    _hide_replaced(source, final, report)
    return report


def _hide_replaced(source, final, report):
    """The replaced originals leave the export (hidden in the .blend, for comparing)."""
    report["replaced"] = []
    for si in final:
        s = source.surfaces[si]
        s["obj"].hide_set(True)
        s["obj"].hide_render = True
        s["obj"]["mohaa_remaster"] = "replaced"
        report["replaced"].append(s["skd"])


def subdivide(arm, options, log=print):
    """Round every surface by subdivision (smoothed_copy), for props: their shapes are
    right, only coarse. A surface that strays from the original's shape, stretches in
    animation or would break the engine's limits stays as it was. Returns a report dict."""
    rng = np.random.default_rng(options.seed)
    report = {"mode": "subdivide", "levels": options.levels, "surfaces": {}, "warnings": []}
    actions = [a for a in bpy.data.actions if a.get("mohaa_armature") == arm.name]
    clear_pose(arm)
    source = Source(arm, options.keep)
    _, _, diag = bounds(source.verts)
    tol = options.fit_tolerance * diag
    final = {}
    tri_counts = {}
    for si, s in enumerate(source.surfaces):
        entry = {"skd": s["skd"], "original_tris": int(len(s["tris"]))}
        sm = None
        reason = "kept (--keep)" if s["keep"] else None
        if reason is None:
            sm = smoothed_copy(arm, s, "%s.smooth" % s["obj"].name, levels=options.levels)
            if sm is None:
                reason = "has morph targets"
        if sm is not None:
            co, tris, _ = posed_geometry(arm, sm)
            near, extra = fit_metrics(source, si, co, tris, rng)
            entry.update({"tris": int(len(tris)), "missing_p95": near / diag, "extra_p95": extra / diag})
            if near > tol or extra > tol:
                reason = "strays from the original (%.3f of the model's size)" % (max(near, extra) / diag)
            elif actions:
                so, sn = deformation_check(arm, [(s["obj"], sm)], actions)[sm.name]
                entry.update({"stretch_original": so, "stretch_new": sn})
                if sn > so * options.stretch_margin + options.stretch_slack:
                    reason = "stretches in animation (%.3f, the original %.3f)" % (sn, so)
        if reason is None:
            entry["result"] = "smoothed"
            final[si] = sm
            tri_counts[si] = entry["tris"]
        else:
            if sm is not None:
                bpy.data.objects.remove(sm, do_unlink=True)
            entry["result"] = "original"
            entry["reason"] = reason
        report["surfaces"]["%s (%s)" % (s["name"], os.path.basename(s["skd"]))] = entry
        log("  %-28s %s%s" % (s["name"], entry["result"], (": " + entry["reason"]) if entry.get("reason") else ""))

    # the engine's limits: drop the biggest rounded surfaces until the model fits
    def totals():
        counts = [tri_counts.get(si, len(s["tris"])) for si, s in enumerate(source.surfaces)]
        return sum(counts), estimate_pieces(counts)
    total, pieces = totals()
    while final and (pieces > MAX_SURFACES or total > options.max_tris):
        si = max(final, key=lambda k: tri_counts[k])
        bpy.data.objects.remove(final.pop(si), do_unlink=True)
        del tri_counts[si]
        s = source.surfaces[si]
        e = report["surfaces"]["%s (%s)" % (s["name"], os.path.basename(s["skd"]))]
        e["result"], e["reason"] = "original", "over the engine's or --max-tris budget"
        report["warnings"].append("%s left as it was: the model would be over budget" % s["name"])
        total, pieces = totals()
    report["tris"], report["surfaces_once_split"] = int(total), int(pieces)
    _hide_replaced(source, final, report)
    return report


def _apply_pose(arm, spread):
    """The lifted pose prepare() recorded: {bone: 4x4 armature-space matrix}."""
    for name, rows in spread.items():
        pb = arm.pose.bones.get(name)
        if pb is not None:
            pb.matrix = Matrix(rows)
            bpy.context.view_layer.update()


def record_pose(arm, bones):
    arm["mohaa_remaster_pose"] = json.dumps({n: [list(r) for r in arm.pose.bones[n].matrix] for n in bones})


def export_surfaces(context, arm, out_root, work_dir, log=print):
    """Write the .skd files holding a remastered surface to out_root at their game paths
    (the game's .tik and animations stay as they are). Returns the paths written."""
    from . import exporter
    visible = [o for o in model_meshes(arm) if not o.hide_get() and o.get("mohaa_remaster") != "replaced"]
    tmp_tik = os.path.join(work_dir, "export", "models", "remaster.tik")
    ex = exporter.Exporter(context, tmp_tik, arm_obj=arm, meshes=visible, anim_mode="NONE", write_textures=False,
                           split_same_name=True)
    ex.run()
    changed = {o.get("mohaa_skd", "") for o in visible if o.get("mohaa_remaster") in ("detail", "smoothed")}
    written = []
    for game_path in sorted(changed):
        if not game_path:
            continue
        src = os.path.join(ex.data_dir, os.path.basename(game_path))
        dst = os.path.join(out_root, *game_path.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(src, "rb") as f:
            data = f.read()
        with open(dst, "wb") as f:
            f.write(data)
        written.append(dst)
    count = 0
    for game_path in {o.get("mohaa_skd", "") for o in visible}:
        p = os.path.join(ex.data_dir, os.path.basename(game_path))
        if os.path.exists(p):
            with open(p, "rb") as f:
                count += len(skd.read(f.read()).surfaces)
    if count > MAX_SURFACES:
        log("warning: %d surfaces, over the engine's %d" % (count, MAX_SURFACES))
    return written, ex.warnings, count


# ----------------------------------------------------------------------------- prepare

CHECK_ANIM_HINTS = ("idle", "run", "walk", "crouch", "fire", "reload", "death", "jump", "throw")


def pick_check_anims(aliases, count=6):
    """A few animations covering the ways a model moves: the first matching each hint, then
    any others spread over the list."""
    chosen = []
    for hint in CHECK_ANIM_HINTS:
        a = next((a for a in aliases if hint in a.lower() and a not in chosen), None)
        if a is not None:
            chosen.append(a)
        if len(chosen) >= count:
            return chosen
    rest = [a for a in aliases if a not in chosen]
    step = max(1, len(rest) // max(1, count - len(chosen)))
    chosen += rest[::step][:count - len(chosen)]
    return chosen


def prepare(context, model_path, folders, work_dir, spread=30.0, check_anims="auto", size=1024,
            yaws=(0.0, 90.0, 180.0, 270.0), log=print):
    """Import a model for remastering, lift its arms, render it for a generator and save the
    scene as work_dir/source.blend. Returns a report dict."""
    from . import importer
    from .mohaa import tiki, vfs
    report = {"model": model_path}
    pattern = check_anims
    if check_anims == "auto" and model_path.lower().endswith(".tik"):
        fs = vfs.GameFS(folders)
        root = vfs.guess_root(os.path.abspath(model_path))
        if root:
            fs.add_loose_root(root)
            gp = vfs.game_path(os.path.abspath(model_path), root)
        else:
            gp = os.path.basename(model_path)

        def read_text(p):
            if p.lower() == gp.lower() and os.path.exists(model_path):
                with open(model_path, encoding="latin-1") as f:
                    return f.read()
            return fs.read_text(p)

        tk = tiki.parse(read_text, gp)
        pattern = ",".join(pick_check_anims([a.alias for a in tk.anims]))
    elif check_anims == "auto":
        pattern = ""
    arm, warnings = importer.import_model(context, model_path, folders, anim_mode="FILTER" if pattern else "REFERENCE",
                                          anim_filter=pattern, load_textures=True, pack_images=True)
    report["warnings"] = warnings
    report["check_anims"] = [a.name for a in bpy.data.actions if a.get("mohaa_armature") == arm.name]
    moved = spread_pose(arm, spread)
    record_pose(arm, moved)
    report["lifted"] = moved
    log("  lifted %s" % (", ".join(moved) or "nothing (no upper arms)"))
    report["views"] = render_views(model_meshes(arm), os.path.join(work_dir, "views"), size=size, yaws=yaws)
    arm["mohaa_remaster_source"] = model_path
    blend = os.path.join(work_dir, "source.blend")
    bpy.ops.wm.save_as_mainfile(filepath=blend, compress=True)
    report["blend"] = blend
    return report


def build_from_blend(context, blend, candidates, out_root, work_dir, options, log=print):
    """build() and export_surfaces() on a scene prepare() saved, plus before/after renders.
    Saves the result as work_dir/remastered.blend. Returns the report."""
    bpy.ops.wm.open_mainfile(filepath=blend)
    arm = next(o for o in bpy.context.scene.objects if o.type == "ARMATURE" and "mohaa_remaster_pose" in o)
    clear_pose(arm)
    before = render_views(model_meshes(arm), os.path.join(work_dir, "review"), size=512,
                          yaws=(0.0, 45.0, 90.0, 180.0), elevation=5.0, prefix="before")
    _apply_pose(arm, json.loads(arm["mohaa_remaster_pose"]))
    report = build(arm, candidates, options, log=log)
    clear_pose(arm)
    shown = [o for o in model_meshes(arm) if o.get("mohaa_remaster") != "replaced"]
    after = render_views(shown, os.path.join(work_dir, "review"), size=512,
                         yaws=(0.0, 45.0, 90.0, 180.0), elevation=5.0, prefix="after")
    report["sheet"] = contact_sheet([before, after], os.path.join(work_dir, "review", "before_after.png"))
    if any(e["result"] != "original" for e in report["surfaces"].values()):
        written, warnings, count = export_surfaces(context, arm, out_root, work_dir, log=log)
        report["written"] = written
        report["warnings"] += warnings
        report["surface_count"] = count
    else:
        report["written"] = []
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(work_dir, "remastered.blend"), compress=True)
    with open(os.path.join(work_dir, "report.json"), "w") as f:
        json.dump(report, f, indent=1)
    return report
