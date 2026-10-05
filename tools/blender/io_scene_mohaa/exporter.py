"""Blender armatures and meshes out to MOHAA: .skd meshes, .skc animations and a .tik.

Works on a model imported by this add-on (round trips keep surface names, shaders, bone
types, the TIKI's init and animation blocks, LOD data and hit boxes) and on anything made
in Blender: a mesh without an armature gets a one-bone skeleton, and materials with an
image get the image written as a .tga next to the model.
"""

import json
import math
import os
import re
import zipfile

import bpy
from mathutils import Matrix, Vector

from . import anim as animutil
from . import materials
from .importer import UNIT_METERS, bone_axis_matrix
from .mohaa import pose, skc, skd, tiki, vfs


class ExportError(Exception):
    pass


def _safe(name, limit=63):
    name = re.sub(r"[^A-Za-z0-9_\-. ]", "_", name).strip() or "unnamed"
    return name[:limit]


def _file_safe(name):
    return re.sub(r"[^A-Za-z0-9_\-.]", "_", name)


def _rows(m):
    return [list(r) for r in m]


def find_objects(context, arm_obj=None):
    """(armature or None, [mesh objects]) to export, from the selection when not given."""
    if arm_obj is None:
        sel = list(context.selected_objects) or ([context.active_object] if context.active_object else [])
        arms = [o for o in sel if o.type == "ARMATURE"]
        if not arms:
            for o in sel:
                if o.type == "MESH":
                    a = o.find_armature()
                    if a is not None:
                        arms.append(a)
                        break
        arm_obj = arms[0] if arms else None
        if arm_obj is None:
            meshes = [o for o in sel if o.type == "MESH"]
            if not meshes:
                raise ExportError("select an armature or the meshes to export")
            return None, meshes
    meshes = [o for o in bpy.data.objects if o.type == "MESH" and (o.parent == arm_obj or o.find_armature() == arm_obj)]
    meshes = [o for o in meshes if o.name in bpy.context.view_layer.objects]
    if not meshes:
        raise ExportError("armature %s has no meshes" % arm_obj.name)
    return arm_obj, meshes


class _Bone:
    def __init__(self, name, parent, btype, rest):
        self.name = name
        self.parent = parent
        self.type = btype
        self.rest = rest          # engine model-space matrix (column convention, raw units)
        self.base_data = b""
        self.channels = []
        self.refs = []


