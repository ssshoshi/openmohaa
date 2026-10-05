"""MOHAA models (.tik, .skd, .skb) and animations (.skc) into Blender.

A model becomes an armature with one mesh object per surface. Its rest pose is the first
frame of a reference animation (MOHAA meshes have no bind pose of their own: a vertex is
stored as offsets from the bones that move it), "idle" unless told otherwise.

Every bone the engine computes (IK legs, shoulder and hip helpers) is evaluated with the
engine's solver for every frame, so an imported animation plays back in Blender as it does
in game. See mohaa/pose.py.
"""

import fnmatch
import json
import os

import bpy
from mathutils import Matrix

from . import anim as animutil
from . import materials
from .mohaa import pose, shader, skc, skd, tiki, vfs

UNIT_METERS = 0.3048 / 16.0  # the world is 16 units to the foot

# Blender bones point along their Y axis, MOHAA's (3ds Max Biped) along X. With bone axis
# "MOHAA" a Blender bone's matrix is the engine's times BONE_AXIS, so bones display along
# their length; the exporter undoes it. Columns: X <- -Y, Y <- X, Z <- Z.
BONE_AXIS = Matrix(((0.0, 1.0, 0.0, 0.0), (-1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 1.0, 0.0), (0.0, 0.0, 0.0, 1.0)))


def bone_axis_matrix(arm_obj):
    return BONE_AXIS.copy() if arm_obj.data.get("mohaa_bone_axis", "") == "MOHAA" else Matrix.Identity(4)


class ImportError_(Exception):
    pass


class Context:
    """What an import works from: the game's files and where the picked file sits."""

    def __init__(self, filepath, folders):
        self.filepath = os.path.abspath(filepath)
        self.fs = vfs.GameFS(folders)
        self.root = vfs.guess_root(self.filepath)
        if self.root:
            self.fs.add_loose_root(self.root)
            self.game_path = vfs.game_path(self.filepath, self.root)
        else:
            self.game_path = os.path.basename(self.filepath)
        self.warnings = []

    def read(self, path):
        data = self.fs.read(path)
        if data is None:
            # a file outside any game folder: look next to the picked file
            local = os.path.join(os.path.dirname(self.filepath), os.path.basename(path))
            if path.lower() == self.game_path.lower():
                local = self.filepath
            if os.path.isfile(local):
                with open(local, "rb") as f:
                    data = f.read()
        return data

    def read_text(self, path):
        data = self.read(path)
        return None if data is None else data.decode("latin-1")

    def warn(self, msg):
        self.warnings.append(msg)
        print("MOHAA import:", msg)


def _m(rows):
    return Matrix(rows)


def _bone_lengths(skeleton, world):
    heads = {k: (m[0][3], m[1][3], m[2][3]) for k, m in world.items()}
    children = {}
    for b in skeleton.bones:
        if b.parent:
            children.setdefault(b.parent.lower(), []).append(b.name.lower())
    out = {}
    for b in skeleton.bones:
        key = b.name.lower()
        h = heads[key]
        best = 0.0
        for c in children.get(key, ()):
            ch = heads.get(c)
            if ch:
                best = max(best, sum((ch[i] - h[i]) ** 2 for i in range(3)) ** 0.5)
        out[key] = min(max(best, 1.0), 20.0) if best else 2.0
    return out


def _cases(text):
    out = {}
    for part in (text or "").replace(",", " ").split():
        if "=" in part:
            k, v = part.split("=", 1)
            out[k.strip().lower()] = v.strip().lower()
    return out


def _pick_reference(anims, wanted):
    if wanted:
        for a in anims:
            if a.alias.lower() == wanted.lower():
                return a
    for a in anims:
        if a.alias.lower() == "idle":
            return a
    for a in anims:
        if a.alias.lower().startswith("idle"):
            return a
    return anims[0] if anims else None


