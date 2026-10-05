"""Checks the remaster pipeline end to end, without game data or TRELLIS.

    blender -b --factory-startup --python tools/blender/tests/test_remaster.py -- --work <scratch folder>
    (or the bpy module: python tools/blender/tests/test_remaster.py --work <scratch folder>)

A small character is made and exported as a game model: a torso, two arms hanging at its
sides, a head with a facial morph, textures, and a wave animation. Then:
  1. prepare imports it, lifts its arms and renders it;
  2. a stand-in for TRELLIS: the lifted model fused into one blob (voxel remesh, no rig, no
     UVs) with bumps for "new detail", scaled, turned and moved like a generator's output;
     and a bad candidate, a sphere;
  3. build must pick the blob, cut it into the three surfaces with their UVs, weights and
     morph, and export .skd files the engine's maths skins like Blender shows them;
  4. with only the sphere, every surface must fall back: smoothed, or kept when it has
     morphs.
"""

import argparse
import json
import math
import os
import shutil
import sys

import bpy
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "io_scene_mohaa"))

import io_scene_mohaa  # noqa: E402
from io_scene_mohaa import anim as animutil  # noqa: E402
from io_scene_mohaa import exporter, remaster  # noqa: E402
from mohaa import pose, skc, skd, tiki, vfs  # noqa: E402


_registered = False


def clear():
    global _registered
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if not _registered:
        io_scene_mohaa.register()
        _registered = True


def textured(name, rgb):
    img = bpy.data.images.new(name, 32, 32)
    px = np.zeros((32, 32, 4), dtype=np.float32)
    px[..., :3] = rgb
    px[::4, :, :3] *= 0.5  # stripes, so a texture shift would show
    px[..., 3] = 1.0
    img.pixels.foreach_set(px.ravel())
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
    tex.image = img
    bsdf = next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    mat.use_backface_culling = True
    return mat


def mesh_obj(name, verts, faces, mat, arm, weights, uv_mode="cylinder"):
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)
    uvl = me.uv_layers.new(name="UVMap")
    for poly in me.polygons:
        for li in poly.loop_indices:
            x, y, z = me.vertices[me.loops[li].vertex_index].co
            uvl.data[li].uv = ((math.atan2(y, x) / (2 * math.pi)) % 1.0, z / 2.0 % 1.0)
    for p in me.polygons:
        p.use_smooth = True
    me.materials.append(mat)
    obj = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(obj)
    obj.parent = arm
    for vi, ws in enumerate(weights):
        for g, w in ws.items():
            vg = obj.vertex_groups.get(g) or obj.vertex_groups.new(name=g)
            vg.add([vi], w, "REPLACE")
    obj.modifiers.new("Armature", "ARMATURE").object = arm
    return obj


def cylinder(center, radius, z0, z1, rings, seg=12):
    verts, faces = [], []
    for r in range(rings + 1):
        z = z0 + (z1 - z0) * r / rings
        for s in range(seg):
            a = 2 * math.pi * s / seg
            verts.append((center[0] + radius * math.cos(a), center[1] + radius * math.sin(a), z))
    for r in range(rings):
        for s in range(seg):
            a, b = r * seg + s, r * seg + (s + 1) % seg
            faces.append((a, b, b + seg, a + seg))
    bot, top = len(verts), len(verts) + 1
    verts += [(center[0], center[1], z0), (center[0], center[1], z1)]
    for s in range(seg):
        faces.append((bot, (s + 1) % seg, s))
        faces.append((top, rings * seg + s, rings * seg + (s + 1) % seg))
    return verts, faces


