"""Checks the add-on inside Blender against the engine's own maths.

    blender -b --factory-startup --python tools/blender/tests/test_blender.py -- \
        --folders "D:\\Medal of Honor\\main" --work <scratch folder> [--model models/...tik --anim run*]

For each model:
  1. import it with an animation, and compare every vertex Blender's armature puts on
     screen, frame by frame, with pose.py's port of skeletor (the importer's job);
  2. export it, read the written .skd/.skc back with the format library, and compare the
     skinned vertices of the original and the exported files at the same frames (the
     exporter's job: what the game would draw from the new files).
Exit status 1 when any distance passes the tolerance.
"""

import argparse
import os
import sys

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "io_scene_mohaa"))

import io_scene_mohaa  # noqa: E402
from io_scene_mohaa import exporter, importer  # noqa: E402
from mohaa import pose, skc, skd, tiki, vfs  # noqa: E402

DEFAULT_MODELS = [
    ("models/weapons/kar98.tik", "reload"),
    ("models/human/allied_airborne_soldier.tik", "scared_run_intro01"),
    ("models/human/german_wehrmact_soldier.tik", "walk_bored_back"),
]


def clear():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for a in list(bpy.data.actions):
        bpy.data.actions.remove(a)


def engine_positions(models, skeleton, channels):
    world = skeleton.evaluate(channels)
    out = []
    for _, m in models:
        names = [b.name for b in m.bones]
        for s in m.surfaces:
            out.append([pose.skin_vertex(v, names, world) for v in s.verts])
    return out


def blender_positions(arm, meshes_in_order):
    dg = bpy.context.evaluated_depsgraph_get()
    inv = arm.matrix_world.inverted()
    out = []
    for obj in meshes_in_order:
        ev = obj.evaluated_get(dg)
        me = ev.to_mesh()
        m = inv @ obj.matrix_world
        out.append([tuple(m @ v.co) for v in me.vertices])
        ev.to_mesh_clear()
    return out


def worst(a, b):
    d = 0.0
    for sa, sb in zip(a, b):
        for p, q in zip(sa, sb):
            d = max(d, sum((p[i] - q[i]) ** 2 for i in range(3)) ** 0.5)
    return d


