"""Runs inside Blender for tools/lamps/remodel.py: one lamp model made again.

    blender -b --factory-startup --python remodel_blender.py -- IN.tik OUT_DIR SPEC.json --folders F1;F2

A lamp model is one surface with one texture: the frame, the glass or the
shade, and no bulb in it. Made again here, from the game's own model:

  - its glass (the bright part of its texture, in pieces big enough to be
    a shade) becomes a surface of its own, opm_<lamp>_glass, which the
    lamps' shaders light from inside (data/opm-lamps/scripts/opm_lampglass.shader);
    rounded by subdivision when it has room under the engine's limits;
  - a bulb (or a flame) at each bulb lamps.txt gives, a surface opm_bulb
    (opm_flame) that glows;
  - the rest is the frame, as it was.

A surface the TIKI names no shader for takes the shader of its own name, so
the game's .tik is left as it is: only the .skd is written (OUT_DIR, the
mohaa_result line says where).
"""

import json
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "blender"))

from io_scene_mohaa import exporter, importer, materials, remaster  # noqa: E402
from io_scene_mohaa.importer import UNIT_METERS  # noqa: E402

MAX_VERTS = 1000  # TIKI_MAX_VERTEXES, a surface


def clear_scene():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for coll in list(bpy.data.collections):
        bpy.data.collections.remove(coll)


def image_array(mat):
    img = materials.material_image(mat) if mat else None
    if img is None or not img.size[0]:
        return None
    px = np.empty(img.size[0] * img.size[1] * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    return px.reshape(img.size[1], img.size[0], 4)  # rows bottom up, as Blender keeps them


def face_luma(me, image):
    """Each face's brightness in its texture, and whether it is drawn at all (alpha)."""
    uv = me.uv_layers.active.data
    h, w = image.shape[:2]
    luma = np.zeros(len(me.polygons))
    for poly in me.polygons:
        corners = [uv[li].uv for li in poly.loop_indices]
        mid = Vector((sum(c.x for c in corners), sum(c.y for c in corners))) / len(corners)
        samples = [mid] + [mid.lerp(c, 0.6) for c in corners]
        acc = np.zeros(4)
        for s in samples:
            x = int((s.x % 1.0) * w) % w
            y = int((s.y % 1.0) * h) % h
            acc += image[y, x]
        c = acc / len(samples)
        luma[poly.index] = (0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]) if c[3] >= 0.5 else 0.0
    return luma


def glass_faces(me, luma, threshold=None, bulbs=()):
    """The faces of the glass: the pieces of the mesh (its texture's islands)
    mostly of the bright part of the texture. A shade is a piece of its own, its rim darker than its middle; the
    frame's pieces are dark, a bright highlight here and there. A piece round a
    bulb (its box holds one) is a shade too, however its texture is smeared,
    unless it is the dark metal of a socket."""
    area = np.array([p.area for p in me.polygons])
    if not (area > 0).any():
        return set()
    if threshold is None:
        threshold = max(0.5, 0.8 * float(np.percentile(luma[area > 0], 95)))

    # pieces: the texture's islands (faces joined where their texture runs
    # on across the edge). Joined by where their corners are, the shades on
    # a chandelier's arms were one piece with the arms they touch.
    from bpy_extras import mesh_utils
    pieces = {i: faces for i, faces in enumerate(mesh_utils.mesh_linked_uv_islands(me))}

    glass = set()
    for faces in pieces.values():
        total = sum(area[i] for i in faces)
        if total <= 0:
            continue
        bright = sum(area[i] for i in faces if luma[i] >= threshold)
        mean = sum(area[i] * luma[i] for i in faces) / total
        co = np.array([me.vertices[vi].co[:] for i in faces for vi in me.polygons[i].vertices])
        lo, hi = co.min(0), co.max(0)
        pad = 0.02 * float(np.linalg.norm(hi - lo))
        round_bulb = any(np.all(b >= lo - pad) and np.all(b <= hi + pad) for b in bulbs)
        if bright / total > 0.5 or (round_bulb and mean > 0.3):
            glass.update(faces)
    return glass


def split_object(obj, faces, name):
    """obj's faces in faces moved to a new object called name (kept in place, skinned the same)."""
    new = obj.copy()
    new.data = obj.data.copy()
    new.name = name
    for coll in obj.users_collection:
        coll.objects.link(new)
    for keep, o in ((faces, new), (None, obj)):
        bm = bmesh.new()
        bm.from_mesh(o.data)
        bm.faces.ensure_lookup_table()
        if keep is not None:
            drop = [f for f in bm.faces if f.index not in faces]
        else:
            drop = [f for f in bm.faces if f.index in faces]
        bmesh.ops.delete(bm, geom=drop, context="FACES")
        bm.to_mesh(o.data)
        bm.free()
        # its LOD data names the vertexes of the whole surface: cut in two, it
        # would pull these to others' places as the view moves away
        for a in [a.name for a in o.data.attributes if a.name.startswith("mohaa_collapse")]:
            o.data.attributes.remove(o.data.attributes[a])
    return new