def load_model(ctx, path, cases="", is_tiki=None):
    """(Tiki, [(game path, skd.Model)]) for a .tik, or a .skd/.skb on its own."""
    if is_tiki is None:
        is_tiki = path.lower().endswith(".tik")
    if is_tiki:
        tk = tiki.parse(ctx.read_text, path, _cases(cases))
    else:
        tk = tiki.Tiki()
        tk.path = path
        tk.scale = 0.52
        tk.skelmodels = [path]
        stem = os.path.splitext(path)[0]
        for ext in (".skc", ".SKC"):
            if ctx.read(stem + ext) is not None:
                tk.anims.append(tiki.Anim("idle", stem + ext))
                break
    models = []
    for p in tk.skelmodels:
        data = ctx.read(p)
        if data is None:
            ctx.warn("missing model %s" % p)
            continue
        models.append((p, skd.read(data)))
    if not models:
        raise ImportError_("no model files could be read for %s" % path)
    return tk, models


# --------------------------------------------------------------------------- building

def _new_collection(context, name):
    coll = bpy.data.collections.new(name)
    context.scene.collection.children.link(coll)
    return coll


def build_armature(context, coll, name, skeleton, world, axis, proxies=None):
    data = bpy.data.armatures.new(name)
    data.display_type = "STICK"
    obj = bpy.data.objects.new(name, data)
    coll.objects.link(obj)
    view_layer = context.view_layer
    for o in list(context.selected_objects):
        if o is not None:
            o.select_set(False)
    view_layer.objects.active = obj
    obj.select_set(True)
    lengths = _bone_lengths(skeleton, world)
    bpy.ops.object.mode_set(mode="EDIT")
    try:
        ebs = {}
        for b in skeleton.bones:
            eb = data.edit_bones.new(b.name)
            eb.head = (0.0, 0.0, 0.0)
            eb.tail = (0.0, lengths[b.name.lower()], 0.0)
            eb.matrix = _m(world[b.name.lower()]) @ axis
            ebs[b.name.lower()] = eb
        for b in skeleton.bones:
            if b.parent and b.parent.lower() in ebs:
                ebs[b.name.lower()].parent = ebs[b.parent.lower()]
        for pname, (real, matrix) in (proxies or {}).items():
            eb = data.edit_bones.new(pname)
            eb.head = (0.0, 0.0, 0.0)
            eb.tail = (0.0, lengths[real] * 0.5, 0.0)
            eb.matrix = _m(matrix) @ axis
            eb.parent = ebs[real]
    finally:
        bpy.ops.object.mode_set(mode="OBJECT")
    if proxies:
        hidden = data.collections.new("MOHAA proxies") if hasattr(data, "collections") else None
        for pname, (real, _) in proxies.items():
            db = data.bones[pname]
            db["mohaa_proxy_of"] = skeleton.by_name[real].name
            if hidden is not None:
                hidden.assign(db)
            con = obj.pose.bones[pname].constraints.new("COPY_TRANSFORMS")
            con.target = obj
            con.subtarget = skeleton.by_name[real].name
        if hidden is not None:
            hidden.is_visible = False
    for b in skeleton.bones:
        db = data.bones[b.name]
        db["mohaa_type"] = skd.BONE_TYPE_NAMES[b.type] if 0 <= b.type < len(skd.BONE_TYPE_NAMES) else str(b.type)
        # the offset the engine places the bone at, and where the rest pose has it: the
        # exporter keeps the first unless the bone was moved
        db["mohaa_offset"] = list(b.offset)
        parent = b.parent.lower() if b.parent and b.parent.lower() in world else None
        local = (_m(world[parent]).inverted_safe() @ _m(world[b.name.lower()])) if parent else _m(world[b.name.lower()])
        db["mohaa_rest_local"] = list(local.translation)
        if b.type not in (skd.ROTATION, skd.POSROT):
            db["mohaa_data"] = b.base_data.hex()
            db["mohaa_channels"] = "|".join(b.channels)
            db["mohaa_refs"] = "|".join(b.refs)
    return obj