def check_model(path, anim_pattern, folders, work, tolerance):
    clear()
    fs = vfs.GameFS(folders)
    local = os.path.join(work, "game", *path.split("/"))
    os.makedirs(os.path.dirname(local), exist_ok=True)
    with open(local, "wb") as f:
        f.write(fs.read(path))
    arm, warnings = importer.import_model(bpy.context, local, folders, anim_mode="FILTER", anim_filter=anim_pattern,
                                          load_textures=False)
    tk = tiki.parse(fs.read_text, path)
    models = [(p, skd.read(fs.read(p))) for p in tk.skelmodels if fs.exists(p)]
    skeleton = pose.Skeleton([b for _, m in models for b in m.bones])
    meshes = [o for o in arm.children if o.type == "MESH"]
    order = []
    for p, m in models:
        for s in m.surfaces:
            order.append(next(o for o in meshes if o.get("mohaa_skd") == p and o.get("mohaa_surface") == s.name
                              and o not in order))
    actions = [a for a in bpy.data.actions if a.get("mohaa_armature") == arm.name and a.get("mohaa_skc")]
    action = next((a for a in actions if a.name != arm.get("mohaa_reference")), actions[0])
    anim = skc.read(fs.read(action["mohaa_skc"]))
    from io_scene_mohaa import anim as animutil
    animutil.assign(arm, action)
    scene = bpy.context.scene
    fps = scene.render.fps / scene.render.fps_base
    frames = sorted({0, anim.num_frames // 3, anim.num_frames // 2, anim.num_frames - 1})
    result = {"model": path, "action": action.name, "frames": anim.num_frames}
    worst_import = 0.0
    for f in frames:
        t = 1.0 + f * anim.frame_time * fps
        scene.frame_set(int(t), subframe=t - int(t))
        a = engine_positions(models, skeleton, pose.frame_channels(anim, f))
        b = blender_positions(arm, order)
        worst_import = max(worst_import, worst(a, b))
    result["import_error"] = worst_import

    # round trip: export everything, read it back, skin both at the same frames
    out_dir = os.path.join(work, "export", "models")
    tik_out = os.path.join(out_dir, os.path.basename(path))
    written, ex_warn = exporter.export_model(bpy.context, tik_out, arm_obj=arm, anim_mode="ALL")
    root = os.path.join(work, "export")
    efs = vfs.GameFS()
    efs.add_loose_root(root)
    etk = tiki.parse(efs.read_text, vfs.game_path(tik_out, root))
    emodels = [(p, skd.read(efs.read(p))) for p in etk.skelmodels]
    eskel = pose.Skeleton([b for _, m in emodels for b in m.bones])
    alias = action.name
    ea = next(x for x in etk.anims if x.alias == alias)
    eanim = skc.read(efs.read(ea.path))
    worst_rt = 0.0
    for f in frames:
        a = engine_positions(models, skeleton, pose.frame_channels(anim, f))
        b = engine_positions(emodels, eskel, pose.frame_channels(eanim, f))
        # surfaces can come back in another order: match them by file, name and size
        eflat = []
        for p, m in emodels:
            for s in m.surfaces:
                eflat.append((os.path.basename(p).lower(), s.name, len(s.verts)))
        oflat = []
        for p, m in models:
            for s in m.surfaces:
                oflat.append((os.path.basename(p).lower(), s.name, len(s.verts)))
        used = set()
        for i, key in enumerate(oflat):
            j = next((j for j, k in enumerate(eflat) if k == key and j not in used), None)
            if j is None:
                result.setdefault("unmatched", []).append(key)
                continue
            used.add(j)
            worst_rt = max(worst_rt, worst([a[i]], [b[j]]))
    result["roundtrip_error"] = worst_rt
    result["export_frames"] = eanim.num_frames
    result["written"] = len(written)
    result["warnings"] = warnings + ex_warn
    result["ok"] = worst_import < tolerance and worst_rt < tolerance and eanim.num_frames == anim.num_frames
    return result


def check_made_in_blender(work, tolerance):
    """A textured static prop and a rigged, animated mesh built here, exported, and read
    back: the engine-side skinning of the files must match what Blender shows."""
    results = []
    from mathutils import Matrix
    k = 1.0 / (0.52 * importer.UNIT_METERS)  # Blender metres -> raw model units

    # 1. static prop: a 1 m cube with an image texture and no armature
    clear()
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0, 0, 0.5))
    cube = bpy.context.active_object
    img = bpy.data.images.new("crate_test", 16, 16)
    img.pixels = [0.8, 0.5, 0.2, 1.0] * 256
    mat = bpy.data.materials.new("crate")
    mat.use_nodes = True
    tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
    tex.image = img
    bsdf = next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    cube.data.materials.append(mat)
    out = os.path.join(work, "made", "models", "props", "crate.tik")
    written, warns = exporter.export_model(bpy.context, out, meshes=[cube])
    m = skd.read(open(os.path.join(work, "made", "models", "props", "crate", "crate.skd"), "rb").read())
    xs = [v.weights[0][2] for s in m.surfaces for v in s.verts]
    size = max(p[0] for p in xs) - min(p[0] for p in xs)
    tik = open(out).read()
    ok = (len(m.bones) == 1 and abs(size - k) < 0.01 and "crate_test.tga" in tik
          and os.path.exists(os.path.join(work, "made", "models", "props", "crate", "crate_test.tga"))
          and len(m.surfaces[0].verts) == 24 and len(m.surfaces[0].triangles) == 12)
    results.append(("static prop", ok, "cube width %.2f units (want %.2f), %d verts, %d files"
                    % (size, k, len(m.surfaces[0].verts), len(written))))

    # 2. rigged: a two-bone armature, a skinned cylinder, a bend animation
    clear()
    bpy.ops.object.armature_add(location=(0, 0, 0))
    arm = bpy.context.active_object
    bpy.ops.object.mode_set(mode="EDIT")
    b1 = arm.data.edit_bones[0]
    b1.name = "root"
    b1.head, b1.tail = (0, 0, 0), (0, 0, 1)
    b2 = arm.data.edit_bones.new("tip")
    b2.head, b2.tail = (0, 0, 1), (0, 0, 2)
    b2.parent = b1
    bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.mesh.primitive_cylinder_add(radius=0.2, depth=2.0, location=(0, 0, 1), vertices=12)
    cyl = bpy.context.active_object
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.subdivide(number_cuts=6)
    bpy.ops.object.mode_set(mode="OBJECT")
    cyl.parent = arm
    mod = cyl.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    g1 = cyl.vertex_groups.new(name="root")
    g2 = cyl.vertex_groups.new(name="tip")
    for v in cyl.data.vertices:
        t = min(max((v.co.z + 1.0) / 2.0, 0.0), 1.0)  # 0 at the bottom, 1 at the top
        g1.add([v.index], 1.0 - t, "REPLACE")
        g2.add([v.index], t, "REPLACE")
    action = bpy.data.actions.new("bend")
    from io_scene_mohaa import anim as animutil
    animutil.assign(arm, action)
    pb = arm.pose.bones["tip"]
    pb.rotation_mode = "QUATERNION"
    import math
    from mathutils import Quaternion
    for frame, angle in ((1, 0.0), (11, 60.0), (21, 0.0)):
        pb.rotation_quaternion = Quaternion((1, 0, 0), math.radians(angle))
        pb.keyframe_insert("rotation_quaternion", frame=frame)
    out = os.path.join(work, "made", "models", "props", "bender.tik")
    written, warns = exporter.export_model(bpy.context, out, arm_obj=arm)
    root = os.path.join(work, "made")
    efs = vfs.GameFS()
    efs.add_loose_root(root)
    etk = tiki.parse(efs.read_text, "models/props/bender.tik")
    em = skd.read(efs.read(etk.skelmodels[0]))
    ea = skc.read(efs.read(etk.anims[0].path))
    eskel = pose.Skeleton(em.bones)
    scene = bpy.context.scene
    worst_d = 0.0
    fps = scene.render.fps / scene.render.fps_base
    for f in range(ea.num_frames):
        t = 1.0 + f * ea.frame_time * fps
        scene.frame_set(int(t), subframe=t - int(t))
        world = eskel.evaluate(pose.frame_channels(ea, f))
        names = [b.name for b in em.bones]
        engine = sorted(tuple(round(c, 1) for c in pose.skin_vertex(v, names, world)) for v in em.surfaces[0].verts)
        dg = bpy.context.evaluated_depsgraph_get()
        ev = cyl.evaluated_get(dg)
        me = ev.to_mesh()
        blender = [tuple(c * k for c in (cyl.matrix_world @ v.co)) for v in me.vertices]
        ev.to_mesh_clear()
        # compare as point sets (the exporter splits vertices at UV seams)
        for p in engine:
            worst_d_p = min(sum((p[i] - q[i]) ** 2 for i in range(3)) for q in blender) ** 0.5
            worst_d = max(worst_d, worst_d_p)
    ok = worst_d < max(tolerance, 0.1) and ea.num_frames == 21
    results.append(("rigged + animated", ok, "worst vertex distance %.4f units over %d frames, bones %s"
                    % (worst_d, ea.num_frames, [skd.BONE_TYPE_NAMES[b.type] for b in em.bones])))
    return results


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--folders", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--model", action="append")
    ap.add_argument("--anim", action="append")
    ap.add_argument("--tolerance", type=float, default=0.05, help="game units")
    args = ap.parse_args(argv)
    io_scene_mohaa.register()
    folders = [f for f in args.folders.split(";") if f]
    models = list(zip(args.model, args.anim or ["*"] * len(args.model))) if args.model else DEFAULT_MODELS
    failed = 0
    for path, pattern in models:
        r = check_model(path, pattern, folders, args.work, args.tolerance)
        print("TEST %s %s: import error %.5f, round trip error %.5f, frames %d -> %d, %d files%s" % (
            "ok  " if r["ok"] else "FAIL", r["model"], r["import_error"], r["roundtrip_error"], r["frames"],
            r["export_frames"], r["written"], (", unmatched %s" % r["unmatched"]) if r.get("unmatched") else ""))
        for w in r["warnings"]:
            print("     warning:", w)
        failed += not r["ok"]
    for name, ok, detail in check_made_in_blender(args.work, args.tolerance):
        print("TEST %s made in Blender, %s: %s" % ("ok  " if ok else "FAIL", name, detail))
        failed += not ok
    print("TESTS %s" % ("FAILED" if failed else "PASSED"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