def make_character(work):
    """A rigged, textured, animated character exported to work/game/models/test/dude.tik."""
    clear()
    arm_data = bpy.data.armatures.new("dude")
    arm = bpy.data.objects.new("dude", arm_data)
    bpy.context.scene.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    eb = arm_data.edit_bones
    root = eb.new("Bip01")
    root.head, root.tail = (0, 0, 0.9), (0, 0, 1.0)
    spine = eb.new("Bip01 Spine")
    spine.head, spine.tail, spine.parent = (0, 0, 1.0), (0, 0, 1.5), root
    for side, y in (("L", 0.28), ("R", -0.28)):
        ua = eb.new("Bip01 %s UpperArm" % side)
        ua.head, ua.tail, ua.parent = (0, y, 1.45), (0, y, 1.15), spine
        fa = eb.new("Bip01 %s Forearm" % side)
        fa.head, fa.tail, fa.parent = (0, y, 1.15), (0, y, 0.85), ua
    bpy.ops.object.mode_set(mode="OBJECT")

    v, f = cylinder((0, 0), 0.18, 0.9, 1.5, 8)
    w = [{"Bip01 Spine": min(max((z - 0.9) / 0.3, 0.0), 1.0), "Bip01": 1.0 - min(max((z - 0.9) / 0.3, 0.0), 1.0)}
         for _, _, z in v]
    mesh_obj("body", v, f, textured("cloth", (0.3, 0.4, 0.2)), arm, w)
    av, af, aw = [], [], []
    for side, y in (("L", 0.28), ("R", -0.28)):
        v, f = cylinder((0, y), 0.06, 0.85, 1.45, 10, seg=8)
        base = len(av)
        av += v
        af += [tuple(i + base for i in face) for face in f]
        for _, _, z in v:
            t = min(max((1.15 - z) / 0.1 + 0.5, 0.0), 1.0)  # 0 above the elbow, 1 below
            aw.append({"Bip01 %s UpperArm" % side: 1.0 - t, "Bip01 %s Forearm" % side: t})
    mesh_obj("arms", av, af, textured("sleeve", (0.35, 0.45, 0.25)), arm, aw)
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.12, location=(0, 0, 1.66), segments=16, ring_count=8)
    tmp = bpy.context.active_object
    hv = [tuple(tmp.matrix_world @ x.co) for x in tmp.data.vertices]
    hf = [tuple(p.vertices) for p in tmp.data.polygons]
    bpy.data.objects.remove(tmp, do_unlink=True)
    head = mesh_obj("head", hv, hf, textured("face", (0.8, 0.6, 0.5)), arm, [{"Bip01 Spine": 1.0}] * len(hv))
    head.shape_key_add(name="Basis", from_mix=False)
    kb = head.shape_key_add(name="smile", from_mix=False)
    for i, x in enumerate(hv):
        if x[0] > 0.05 and x[2] < 1.66:  # the front lower half moves forward
            kb.data[i].co = (x[0] + 0.03, x[1], x[2])
    kb.value = 0.0

    action = bpy.data.actions.new("wave")
    animutil.assign(arm, action)
    for name, axis in (("Bip01 L UpperArm", 1), ("Bip01 R UpperArm", -1)):
        pb = arm.pose.bones[name]
        pb.rotation_mode = "QUATERNION"
        from mathutils import Quaternion
        for frame, angle in ((1, 0.0), (11, 70.0), (21, 0.0)):
            pb.rotation_quaternion = Quaternion((0, 0, 1), math.radians(angle * axis * 0.3)) @ \
                Quaternion((1, 0, 0), math.radians(-angle * axis))
            pb.keyframe_insert("rotation_quaternion", frame=frame)
    pb = arm.pose.bones["Bip01 Spine"]
    pb.rotation_mode = "QUATERNION"
    for frame, angle in ((1, 0.0), (11, 25.0), (21, 0.0)):
        pb.rotation_quaternion = Quaternion((0, 1, 0), math.radians(angle))
        pb.keyframe_insert("rotation_quaternion", frame=frame)
    out = os.path.join(work, "game", "models", "test", "dude.tik")
    exporter.export_model(bpy.context, out, arm_obj=arm)
    return out