class Exporter:
    def __init__(self, context, filepath, arm_obj=None, meshes=None, skd_version=5, anim_mode="ALL",
                 write_textures=True, apply_modifiers=True, pk3_path=None, scale=None):
        self.context = context
        self.filepath = os.path.abspath(filepath)
        self.skd_version = skd_version
        self.anim_mode = anim_mode
        self.write_textures = write_textures
        self.apply_modifiers = apply_modifiers
        self.pk3_path = pk3_path
        self.warnings = []
        self.written = []
        if meshes is None:
            self.arm, self.meshes = find_objects(context, arm_obj)
        else:
            self.arm, self.meshes = arm_obj, list(meshes)
        holder = self.arm or self.meshes[0]
        self.tik_scale = float(scale if scale is not None else holder.get("mohaa_scale", 0.52))
        self.unit = float(holder.get("mohaa_unit", UNIT_METERS))
        self.stem = _file_safe(os.path.splitext(os.path.basename(self.filepath))[0])
        self.root = vfs.guess_root(self.filepath)
        self.data_dir = os.path.join(os.path.dirname(self.filepath), self.stem)
        if self.root:
            self.game_dir = vfs.game_path(self.data_dir, self.root)
        else:
            self.game_dir = "models/" + self.stem
            self.warn("%s is not under a models folder: the .tik expects its files in %s/"
                      % (self.filepath, self.game_dir))

    def warn(self, msg):
        self.warnings.append(msg)
        print("MOHAA export:", msg)

    # ------------------------------------------------------------------ space

    def model_matrix(self, obj):
        """Object space -> model space (raw units) for obj."""
        k = 1.0 / (self.tik_scale * self.unit)
        if self.arm is not None:
            arm_mw = self.arm.matrix_world
            loc, rot, scl = arm_mw.decompose()
            unscaled = Matrix.LocRotScale(loc, rot, Vector((1.0, 1.0, 1.0)))
            return Matrix.Scale(k, 4) @ unscaled.inverted() @ obj.matrix_world
        return Matrix.Scale(k, 4) @ obj.matrix_world

    def armature_scale(self):
        if self.arm is None:
            return 1.0
        s = self.arm.matrix_world.to_scale()
        return (s.x + s.y + s.z) / 3.0 / (self.tik_scale * self.unit)

    def engine_matrix(self, blender_matrix):
        """An armature-space bone matrix (Blender convention, Blender units) as an engine
        model-space bone matrix (raw units, engine bone axes)."""
        m = blender_matrix @ self.axis_inv
        loc, rot, _ = m.decompose()
        return Matrix.LocRotScale(loc * self.k_bones, rot, Vector((1.0, 1.0, 1.0)))

    # ------------------------------------------------------------------ skeleton

    def build_skeleton(self, actions):
        self.bones = []
        self.bone_index = {}
        if self.arm is None:
            b = _Bone("origin", None, skd.POSROT, Matrix.Identity(4))
            self.bones.append(b)
            self.bone_index["origin"] = 0
            self.deformers = {"origin": (0, b.rest)}
            self.axis_inv = Matrix.Identity(4)
            self.k_bones = 1.0
            return
        self.axis_inv = bone_axis_matrix(self.arm).inverted()
        self.k_bones = self.armature_scale()
        moving = set()
        for action in actions:
            for fc in animutil.all_fcurves(action):
                if fc.data_path.endswith(".location") and fc.data_path.startswith("pose.bones["):
                    vals = [kp.co[1] for kp in fc.keyframe_points]
                    if vals and max(abs(v) for v in vals) > 1e-4:
                        moving.add(fc.data_path.split('"')[1])
        order = []

        def visit(db):
            if "mohaa_proxy_of" in db:
                return  # an import's stand-in: its vertices belong to the real bone
            order.append(db)
            for c in db.children:
                visit(c)

        for db in self.arm.data.bones:
            if db.parent is None:
                visit(db)
        for db in order:
            name = db.name
            if len(skd.rotation_channel(name)) >= skd.CHANNEL_LEN:
                raise ExportError("bone name %r is too long (at most %d characters)" % (name, skd.CHANNEL_LEN - 5))
            t = db.get("mohaa_type")
            if t in skd.BONE_TYPE_NAMES:
                # an imported bone keeps its type: the game's own animations rely on it
                btype = skd.BONE_TYPE_NAMES.index(t)
            else:
                btype = skd.POSROT if db.parent is None or name in moving else skd.ROTATION
            b = _Bone(name, db.parent.name if db.parent else None, btype, self.engine_matrix(db.matrix_local))
            b.offset = None
            if "mohaa_offset" in db and "mohaa_rest_local" in db:
                # an imported bone keeps the engine's offset unless it was moved in Blender
                if db.parent is not None:
                    p_rest = self.engine_matrix(db.parent.matrix_local)
                    now = (p_rest.inverted_safe() @ b.rest).translation
                else:
                    now = b.rest.translation
                if (Vector(db["mohaa_rest_local"]) - now).length < 0.01:
                    b.offset = tuple(db["mohaa_offset"])
            if btype not in (skd.ROTATION, skd.POSROT, skd.ZERO):
                b.base_data = bytes.fromhex(db.get("mohaa_data", ""))
                b.channels = [c for c in db.get("mohaa_channels", "").split("|") if c]
                b.refs = [c for c in db.get("mohaa_refs", "").split("|") if c]
            self.bone_index[name] = len(self.bones)
            self.bones.append(b)
        if len(self.bones) > skd.MAX_BONES:
            raise ExportError("%d bones: the engine allows %d" % (len(self.bones), skd.MAX_BONES))
        # vertex groups -> (bone index, rest matrix the offsets are taken from)
        self.deformers = {b.name: (i, b.rest) for i, b in enumerate(self.bones)}
        for db in self.arm.data.bones:
            real = db.get("mohaa_proxy_of")
            if real in self.bone_index:
                self.deformers[db.name] = (self.bone_index[real], self.engine_matrix(db.matrix_local))

    def local_offset(self, b):
        if getattr(b, "offset", None) is not None:
            return b.offset
        if b.parent is None:
            return tuple(b.rest.translation)
        return tuple((self.bones[self.bone_index[b.parent]].rest.inverted_safe() @ b.rest).translation)

    # ------------------------------------------------------------------ meshes

    def _mesh_data(self, obj):
        """(mesh, temporary) with modifiers but without armature deformation."""
        if obj.data.shape_keys or not self.apply_modifiers:
            if obj.data.shape_keys and self.apply_modifiers and any(m.type != "ARMATURE" for m in obj.modifiers):
                self.warn("%s has shape keys: its other modifiers are not applied" % obj.name)
            return obj.data, False
        toggled = []
        for mod in obj.modifiers:
            if mod.type == "ARMATURE" and mod.show_viewport:
                mod.show_viewport = False
                toggled.append(mod)
        try:
            dg = self.context.evaluated_depsgraph_get()
            dg.update()
            ev = obj.evaluated_get(dg)
            me = bpy.data.meshes.new_from_object(ev, preserve_all_data_layers=True, depsgraph=dg)
        finally:
            for mod in toggled:
                mod.show_viewport = True
        return me, True

    def _weights(self, obj, me):
        """Per vertex [(bone index, weight, rest matrix)]."""
        groups = {g.index: g.name for g in obj.vertex_groups}
        fallback = None
        if obj.parent_type == "BONE" and obj.parent_bone in self.bone_index:
            fallback = self.bone_index[obj.parent_bone]
        if fallback is None:
            fallback = 0
        out = []
        missing = 0
        for v in me.vertices:
            ws = {}
            for g in v.groups:
                d = self.deformers.get(groups.get(g.group))
                if d is not None and g.weight > 1e-4:
                    key = (d[0], id(d[1]))
                    if key in ws:
                        ws[key] = (d[0], ws[key][1] + g.weight, d[1])
                    else:
                        ws[key] = (d[0], g.weight, d[1])
            ws = list(ws.values())
            total = sum(w for _, w, _ in ws)
            if total <= 0.0:
                ws = [(fallback, 1.0, self.bones[fallback].rest)]
                if self.arm is not None and obj.vertex_groups:
                    missing += 1
            else:
                ws = [(i, w / total, m) for i, w, m in ws]
            out.append(ws)
        if missing:
            self.warn("%s: %d vertices have no weight to a bone; they follow %s"
                      % (obj.name, missing, self.bones[fallback].name))
        return out

    def build_surfaces(self):
        """{skd file name: {"surfaces": [skd.Surface], "morphs": [names], "source": game path}}."""
        groups = {}
        self.surface_shaders = []  # (surface name, material, flags)
        for obj in self.meshes:
            me, temp = self._mesh_data(obj)
            try:
                self._mesh_surfaces(obj, me, groups)
            finally:
                if temp:
                    bpy.data.meshes.remove(me)
        return groups

    def _mesh_surfaces(self, obj, me, groups):
        source = obj.get("mohaa_skd", "")
        skd_name = _file_safe(os.path.basename(source)) if source else self.stem + ".skd"
        if not skd_name.lower().endswith(".skd"):
            skd_name = os.path.splitext(skd_name)[0] + ".skd"
        group = groups.setdefault(skd_name, {"surfaces": [], "morphs": [], "source": source})
        to_model = self.model_matrix(obj)
        rot_model = to_model.to_3x3()
        normal_model = rot_model.inverted_safe().transposed()
        me.calc_loop_triangles()
        if not me.loop_triangles:
            self.warn("%s has no faces" % obj.name)
            return
        uv_layer = me.uv_layers.active
        if uv_layer is None:
            self.warn("%s has no UV map" % obj.name)
        corner_normals = me.corner_normals if hasattr(me, "corner_normals") else None
        weights = self._weights(obj, me)
        positions = [to_model @ v.co for v in me.vertices]
        sharp = set()  # vertices on a sharp edge or a flat face
        for e in me.edges:
            if e.use_edge_sharp:
                sharp.update(e.vertices)
        for poly in me.polygons:
            if not poly.use_smooth:
                sharp.update(poly.vertices)

        # shape keys -> morph targets (model-space deltas)
        morph_deltas = {}
        keys = me.shape_keys
        if keys is not None and len(keys.key_blocks) > 1:
            ref = keys.reference_key
            for kb in keys.key_blocks:
                if kb == ref:
                    continue
                if kb.name not in group["morphs"]:
                    group["morphs"].append(kb.name)
                mi = group["morphs"].index(kb.name)
                for i, (a, b) in enumerate(zip(ref.data, kb.data)):
                    d = rot_model @ (b.co - a.co)
                    if d.length > 1e-6:
                        morph_deltas.setdefault(i, []).append((mi, d))

        flags = obj.get("mohaa_flags", "").split()
        by_material = {}
        for tri in me.loop_triangles:
            by_material.setdefault(tri.material_index, []).append(tri)
        base_name = _safe(obj.get("mohaa_surface", obj.name)).replace(" ", "_")
        attr_c = me.attributes.get("mohaa_collapse")
        attr_ci = me.attributes.get("mohaa_collapse_index")

        for mat_index, tris in sorted(by_material.items()):
            mat = obj.material_slots[mat_index].material if mat_index < len(obj.material_slots) else None
            name = base_name if len(by_material) == 1 else _safe("%s_%s" % (base_name, mat.name if mat else mat_index))
            keyed = {}
            normals_at = {}  # (vertex, uv) -> [(normal, key)]
            seen = 0
            tri_keys = []
            for tri in tris:
                corner = []
                for li, vi in zip(tri.loops, tri.vertices):
                    uv = tuple(round(c, 6) for c in uv_layer.data[li].uv) if uv_layer else (0.0, 0.0)
                    n = corner_normals[li].vector if corner_normals is not None else me.loops[li].normal
                    found = None
                    for other, k in normals_at.get((vi, uv), ()):
                        # a smooth vertex is one vertex whatever its corners say; at a sharp
                        # edge or flat face, corners more than 30 degrees apart are split
                        if vi not in sharp or other.dot(n) > 0.866:
                            found = k
                            break
                    if found is None:
                        found = (vi, uv, len(normals_at.get((vi, uv), ())))
                        normals_at.setdefault((vi, uv), []).append((n.copy(), found))
                        keyed[found] = [vi, seen, Vector((0.0, 0.0, 0.0))]
                        seen += 1
                    keyed[found][2] += n
                    corner.append(found)
                tri_keys.append(corner)
            for k in keyed.values():
                k[2] = k[2].normalized() if k[2].length > 1e-9 else Vector((0.0, 0.0, 1.0))
            # vertices in the mesh's own order, so an untouched import keeps its LOD data
            order = sorted(keyed, key=lambda k: (keyed[k][0], keyed[k][1]))
            index = {k: i for i, k in enumerate(order)}
            verts = []
            for key in order:
                vi, _, n = keyed[key]
                p = positions[vi]
                nm = (normal_model @ n).normalized()
                ws = []
                for bi, w, rest in weights[vi]:
                    ws.append((bi, w, tuple(rest.inverted_safe() @ p)))
                b0 = weights[vi][0][2].to_3x3()
                normal = tuple(b0.transposed() @ nm)
                morphs = []
                for mi, d in morph_deltas.get(vi, ()):
                    blend = Matrix(((0.0,) * 3,) * 3)
                    for _, w, rest in weights[vi]:
                        blend += rest.to_3x3() * w
                    m = blend.inverted_safe() @ d
                    morphs.append((mi, tuple(c / 100.0 for c in m)))
                u, v = key[1]
                verts.append(skd.Vertex(normal, (u, 1.0 - v), ws, morphs))
            triangles = [(index[c[0]], index[c[2]], index[c[1]]) for c in tri_keys]

            collapse = collapse_index = None
            if attr_c is not None and len(by_material) == 1 and [keyed[k][0] for k in order] == list(range(len(me.vertices))):
                collapse = [d.value for d in attr_c.data]
                collapse_index = [d.value for d in attr_ci.data] if attr_ci is not None else None

            for piece_name, piece_verts, piece_tris, c, ci in self._split(name, verts, triangles,
                                                                          collapse, collapse_index):
                s = skd.Surface(piece_name)
                s.verts = piece_verts
                s.triangles = piece_tris
                s.collapse = c
                s.collapse_index = ci
                group["surfaces"].append(s)
                self.surface_shaders.append((piece_name, mat, flags))

    def _split(self, name, verts, triangles, collapse, collapse_index):
        if len(verts) <= skd.MAX_VERTS and len(triangles) <= skd.MAX_TRIS:
            return [(name, verts, triangles, collapse, collapse_index)]
        pieces = []
        cur_tris, cur_map = [], {}
        for t in triangles:
            new = [i for i in t if i not in cur_map]
            if len(cur_tris) + 1 > skd.MAX_TRIS or len(cur_map) + len(new) > skd.MAX_VERTS:
                pieces.append((cur_tris, cur_map))
                cur_tris, cur_map = [], {}
            for i in t:
                if i not in cur_map:
                    cur_map[i] = len(cur_map)
            cur_tris.append(tuple(cur_map[i] for i in t))
        if cur_tris:
            pieces.append((cur_tris, cur_map))
        self.warn("surface %s is over the engine's %d vertices / %d triangles: split in %d"
                  % (name, skd.MAX_VERTS, skd.MAX_TRIS, len(pieces)))
        out = []
        for n, (tris, mapping) in enumerate(pieces):
            inv = sorted(mapping, key=mapping.get)
            out.append(("%s_%d" % (name[:60], n) if n else name, [verts[i] for i in inv], tris, None, None))
        return out

    # ------------------------------------------------------------------ animations

    def actions_to_export(self):
        if self.arm is None or self.anim_mode == "NONE":
            return []
        if self.anim_mode == "ACTIVE":
            ad = self.arm.animation_data
            return [ad.action] if ad and ad.action else []
        names = set(self.arm.data.bones.keys())
        out = []
        for action in bpy.data.actions:
            if action.get("mohaa_skip"):
                continue
            owner = action.get("mohaa_armature")
            if owner and owner != self.arm.name:
                continue  # imported for another armature
            curves = animutil.all_fcurves(action)
            bones = {fc.data_path.split('"')[1] for fc in curves if fc.data_path.startswith("pose.bones[")}
            if bones and bones & names:
                out.append(action)
        return out

    def _bone_channels(self, b):
        if b.type == skd.ROTATION:
            return [(skd.rotation_channel(b.name), "rot", False)]
        if b.type == skd.POSROT:
            return [(skd.rotation_channel(b.name), "rot", False), (skd.position_channel(b.name), "pos", False)]
        if b.type == skd.IKWRIST:
            names = b.channels or [skd.rotation_channel(b.name), skd.position_channel(b.name)]
            return [(names[0], "rot", True), (names[1] if len(names) > 1 else skd.position_channel(b.name), "pos", True)]
        return []

    def _channel_layout(self, morph_names):
        layout = []
        for b in self.bones:
            for name, kind, model_space in self._bone_channels(b):
                layout.append((name, b, kind, model_space))
        for m in morph_names:
            layout.append((m, None, "morph", False))
        return layout

    def _frame_values(self, layout, world):
        """One row of channel values from engine model-space bone matrices."""
        row = []
        for name, b, kind, model_space in layout:
            if b is None:
                row.append((self._morph_value(name), 0.0, 0.0, 0.0))
                continue
            m = world[b.name]
            if not model_space and b.parent is not None:
                m = world[b.parent].inverted_safe() @ m
            if kind == "rot":
                r = [[m[i][j] for j in range(3)] for i in range(3)]
                row.append(pose.matrix_to_quat(r))
            else:
                t = m.translation
                row.append((t.x, t.y, t.z, 0.0))
        return row

    def _morph_value(self, name):
        for obj in self.meshes:
            keys = obj.data.shape_keys
            if keys and name in keys.key_blocks:
                return keys.key_blocks[name].value * 100.0
        return 0.0

    def _skin_setup(self, groups):
        import numpy as np
        idx, w, off, vert = [], [], [], []
        n = 0
        for g in groups.values():
            for s in g["surfaces"]:
                for v in s.verts:
                    for bi, bw, o in v.weights:
                        idx.append(bi)
                        w.append(bw)
                        off.append(o)
                        vert.append(n)
                    n += 1
        self._skin = (np.array(idx, dtype=np.int64), np.array(w), np.array(off, dtype=np.float64).reshape(-1, 3),
                      np.array(vert, dtype=np.int64), n)

    def _bounds(self, world):
        import numpy as np
        idx, w, off, vert, n = self._skin
        if not n:
            return ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)), 0.0
        R = np.array([[[world[b.name][i][j] for j in range(3)] for i in range(3)] for b in self.bones])
        T = np.array([[world[b.name][i][3] for i in range(3)] for b in self.bones])
        p = np.einsum("nij,nj->ni", R[idx], off) + T[idx]
        pos = np.zeros((n, 3))
        np.add.at(pos, vert, p * w[:, None])
        lo, hi = pos.min(axis=0), pos.max(axis=0)
        radius = float(np.sqrt((pos * pos).sum(axis=1)).max())
        return (tuple(lo), tuple(hi)), radius

    def _rest_world(self):
        return {b.name: b.rest for b in self.bones}

    def sample_action(self, action, layout):
        scene = self.context.scene
        fps = scene.render.fps / scene.render.fps_base
        frame_time = float(action.get("mohaa_frame_time", 1.0 / fps))
        start, end = animutil.frame_range(action)
        step = frame_time * fps
        count = max(1, int(round((end - start) / step)) + 1)
        animutil.assign(self.arm, action)
        for obj in self.meshes:
            keys = obj.data.shape_keys
            if keys is not None and any(fc.data_path.startswith("key_blocks[") for fc in animutil.all_fcurves(action)):
                animutil.assign(keys, action)
        a = skc.Animation()
        a.frame_time = frame_time
        a.flags = int(action.get("mohaa_flags", 0))
        a.channels = [c[0] for c in layout]
        for i in range(count):
            t = start + i * step
            f = int(math.floor(t))
            scene.frame_set(f, subframe=t - f)
            world = {pb.name: self.engine_matrix(pb.matrix) for pb in self.arm.pose.bones}
            a.values.append(self._frame_values(layout, world))
            bounds, radius = self._bounds(world)
            a.frames.append(skc.Frame(bounds, radius))
        deltas = list(action.get("mohaa_deltas", []))
        angles = list(action.get("mohaa_angle_deltas", []))
        if len(deltas) == 3 * count:
            for i, fr in enumerate(a.frames):
                fr.delta = tuple(deltas[3 * i:3 * i + 3])
                fr.angle_delta = angles[i] if i < len(angles) else 0.0
            a.total_delta = tuple(action.get("mohaa_total_delta", (0.0, 0.0, 0.0)))
            a.total_angle_delta = float(action.get("mohaa_total_angle_delta", 0.0))
        return a

    def rest_animation(self, layout):
        a = skc.Animation()
        a.frame_time = 0.05
        a.channels = [c[0] for c in layout]
        world = self._rest_world()
        a.values.append(self._frame_values(layout, world))
        bounds, radius = self._bounds(world)
        a.frames.append(skc.Frame(bounds, radius))
        return a

    # ------------------------------------------------------------------ shaders and textures

    def shader_for(self, mat, shader_lines):
        if mat is None:
            self.warn("a surface has no material: it will draw the engine's missing texture")
            return "noshader"
        img = materials.material_image(mat)
        kept = mat.get("mohaa_shader")
        if kept and (img is None or img.get("mohaa_path", "").lower() == mat.get("mohaa_image", "").lower()):
            return kept  # the game has this shader already
        if img is None:
            if kept:
                return kept
            self.warn("material %s has no image texture: using its name as the shader" % mat.name)
            return _file_safe(mat.name)
        tex_name = _file_safe(os.path.splitext(img.name)[0]) + ".tga"
        if self.write_textures:
            out = os.path.join(self.data_dir, tex_name)
            if out not in self.written:
                materials.save_image_tga(img, out)
                self.written.append(out)
        tex_game = self.game_dir + "/" + tex_name
        alpha = False
        bsdf = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None) if mat.node_tree else None
        if bsdf is not None and bsdf.inputs["Alpha"].is_linked:
            alpha = True
        blended = getattr(mat, "surface_render_method", "") == "BLENDED" or getattr(mat, "blend_method", "") == "BLEND"
        two_sided = not mat.use_backface_culling
        if alpha or two_sided:
            name = self.game_dir + "/" + os.path.splitext(tex_name)[0]
            stage = ["\t{", "\t\tmap %s" % tex_game]
            if alpha and blended:
                stage.append("\t\tblendFunc blend")
            elif alpha:
                stage.append("\t\talphaFunc GE128")
                stage.append("\t\tdepthWrite")
            stage += ["\t\trgbGen lightingDiffuse", "\t}"]
            body = [name, "{"] + (["\tcull none"] if two_sided else []) + stage + ["}", ""]
            if name not in shader_lines:
                shader_lines[name] = "\n".join(body)
            return name
        return tex_name  # a dot makes the TIKI read it as a file in the model's folder

    # ------------------------------------------------------------------ writing

    def _write(self, path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        mode = "w" if isinstance(data, str) else "wb"
        with open(path, mode, newline="\n" if mode == "w" else None) as f:
            f.write(data)
        self.written.append(path)

    def _bones_for(self, surfaces, all_groups):
        """The bones one .skd carries: all of them when it is the only one, else the bones
        its weights use, their parents and the bones those reference."""
        if all_groups == 1:
            return list(range(len(self.bones)))
        need = set()
        for s in surfaces:
            for v in s.verts:
                for bi, _, _ in v.weights:
                    need.add(bi)
        changed = True
        while changed:
            changed = False
            for bi in list(need):
                b = self.bones[bi]
                extra = ([b.parent] if b.parent else []) + list(b.refs)
                for name in extra:
                    j = self.bone_index.get(name)
                    if j is not None and j not in need:
                        need.add(j)
                        changed = True
        return sorted(need)

    def run(self):
        actions = self.actions_to_export()
        self.build_skeleton(actions)
        groups = self.build_surfaces()
        if not any(g["surfaces"] for g in groups.values()):
            raise ExportError("nothing to export: no faces")
        self._skin_setup(groups)
        info = json.loads(self.arm.get("mohaa_models", "{}")) if self.arm is not None else {}

        # .skd files, in the original TIKI's order: the engine takes a bone two files share
        # from the first one listed
        order = list(info)
        ranked = sorted(groups.items(), key=lambda kv: order.index(kv[1]["source"]) if kv[1]["source"] in order
                        else len(order))
        skd_names = []
        all_morphs = []
        for skd_name, g in ranked:
            keep = self._bones_for(g["surfaces"], len(groups))
            remap = {old: new for new, old in enumerate(keep)}
            m = skd.Model(skd_name)
            meta = info.get(g["source"], {})
            m.lod_index = meta.get("lod_index", [0] * 10)
            m.scale = meta.get("scale", 1.0)
            own = meta.get("bones", {})
            for old in keep:
                b = self.bones[old]
                sb = skd.Bone(b.name, b.parent, b.type, self.local_offset(b))
                sb.base_data, sb.channels, sb.refs = b.base_data, b.channels, b.refs
                rec = own.get(b.name)
                if rec and len(rec) >= 5 and getattr(b, "offset", None) is not None:
                    # unmoved imported bone: this file's own record (the engine uses the
                    # first file's copy of a shared bone; each file keeps its own)
                    sb.type = rec[0]
                    sb.base_data = bytes.fromhex(rec[1])
                    sb.offset = tuple(rec[2])
                    sb.channels, sb.refs = list(rec[3]), list(rec[4])
                m.bones.append(sb)
            for s in g["surfaces"]:
                for v in s.verts:
                    v.weights = [(remap[bi], w, o) for bi, w, o in v.weights]
            m.surfaces = g["surfaces"]
            m.morph_names = g["morphs"]
            m.boxes = [i for i in (m.bone_index(n) for n in meta.get("boxes", [])) if i >= 0]
            errors = skd.validate(m)
            if errors:
                raise ExportError("; ".join(errors))
            self._write(os.path.join(self.data_dir, skd_name), skd.write(m, self.skd_version))
            for s in g["surfaces"]:  # back to global indices for bounds
                inv = {new: old for old, new in remap.items()}
                for v in s.verts:
                    v.weights = [(inv[bi], w, o) for bi, w, o in v.weights]
            skd_names.append(skd_name)
            all_morphs += [n for n in g["morphs"] if n not in all_morphs]

        # .skc files
        layout = self._channel_layout(all_morphs)
        anims = []
        scene = self.context.scene
        saved_frame = scene.frame_current
        saved_action = self.arm.animation_data.action if self.arm is not None and self.arm.animation_data else None
        saved_slot = getattr(self.arm.animation_data, "action_slot", None) if saved_action else None
        used_files = set()
        try:
            for action in actions:
                a = self.sample_action(action, layout)
                fname = _file_safe(os.path.basename(action.get("mohaa_skc", "")) or action.name + ".skc")
                if not fname.lower().endswith(".skc"):
                    fname += ".skc"
                while fname.lower() in used_files:
                    fname = os.path.splitext(fname)[0] + "_.skc"
                used_files.add(fname.lower())
                self._write(os.path.join(self.data_dir, fname), skc.write(a))
                aliases = json.loads(action.get("mohaa_aliases", "[]")) or [[_safe(action.name).replace(" ", "_"), [], ""]]
                for alias, options, block in aliases:
                    anims.append((alias, fname, options, block))
        finally:
            if self.arm is not None:
                ad = self.arm.animation_data
                if ad is not None:
                    ad.action = saved_action
                    if saved_slot is not None and hasattr(ad, "action_slot"):
                        ad.action_slot = saved_slot
            scene.frame_set(saved_frame)
        if not anims:
            fname = "idle.skc"
            self._write(os.path.join(self.data_dir, fname), skc.write(self.rest_animation(layout)))
            anims.append(("idle", fname, [], ""))

        # shaders, textures and the .tik
        shader_lines = {}
        surfaces = []
        seen = set()
        for name, mat, flags in self.surface_shaders:
            if name.lower() in seen:
                continue
            seen.add(name.lower())
            surfaces.append((name, self.shader_for(mat, shader_lines), flags))
        if shader_lines:
            if self.root:
                path = os.path.join(self.root, "scripts", self.stem + ".shader")
            else:
                path = os.path.join(os.path.dirname(self.filepath), "scripts", self.stem + ".shader")
            self._write(path, "\n".join(shader_lines.values()))
        holder = self.arm or self.meshes[0]
        extra = [l for l in holder.get("mohaa_setup", "").splitlines() if l.strip()]
        text = tiki.write(self.stem, self.game_dir, skd_names, surfaces, anims, scale=self.tik_scale,
                          init_block=holder.get("mohaa_init", ""), extra_setup=extra)
        self._write(self.filepath, text)

        if self.pk3_path:
            self.write_pk3()
        return self.written, self.warnings

    def write_pk3(self):
        base = self.root or os.path.dirname(self.filepath)
        with zipfile.ZipFile(self.pk3_path, "w", zipfile.ZIP_DEFLATED) as z:
            for path in self.written:
                if path == self.pk3_path:
                    continue
                if self.root:
                    arc = os.path.relpath(path, base).replace("\\", "/")
                elif path == self.filepath:
                    arc = "models/" + os.path.basename(path)
                elif path.startswith(self.data_dir):
                    arc = self.game_dir + "/" + os.path.relpath(path, self.data_dir).replace("\\", "/")
                else:
                    arc = os.path.relpath(path, base).replace("\\", "/")
                z.write(path, arc)
        self.written.append(self.pk3_path)


def export_model(context, filepath, **kw):
    return Exporter(context, filepath, **kw).run()


def export_action(context, filepath, arm_obj, action=None):
    """One action as a .skc for an armature (no mesh files): for adding animations to a
    model the game already has."""
    ex = Exporter(context, filepath + ".tik" if not filepath.lower().endswith(".tik") else filepath, arm_obj=arm_obj,
                  meshes=[o for o in bpy.data.objects if o.type == "MESH" and o.find_armature() == arm_obj] or [arm_obj])
    action = action or (arm_obj.animation_data.action if arm_obj.animation_data else None)
    if action is None:
        raise ExportError("%s has no action to export" % arm_obj.name)
    ex.build_skeleton([action])
    groups = ex.build_surfaces() if all(o.type == "MESH" for o in ex.meshes) else {}
    ex._skin_setup(groups)
    morphs = []
    for g in groups.values():
        morphs += [n for n in g["morphs"] if n not in morphs]
    layout = ex._channel_layout(morphs)
    saved = arm_obj.animation_data.action if arm_obj.animation_data else None
    try:
        a = ex.sample_action(action, layout)
    finally:
        if saved is not None:
            animutil.assign(arm_obj, saved)
    out = filepath if filepath.lower().endswith(".skc") else filepath + ".skc"
    with open(out, "wb") as f:
        f.write(skc.write(a))
    return out, ex.warnings