def build_surface(ctx, coll, arm_obj, model_path, model, surf, positions, weights, matrices, material, flags,
                  custom_normals):
    """A mesh object for one surface. positions and weights come from pose.bind; matrices
    maps every deforming bone name (lower, proxies included) to its rest matrix."""
    verts = positions
    faces, dropped = [], 0
    for a, b, c in surf.triangles:
        if a == b or b == c or a == c or max(a, b, c) >= len(verts):
            dropped += 1
            continue
        faces.append((a, c, b))  # files wind clockwise
    if dropped:
        ctx.warn("%s/%s: %d degenerate triangles left out" % (model_path, surf.name, dropped))
    me = bpy.data.meshes.new(surf.name)
    me.from_pydata(verts, [], faces)
    if faces:
        uv = me.uv_layers.new(name="UVMap")
        flat = []
        for poly in me.polygons:
            for li in poly.loop_indices:
                u, v = surf.verts[me.loops[li].vertex_index].uv
                flat += (u, 1.0 - v)
        uv.data.foreach_set("uv", flat)
        me.polygons.foreach_set("use_smooth", [True] * len(me.polygons))
    if surf.collapse:
        a = me.attributes.new("mohaa_collapse", "INT", "POINT")
        a.data.foreach_set("value", surf.collapse)
    if surf.collapse_index:
        a = me.attributes.new("mohaa_collapse_index", "INT", "POINT")
        a.data.foreach_set("value", surf.collapse_index)
    me.update()
    if custom_normals and faces:
        normals = []
        for v, ws in zip(surf.verts, weights):
            m = matrices.get(ws[0][0].lower(), pose.IDENTITY) if ws else pose.IDENTITY
            n = pose.rotate(m, v.normal)  # the engine turns a normal with its first bone
            ln = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) ** 0.5 or 1.0
            normals.append((n[0] / ln, n[1] / ln, n[2] / ln))
        me.normals_split_custom_set_from_vertices(normals)
    if material is not None:
        me.materials.append(material)

    obj = bpy.data.objects.new(surf.name, me)
    coll.objects.link(obj)
    obj.parent = arm_obj
    obj["mohaa_surface"] = surf.name
    obj["mohaa_skd"] = model_path
    if flags:
        obj["mohaa_flags"] = " ".join(flags)
    mod = obj.modifiers.new("Armature", "ARMATURE")
    mod.object = arm_obj

    groups = {}
    for i, ws in enumerate(weights):
        for bone_name, bw in ws:
            groups.setdefault(bone_name, {}).setdefault(bw, []).append(i)
    for bone_name, by_weight in groups.items():
        vg = obj.vertex_groups.new(name=bone_name)
        for w, idx in by_weight.items():
            vg.add(idx, w, "REPLACE")

    if any(v.morphs for v in surf.verts):
        obj.shape_key_add(name="Basis", from_mix=False)
        used = sorted({mi for v in surf.verts for mi, _ in v.morphs})
        for mi in used:
            key_name = model.morph_names[mi] if mi < len(model.morph_names) else "morph%d" % mi
            kb = obj.shape_key_add(name=key_name, from_mix=False)
            co = list(verts)
            for i, v in enumerate(surf.verts):
                total = [0.0, 0.0, 0.0]
                for m_i, off in v.morphs:
                    if m_i != mi:
                        continue
                    for bone_name, bw in weights[i]:
                        m = matrices.get(bone_name.lower(), pose.IDENTITY)
                        d = pose.rotate(m, off)
                        for k in range(3):
                            total[k] += bw * d[k] * 100.0  # a channel's 100 is the full target
                if total != [0.0, 0.0, 0.0]:
                    co[i] = (co[i][0] + total[0], co[i][1] + total[1], co[i][2] + total[2])
            kb.data.foreach_set("co", [c for p in co for c in p])
            kb.slider_max = 1.0
            kb.value = 0.0  # shape_key_add leaves new keys at 1 in recent Blenders
    return obj


def _world_to_basis(skeleton, rest, world, axis, axis_inv):
    """{bone: (location, quaternion)} pose basis for each bone, for an engine pose."""
    out = {}
    rest_m = {k: _m(v) for k, v in rest.items()}
    pose_m = {k: _m(v) for k, v in world.items()}
    for b in skeleton.bones:
        key = b.name.lower()
        parent = b.parent.lower() if b.parent and b.parent.lower() in rest_m else None
        if parent:
            le_rest = rest_m[parent].inverted_safe() @ rest_m[key]
            le = pose_m[parent].inverted_safe() @ pose_m[key]
        else:
            le_rest = rest_m[key]
            le = pose_m[key]
        basis = axis_inv @ le_rest.inverted_safe() @ le @ axis
        loc, rot, _scale = basis.decompose()
        out[key] = (loc, rot)
    return out