def fake_trellis(work, wd):
    """Stand-ins for TRELLIS: the lifted model fused into one rough, unrigged blob in a
    generator's frame (unit cube, turned, Z up), and a sphere."""
    bpy.ops.wm.open_mainfile(filepath=os.path.join(wd, "source.blend"))
    arm = next(o for o in bpy.context.scene.objects if o.type == "ARMATURE")
    vs, ts, base = [], [], 0
    for o in remaster.model_meshes(arm):
        v, t, _ = remaster.posed_geometry(arm, o)
        vs.append(v)
        ts.append(t + base)
        base += len(v)
    v, t = np.concatenate(vs), np.concatenate(ts)
    me = bpy.data.meshes.new("blob")
    me.from_pydata(v.tolist(), [], t.tolist())
    blob = bpy.data.objects.new("blob", me)
    bpy.context.scene.collection.objects.link(blob)
    rm = blob.modifiers.new("Remesh", "REMESH")
    rm.mode = "VOXEL"
    lo, hi, diag = remaster.bounds(v)
    rm.voxel_size = diag / 90.0
    dg = bpy.context.evaluated_depsgraph_get()
    fused = bpy.data.meshes.new_from_object(blob.evaluated_get(dg), depsgraph=dg)
    fused.calc_loop_triangles()
    fv = np.array([x.co[:] for x in fused.vertices])
    ft = np.array([tri.vertices[:] for tri in fused.loop_triangles])
    # new detail: small bumps along the normals
    fn = np.array([x.normal[:] for x in fused.vertices])
    bump = np.sin(fv[:, 2] * 0.6) * np.sin(fv[:, 0] * 0.6 + fv[:, 1] * 0.6)
    fv = fv + fn * bump[:, None] * diag * 0.004
    # a generator's frame: centred in a unit cube, turned a quarter about Z
    c = (fv.min(0) + fv.max(0)) / 2
    fv = (fv - c) / (fv.max(0) - fv.min(0)).max()
    rot = np.array([[0, -1, 0], [1, 0, 0], [0, 0, 1]], dtype=float)
    fv = fv @ rot.T + np.array([0.01, -0.02, 0.03])
    cdir = os.path.join(wd, "candidates")
    os.makedirs(cdir, exist_ok=True)
    good = os.path.join(cdir, "trellis_s1.obj")
    write_obj(good, fv, ft)
    sphere = bpy.data.meshes.new("s")
    import bmesh
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=24, v_segments=12, radius=0.5)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(sphere)
    sv = np.array([x.co[:] for x in sphere.vertices])
    st = np.array([p.vertices[:] for p in sphere.polygons])
    bad = os.path.join(work, "sphere.obj")
    write_obj(bad, sv, st)
    return good, bad, len(ft)


def write_obj(path, v, t):
    with open(path, "w") as f:
        for p in v:
            f.write("v %.6f %.6f %.6f\n" % tuple(p))
        for tri in t:
            f.write("f %d %d %d\n" % tuple(i + 1 for i in tri))