def named_material(base, shader):
    """A copy of base the exporter writes as the shader called shader."""
    mat = base.copy() if base else bpy.data.materials.new(shader)
    mat.name = shader
    mat["mohaa_shader"] = shader
    img = materials.material_image(mat)
    if img is not None and "mohaa_image" not in mat:
        mat["mohaa_image"] = img.get("mohaa_path", "")
    return mat


def add_bulb(arm, frame_obj, at_world, radius, shape, shader):
    """A bulb (an egg, its narrow end up) or a flame (a teardrop) at at_world, skinned like the frame."""
    bm = bmesh.new()
    if shape == "flame":
        bmesh.ops.create_uvsphere(bm, u_segments=8, v_segments=6, radius=1.0)
        for v in bm.verts:
            z = v.co.z
            # a teardrop: narrow and drawn up above, round below
            k = 1.0 - 0.75 * max(0.0, z)
            v.co.x *= radius * k
            v.co.y *= radius * k
            v.co.z = z * radius * (2.6 if z > 0 else 1.2)
    else:
        bmesh.ops.create_uvsphere(bm, u_segments=10, v_segments=7, radius=1.0)
        for v in bm.verts:
            z = v.co.z
            k = 1.0 - 0.3 * max(0.0, z)
            v.co.x *= radius * k
            v.co.y *= radius * k
            v.co.z = z * radius * 1.3
    me = bpy.data.meshes.new(shader)
    bm.to_mesh(me)
    bm.free()
    uv = me.uv_layers.new(name="UVMap")
    for loop in uv.data:
        loop.uv = (0.5, 0.5)
    for p in me.polygons:
        p.use_smooth = True
    obj = bpy.data.objects.new(shader, me)
    for coll in frame_obj.users_collection:
        coll.objects.link(obj)
    obj.parent = arm
    obj.matrix_world = Matrix.Translation(at_world)
    # the frame's bone, all of it
    for g in frame_obj.vertex_groups:
        obj.vertex_groups.new(name=g.name)
    weights = {}
    for v in frame_obj.data.vertices:
        for ge in v.groups:
            weights[ge.group] = weights.get(ge.group, 0.0) + ge.weight
    if weights:
        main = max(weights, key=weights.get)
        obj.vertex_groups[frame_obj.vertex_groups[main].name].add(list(range(len(me.vertices))), 1.0, "REPLACE")
    mod = obj.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    for key in ("mohaa_skd", "mohaa_flags"):
        if key in frame_obj:
            obj[key] = frame_obj[key]
    obj["mohaa_surface"] = shader
    # one material for all of them, so that joined they are one surface
    mat = bpy.data.materials.get(shader) or named_material(None, shader)
    me.materials.append(mat)
    return obj


def rewrap(obj):
    """The glass's UVs wrapped round each of its shades: u round it, v from its
    neck (0) to its rim (1), for its own texture (tools/lamps/glass.py). A
    shade is a piece of the mesh, its axis up through the middle of its box
    and its neck at the top."""
    me = obj.data
    uv = me.uv_layers.active.data
    co = np.array([obj.matrix_world @ v.co for v in me.vertices])
    parent = list(range(len(me.vertices)))

    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    at = {}
    for i, c in enumerate(co):
        key = tuple(np.round(c, 5))
        if key in at:
            parent[root(i)] = root(at[key])
        else:
            at[key] = i
    for e in me.edges:
        a, b = root(e.vertices[0]), root(e.vertices[1])
        if a != b:
            parent[a] = b
    pieces = {}
    for i in range(len(co)):
        pieces.setdefault(root(i), []).append(i)
    u_of, v_of = {}, {}
    for members in pieces.values():
        p = co[members]
        lo, hi = p.min(0), p.max(0)
        mid = (lo + hi) / 2
        for i in members:
            d = co[i] - mid
            u_of[i] = (math.atan2(d[1], d[0]) / (2 * math.pi)) % 1.0
            v_of[i] = (hi[2] - co[i][2]) / max(hi[2] - lo[2], 1e-6)
    for poly in me.polygons:
        us = [u_of[me.loops[li].vertex_index] for li in poly.loop_indices]
        # a face across the seam takes its u from one side of it
        wrap = max(us) - min(us) > 0.5
        for li in poly.loop_indices:
            vi = me.loops[li].vertex_index
            u = u_of[vi]
            if wrap and u < 0.5:
                u += 1.0
            uv[li].uv = (u, 1.0 - v_of[vi])