def build_action(ctx, arm_obj, skeleton, rest, animation, name, axis, meshes, fps):
    axis_inv = axis.inverted()
    action = bpy.data.actions.new(name)
    action.use_fake_user = True
    action["mohaa_armature"] = arm_obj.name
    nframes = animation.num_frames
    step = animation.frame_time * fps
    frames = [1.0 + i * step for i in range(nframes)]
    tracks = {b.name.lower(): ([], []) for b in skeleton.bones}
    for f in range(nframes):
        world = skeleton.evaluate(pose.frame_channels(animation, f))
        basis = _world_to_basis(skeleton, rest, world, axis, axis_inv)
        for key, (loc, rot) in basis.items():
            locs, rots = tracks[key]
            if rots and rots[-1].dot(rot) < 0.0:
                rot.negate()  # keep quaternions on one side so they interpolate the short way
            locs.append(loc)
            rots.append(rot)
    animutil.assign(arm_obj, action)
    for b in skeleton.bones:
        locs, rots = tracks[b.name.lower()]
        if all(l.length < 1e-6 for l in locs) and all(abs(r.w - 1.0) < 1e-7 for r in rots):
            continue  # never moves from rest
        pb = arm_obj.pose.bones[b.name]
        pb.rotation_mode = "QUATERNION"
        base = 'pose.bones["%s"]' % bpy.utils.escape_identifier(b.name)
        for i in range(3):
            fc = animutil.new_fcurve(action, arm_obj, base + ".location", i, b.name)
            animutil.set_keys(fc, frames, [l[i] for l in locs])
        for i in range(4):
            fc = animutil.new_fcurve(action, arm_obj, base + ".rotation_quaternion", i, b.name)
            animutil.set_keys(fc, frames, [r[i] for r in rots])

    # morph target channels drive shape keys
    morph_channels = [c for c in animation.channels if skc.channel_type(c) == skc.CHANNEL_VALUE]
    if morph_channels:
        for obj in meshes:
            key = obj.data.shape_keys
            if key is None:
                continue
            names = {kb.name.lower(): kb.name for kb in key.key_blocks}
            names.update({kb.name.lower().rstrip("_"): kb.name for kb in key.key_blocks})
            done = False
            for c in morph_channels:
                kb = names.get(c.lower()) or names.get(c.lower().rstrip("_"))
                if not kb:
                    continue
                if not done:
                    animutil.assign(key, action)
                    done = True
                values = [v[0] / 100.0 for v in animation.channel(c)]
                path = 'key_blocks["%s"].value' % bpy.utils.escape_identifier(kb)
                fc = animutil.new_fcurve(action, key, path, 0, "Shape Keys")
                animutil.set_keys(fc, frames, values)

    action.use_frame_range = True
    action.frame_start = 1.0
    action.frame_end = max(1.0, frames[-1]) if frames else 1.0
    action["mohaa_frame_time"] = animation.frame_time
    action["mohaa_flags"] = animation.flags
    action["mohaa_total_delta"] = list(animation.total_delta)
    action["mohaa_total_angle_delta"] = animation.total_angle_delta
    action["mohaa_deltas"] = [c for fr in animation.frames for c in fr.delta]
    action["mohaa_angle_deltas"] = [fr.angle_delta for fr in animation.frames]
    return action