def engine_vs_blender(out_root, wd, tik_game_path, source_root):
    """The exported .skd files skinned with the engine's maths at frames of the wave, against
    the remastered objects Blender shows (as point sets per surface name)."""
    bpy.ops.wm.open_mainfile(filepath=os.path.join(wd, "remastered.blend"))
    arm = next(o for o in bpy.context.scene.objects if o.type == "ARMATURE")
    fs = vfs.GameFS()
    fs.add_loose_root(source_root)
    fs.add_loose_root(out_root)  # the remastered files win, as the pk3 would
    tk = tiki.parse(fs.read_text, tik_game_path)
    models = [skd.read(fs.read(p)) for p in tk.skelmodels]
    skel = pose.Skeleton([b for m in models for b in m.bones])
    anim = skc.read(fs.read(next(a.path for a in tk.anims if a.alias == "wave")))
    action = next(a for a in bpy.data.actions if a.get("mohaa_skc", "").endswith("wave.skc"))
    animutil.assign(arm, action)
    scene = bpy.context.scene
    fps = scene.render.fps / scene.render.fps_base
    shown = {}
    worst = 0.0
    for f in (0, anim.num_frames // 2, anim.num_frames - 1):
        t = 1.0 + f * anim.frame_time * fps
        scene.frame_set(int(t), subframe=t - int(t))
        world = skel.evaluate(pose.frame_channels(anim, f))
        for o in remaster.model_meshes(arm):
            if o.hide_get():
                continue
            shown.setdefault(o["mohaa_surface"], []).append(remaster.posed_geometry(arm, o)[0])
        for m in models:
            names = [b.name for b in m.bones]
            for s in m.surfaces:
                eng = np.array([pose.skin_vertex(v, names, world) for v in s.verts])
                bl = shown[s.name][-1]
                d, _ = remaster._nearest(eng, bl)
                worst = max(worst, float(d.max()))
    return worst, sum(len(m.surfaces) for m in models), models


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    args = ap.parse_args(argv)
    work = os.path.abspath(args.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    failed = 0

    def check(name, ok, detail):
        nonlocal failed
        print("TEST %s %s: %s" % ("ok  " if ok else "FAIL", name, detail))
        failed += not ok

    tik = make_character(work)
    source_root = os.path.join(work, "game")

    # 1. prepare
    wd = os.path.join(work, "remaster", "dude")
    clear()
    r = remaster.prepare(bpy.context, tik, [], wd, size=256)
    check("prepare", len(r["lifted"]) == 2 and len(r["views"]) == 4 and os.path.exists(r["blend"])
          and "wave" in r["check_anims"],
          "lifted %s, %d views, animations %s" % (r["lifted"], len(r["views"]), r["check_anims"]))

    # 2. candidates
    good, bad, faces = fake_trellis(work, wd)

    # 3. build with both
    out_root = os.path.join(work, "out")
    opts = remaster.Options(factor=6.0)
    r = remaster.build_from_blend(bpy.context, os.path.join(wd, "source.blend"), [bad, good], out_root, wd, opts)
    results = {k.split(" ")[0]: e for k, e in r["surfaces"].items()}
    print(json.dumps(results, indent=1))
    check("build picks the good candidate", r.get("chosen") == good, os.path.basename(r.get("chosen", "-")))
    check("every surface gets the new detail", all(e["result"] == "detail" for e in results.values()),
          ", ".join("%s %s%s" % (k, e["result"], " (%s)" % e["reason"] if "reason" in e else "")
                    for k, e in results.items()))
    worst, count, models = engine_vs_blender(out_root, wd, "models/test/dude.tik", source_root)
    check("exported files skin as Blender shows", worst < 0.05, "worst %.4f units, %d surfaces" % (worst, count))
    m = models[0]
    head = [s for s in m.surfaces if s.name == "head"]
    check("the head keeps its morph", m.morph_names == ["smile"] and any(v.morphs for s in head for v in s.verts),
          "morphs %s" % m.morph_names)
    tris = sum(len(s.triangles) for s in m.surfaces)
    check("budget", count <= remaster.MAX_SURFACES and tris > 2 * sum(e["original_tris"] for e in results.values()),
          "%d triangles (originally %d), %d surfaces" % (tris, sum(e["original_tris"] for e in results.values()), count))

    # textures land where they did: the front renders before and after look alike
    import bpy as _b
    bef = _b.data.images.load(os.path.join(wd, "review", "before_00.png"))
    aft = _b.data.images.load(os.path.join(wd, "review", "after_00.png"))
    a = np.empty(len(bef.pixels), dtype=np.float32)
    b = np.empty(len(aft.pixels), dtype=np.float32)
    bef.pixels.foreach_get(a)
    aft.pixels.foreach_get(b)
    a, b = a.reshape(-1, 4), b.reshape(-1, 4)
    both = (a[:, 3] > 0.5) & (b[:, 3] > 0.5)
    diff = float(np.abs(a[both, :3] - b[both, :3]).mean())
    check("textures map as before", diff < 0.06, "mean colour difference %.3f over %d pixels" % (diff, both.sum()))

    # 4. fallback
    shutil.rmtree(out_root)
    wd2 = os.path.join(work, "remaster", "dude_fallback")
    shutil.copytree(wd, wd2, ignore=shutil.ignore_patterns("candidates", "review", "report.json", "remastered.blend"))
    r = remaster.build_from_blend(bpy.context, os.path.join(wd2, "source.blend"), [bad], out_root, wd2, opts)
    results = {k.split(" ")[0]: e for k, e in r["surfaces"].items()}
    check("a bad candidate falls back", results["body"]["result"] == "smoothed" and results["arms"]["result"] ==
          "smoothed" and results["head"]["result"] == "original",
          ", ".join("%s %s" % (k, e["result"]) for k, e in results.items()))
    worst, count, models = engine_vs_blender(out_root, wd2, "models/test/dude.tik", source_root)
    check("smoothed files skin as Blender shows", worst < 0.05, "worst %.4f units, %d surfaces" % (worst, count))

    print("TESTS %s" % ("FAILED" if failed else "PASSED"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