def lamp_name(tik_path):
    return os.path.splitext(os.path.basename(tik_path))[0].lower().replace("-", "_")


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    tik_path, out_dir, spec_path = argv[0], argv[1], argv[2]
    folders = argv[argv.index("--folders") + 1].split(";") if "--folders" in argv else []
    spec = json.load(open(spec_path))
    warnings, report = [], {}

    clear_scene()
    arm, w = importer.import_model(bpy.context, tik_path, [f for f in folders if f], anim_mode="REFERENCE",
                                   unit="METERS")
    warnings += w
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    name = lamp_name(spec["model"])
    glass_shader = "opm_%s_glass" % name

    # the glass, out of whichever surface has it
    glass_objs = []
    for obj in meshes:
        mat = obj.data.materials[0] if obj.data.materials else None
        image = image_array(mat)
        if image is None or spec.get("kind") == "off":
            continue
        # the bulbs where the mesh is, in its object's own space
        to_local = obj.matrix_world.inverted()
        unit = arm.get("mohaa_unit", UNIT_METERS)
        bulbs = [np.array((to_local @ (Vector(b) * unit))[:]) for b in spec.get("bulb", [])]
        faces = glass_faces(obj.data, face_luma(obj.data, image), spec.get("glassThreshold"), bulbs)
        report.setdefault("surfaces", {})[obj.get("mohaa_surface", obj.name)] = {
            "faces": len(obj.data.polygons), "glass": len(faces)}
        if not faces:
            continue
        if len(faces) == len(obj.data.polygons):
            # all of it is glass (a bare bulb): the surface itself becomes the glass
            glass = obj
        else:
            glass = split_object(obj, faces, glass_shader)
        glass["mohaa_surface"] = glass_shader
        glass.data.materials.clear()
        glass.data.materials.append(named_material(mat, glass_shader))
        glass_objs.append(glass)

    # rounded, where there is room for it
    rounded = []
    for glass in list(glass_objs):
        if spec.get("round", True) and len(glass.data.vertices) * 4 < MAX_VERTS * 0.9:
            new = remaster.smoothed_copy(arm, {"obj": glass}, glass.name + "_r", levels=1)
            if new is not None:
                new["mohaa_surface"] = glass_shader
                new.data.materials.clear()
                for m in glass.data.materials:
                    new.data.materials.append(m)
                bpy.data.objects.remove(glass, do_unlink=True)
                new.name = glass_shader
                rounded.append(new.name)
    report["rounded"] = rounded

    if spec.get("rewrap"):
        for o in bpy.data.objects:
            if o.type == "MESH" and o.get("mohaa_surface") == glass_shader:
                rewrap(o)

    # a bulb at each of its bulbs: lamps.txt gives them in the model's frame,
    # its TIKI scale in them; Blender's world is that times its unit
    frame = next((o for o in bpy.data.objects if o.type == "MESH" and not o.get("mohaa_surface", "").startswith("opm_")), None)
    shape = "flame" if spec.get("kind") in ("candle", "lantern") else "bulb"
    if spec.get("kind") not in ("bulb", "off") and frame is not None:
        unit = arm.get("mohaa_unit", UNIT_METERS)
        radius = float(spec.get("bulbSize", 0.45 if shape == "flame" else 1.4)) * unit
        objs = [add_bulb(arm, frame, Vector(b) * unit, radius, shape, "opm_flame" if shape == "flame" else "opm_bulb")
                for b in spec.get("bulb", [])]
        if len(objs) > 1:
            # one surface for them all
            bpy.ops.object.select_all(action="DESELECT")
            for o in objs:
                o.select_set(True)
            bpy.context.view_layer.objects.active = objs[0]
            bpy.ops.object.join()
        report["bulbs"] = len(spec.get("bulb", []))

    # the .skd alone: the game's .tik names the frame's shader, the new
    # surfaces take theirs by name
    os.makedirs(out_dir, exist_ok=True)
    out_tik = os.path.join(out_dir, name + ".tik")
    result = exporter.export_model(bpy.context, out_tik, arm_obj=arm, write_textures=False)
    written = result[0] if isinstance(result, tuple) else result
    warnings += result[1] if isinstance(result, tuple) and len(result) > 1 else []
    report["counts"] = {o.get("mohaa_surface", o.name): len(o.data.vertices) for o in bpy.data.objects if o.type == "MESH"}
    print("MOHAA_RESULT " + json.dumps({"written": written if isinstance(written, list) else [str(written)],
                                         "warnings": [str(x) for x in warnings], "report": report}))


main()