def import_model(context, filepath, folders, anim_mode="REFERENCE", anim_filter="", reference="",
                 cases="", unit="METERS", bone_axis="MOHAA", load_textures=True, pack_images=False,
                 extract_dir=None, custom_normals=True):
    """Import a .tik, .skd or .skb. Returns (armature object, warnings)."""
    ctx = Context(filepath, folders)
    tk, models = load_model(ctx, ctx.game_path, cases)
    name = os.path.splitext(os.path.basename(filepath))[0]

    # animations: the reference (rest pose) and the ones asked for
    available = [a for a in tk.anims]
    ref_anim = _pick_reference(available, reference)
    loaded = {}

    def load_anim(a):
        if a.path.lower() not in loaded:
            data = ctx.read(a.path)
            if data is None:
                ctx.warn("missing animation %s (%s)" % (a.path, a.alias))
                loaded[a.path.lower()] = None
            else:
                try:
                    loaded[a.path.lower()] = skc.read(data)
                except ValueError as e:
                    ctx.warn("%s: %s" % (a.path, e))
                    loaded[a.path.lower()] = None
        return loaded[a.path.lower()]

    ref_data = load_anim(ref_anim) if ref_anim else None

    skeleton = pose.Skeleton([b for _, m in models for b in m.bones])
    reference_pose = skeleton.evaluate(pose.frame_channels(ref_data, 0))
    model_list = [m for _, m in models]
    rest, _ = pose.bind_pose(skeleton, model_list, reference_pose)
    binding = pose.bind(skeleton, model_list, rest)
    matrices = dict(rest)
    matrices.update({n.lower(): m for n, (_, m) in binding.proxies.items()})
    axis = BONE_AXIS.copy() if bone_axis == "MOHAA" else Matrix.Identity(4)

    coll = _new_collection(context, name)
    arm_obj = build_armature(context, coll, name, skeleton, rest, axis, binding.proxies)
    arm = arm_obj.data
    arm["mohaa_bone_axis"] = bone_axis
    arm_obj["mohaa_tiki"] = tk.path if tk.path.lower().endswith(".tik") else ""
    arm_obj["mohaa_scale"] = tk.scale
    arm_obj["mohaa_unit"] = UNIT_METERS if unit == "METERS" else 1.0
    arm_obj["mohaa_init"] = tk.init_block
    if ref_anim:
        arm_obj["mohaa_reference"] = ref_anim.alias
    extra = []
    if tk.lod_scale is not None:
        extra.append("lod_scale %g" % tk.lod_scale)
    if tk.lod_bias is not None:
        extra.append("lod_bias %g" % tk.lod_bias)
    if tk.origin:
        extra.append("origin %g %g %g" % tk.origin)
    if tk.lightoffset:
        extra.append("lightoffset %g %g %g" % tk.lightoffset)
    if tk.radius is not None:
        extra.append("radius %g" % tk.radius)
    if tk.ischaracter:
        extra.append("ischaracter")
    arm_obj["mohaa_setup"] = "\n".join(extra)
    arm_obj["mohaa_models"] = json.dumps({p: {"version": m.version, "name": m.name, "lod_index": m.lod_index,
                                              "boxes": [m.bones[i].name for i in m.boxes if 0 <= i < len(m.bones)],
                                              "scale": m.scale,
                                              # each file's own bone records (a bone two
                                              # files share can differ between them)
                                              "bones": {b.name: [b.type, b.base_data.hex(), list(b.offset),
                                                                  b.channels, b.refs] for b in m.bones}}
                                          for p, m in models})
    s = tk.scale * (UNIT_METERS if unit == "METERS" else 1.0)
    arm_obj.scale = (s, s, s)

    # meshes and materials
    library = shader.ShaderLibrary(ctx.fs) if load_textures else None
    if extract_dir is None:
        extract_dir = os.path.join(bpy.app.tempdir or ".", "mohaa_textures")
    meshes = []
    for mi, (path, model) in enumerate(models):
        for si, surf in enumerate(model.surfaces):
            ts = tk.surface(surf.name)
            shader_name = ts.shaders[0] if ts and ts.shaders else surf.name
            mat = materials.get_material(shader_name, ctx.fs, library, extract_dir, load_textures, pack_images)
            meshes.append(build_surface(ctx, coll, arm_obj, path, model, surf, binding.positions[(mi, si)],
                                        binding.weights[(mi, si)], matrices, mat, ts.flags if ts else [],
                                        custom_normals))

    # animations
    if anim_mode == "NONE" or not available:
        chosen = []
    elif anim_mode == "REFERENCE":
        chosen = [ref_anim] if ref_anim else []
    elif anim_mode == "ALL":
        chosen = available
    else:
        pats = [p.strip() for p in anim_filter.replace(";", ",").split(",") if p.strip()] or ["*"]
        chosen = [a for a in available if any(fnmatch.fnmatch(a.alias.lower(), p.lower()) for p in pats)]
    # one action per animation file; every alias using the file is listed on it
    by_file = {}
    for a in chosen:
        by_file.setdefault(a.path.lower(), []).append(a)
    for a in available:  # aliases that share a chosen file come along
        if a.path.lower() in by_file and a not in by_file[a.path.lower()]:
            by_file[a.path.lower()].append(a)
    fps = context.scene.render.fps / context.scene.render.fps_base
    first_action = None
    for key, aliases in by_file.items():
        data = load_anim(aliases[0])
        if data is None or not data.num_frames:
            continue
        action = build_action(ctx, arm_obj, skeleton, rest, data, aliases[0].alias, axis, meshes, fps)
        action["mohaa_skc"] = aliases[0].path
        action["mohaa_aliases"] = json.dumps([[a.alias, a.options, a.block] for a in aliases])
        if first_action is None or (ref_anim and ref_anim in aliases):
            first_action = action
    if first_action is not None:
        animutil.assign(arm_obj, first_action)
        for obj in meshes:
            if obj.data.shape_keys and obj.data.shape_keys.animation_data:
                animutil.assign(obj.data.shape_keys, first_action)
        context.scene.frame_start = 1
        context.scene.frame_end = int(round(first_action.frame_end))
    else:
        # no action: leave the armature in its rest pose
        if arm_obj.animation_data:
            arm_obj.animation_data.action = None
    return arm_obj, ctx.warnings


def import_animations(context, arm_obj, filepaths, folders):
    """Add .skc files to an armature imported earlier (or any armature whose bone names
    match). Returns (actions, warnings)."""
    from .mohaa import skd as skdmod
    warnings = []
    axis = bone_axis_matrix(arm_obj)
    bones = []
    rest = {}
    for db in arm_obj.data.bones:
        if "mohaa_proxy_of" in db:
            continue
        t = db.get("mohaa_type", "ROTATION" if db.parent else "POSROT")
        btype = skdmod.BONE_TYPE_NAMES.index(t) if t in skdmod.BONE_TYPE_NAMES else skdmod.ROTATION
        b = skdmod.Bone(db.name, db.parent.name if db.parent else None, btype)
        m_rest = db.matrix_local @ axis.inverted()
        rest[db.name.lower()] = [list(r) for r in m_rest]
        if db.parent:
            p_rest = db.parent.matrix_local @ axis.inverted()
            local = p_rest.inverted_safe() @ m_rest
        else:
            local = m_rest
        b.offset = tuple(db["mohaa_offset"]) if "mohaa_offset" in db else tuple(local.translation)
        if "mohaa_data" in db:
            b.base_data = bytes.fromhex(db["mohaa_data"])
            b.offset = skdmod.decode_offset(btype, b.base_data)
            b.channels = [c for c in db.get("mohaa_channels", "").split("|") if c]
            b.refs = [c for c in db.get("mohaa_refs", "").split("|") if c]
        bones.append(b)
    skeleton = pose.Skeleton(bones)
    meshes = [o for o in bpy.data.objects if o.type == "MESH" and o.parent == arm_obj]
    fps = context.scene.render.fps / context.scene.render.fps_base
    actions = []
    for path in filepaths:
        with open(path, "rb") as f:
            data = skc.read(f.read())
        name = os.path.splitext(os.path.basename(path))[0]
        ctx = Context(path, folders)
        action = build_action(ctx, arm_obj, skeleton, rest, data, name, axis, meshes, fps)
        action["mohaa_skc"] = ctx.game_path
        action["mohaa_aliases"] = json.dumps([[name, [], ""]])
        actions.append(action)
        warnings += ctx.warnings
    if actions:
        animutil.assign(arm_obj, actions[-1])
    return actions, warnings
