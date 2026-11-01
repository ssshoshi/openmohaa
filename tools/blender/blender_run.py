"""Runs inside Blender for mohaa_blender.py: blender -b --factory-startup --python blender_run.py -- <command> ...

Commands:
  import IN.tik|.skd|.skb OUT.blend|.glb|.gltf|.fbx|.obj [options]
  export IN.blend|.glb|.gltf|.fbx|.obj OUT.tik [options]
  preview IN.tik|.skd|.blend OUT.png [options]
  remaster-prepare IN.tik WORK_DIR [options]
  remaster-build WORK_DIR OUT_ROOT --candidates A.obj;B.glb [options]
"""

import argparse
import json
import math
import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import io_scene_mohaa  # noqa: E402
from io_scene_mohaa import exporter, importer  # noqa: E402


def clear_scene():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for coll in list(bpy.data.collections):
        bpy.data.collections.remove(coll)


def import_any(path, args):
    ext = os.path.splitext(path)[1].lower()
    if ext in (".tik", ".skd", ".skb"):
        clear_scene()
        arm, warnings = importer.import_model(
            bpy.context, path, args.folders, anim_mode=args.anims, anim_filter=args.filter, reference=args.reference,
            cases=args.cases, unit=args.unit, bone_axis=args.bone_axis, load_textures=not args.no_textures,
            pack_images=args.pack, extract_dir=args.extract_dir)
        return arm, warnings
    if ext == ".blend":
        bpy.ops.wm.open_mainfile(filepath=path)
        return None, []
    clear_scene()
    if ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=path, automatic_bone_orientation=False)
    elif ext == ".obj":
        bpy.ops.wm.obj_import(filepath=path)
    else:
        raise SystemExit("cannot read %s" % path)
    return None, []


def save_any(path, args):
    ext = os.path.splitext(path)[1].lower()
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    if ext == ".blend":
        if args.pack:
            bpy.ops.file.pack_all()
        bpy.ops.wm.save_as_mainfile(filepath=path)
    elif ext in (".glb", ".gltf"):
        bpy.ops.export_scene.gltf(filepath=path, export_format="GLB" if ext == ".glb" else "GLTF_SEPARATE",
                                  export_animations=True, export_animation_mode="ACTIONS")
    elif ext == ".fbx":
        bpy.ops.export_scene.fbx(filepath=path, add_leaf_bones=False, bake_anim_use_all_actions=True,
                                 path_mode="COPY", embed_textures=True)
    elif ext == ".obj":
        bpy.ops.wm.obj_export(filepath=path)
    else:
        raise SystemExit("cannot write %s" % path)


def pick_armature(name):
    arms = [o for o in bpy.context.scene.objects if o.type == "ARMATURE"]
    if name:
        arms = [o for o in arms if o.name == name]
    return arms[0] if arms else None


def preview(path, out, args):
    """A quick textured render of a model from the front, for checking an import."""
    arm, warnings = import_any(path, args)
    scene = bpy.context.scene
    if args.frame is not None:
        scene.frame_set(args.frame)
    meshes = [o for o in scene.objects if o.type == "MESH"]
    if args.focus:
        meshes = [o for o in meshes if o.name.lower().startswith(args.focus.lower())] or meshes
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    from mathutils import Vector
    lo = Vector((1e9, 1e9, 1e9))
    hi = Vector((-1e9, -1e9, -1e9))
    for o in meshes:
        ev = o.evaluated_get(dg)
        for c in ev.bound_box:
            w = o.matrix_world @ Vector(c)
            lo = Vector(map(min, lo, w))
            hi = Vector(map(max, hi, w))
    center = (lo + hi) / 2
    size = max((hi - lo).length, 1e-3)
    cam_data = bpy.data.cameras.new("cam")
    cam_data.lens = 50
    cam = bpy.data.objects.new("cam", cam_data)
    scene.collection.objects.link(cam)
    yaw = math.radians(args.yaw)
    dist = size * 1.6
    cam.location = center + Vector((math.cos(yaw) * dist, math.sin(yaw) * dist, size * 0.25))
    direction = center - cam.location
    cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    cam_data.clip_end = dist * 10
    cam_data.clip_start = dist / 1000
    scene.camera = cam
    scene.render.engine = "BLENDER_WORKBENCH"
    shading = scene.display.shading
    shading.light = "STUDIO"
    shading.color_type = "TEXTURE"
    scene.render.resolution_x = args.size
    scene.render.resolution_y = args.size
    scene.render.filepath = out
    scene.render.image_settings.file_format = "PNG"
    bpy.ops.render.render(write_still=True)
    return warnings


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser(prog="blender_run.py")
    ap.add_argument("command", choices=("import", "export", "preview", "remaster-prepare", "remaster-build"))
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--folders", default="", help="game folders separated by ;")
    ap.add_argument("--anims", default="REFERENCE", choices=("NONE", "REFERENCE", "FILTER", "ALL"))
    ap.add_argument("--filter", default="")
    ap.add_argument("--reference", default="")
    ap.add_argument("--cases", default="")
    ap.add_argument("--unit", default="METERS", choices=("METERS", "GAME"))
    ap.add_argument("--bone-axis", default="MOHAA", choices=("MOHAA", "KEEP"))
    ap.add_argument("--no-textures", action="store_true")
    ap.add_argument("--pack", action="store_true")
    ap.add_argument("--extract-dir", default=None)
    ap.add_argument("--armature", default="")
    ap.add_argument("--skd-version", type=int, default=5)
    ap.add_argument("--export-anims", default="ALL", choices=("ALL", "ACTIVE", "NONE"))
    ap.add_argument("--scale", type=float, default=None, help="TIKI scale for models made in Blender")
    ap.add_argument("--pk3", default=None)
    ap.add_argument("--frame", type=int, default=None)
    ap.add_argument("--yaw", type=float, default=-60.0)
    ap.add_argument("--size", type=int, default=768)
    ap.add_argument("--focus", default="", help="preview: frame the objects whose name starts with this")
    ap.add_argument("--spread", type=float, default=30.0, help="remaster: degrees the arms are lifted")
    ap.add_argument("--check-anims", default="auto", help="remaster: animations for the stretch check")
    ap.add_argument("--candidates", default="", help="remaster-build: generated meshes, ; separated")
    ap.add_argument("--options", default="{}", help="remaster-build: JSON of remaster.Options")
    args = ap.parse_args(argv)
    args.folders = [f for f in args.folders.split(";") if f]
    io_scene_mohaa.register()

    result = {"warnings": [], "written": []}
    if args.command == "import":
        if args.extract_dir is None and args.output.lower().endswith(".blend"):
            args.pack = True  # textures pulled into Blender's temp folder would not outlive it
        _, warnings = import_any(args.input, args)
        save_any(args.output, args)
        result["warnings"] = warnings
        result["written"] = [args.output]
    elif args.command == "export":
        import_any(args.input, args)
        arm = pick_armature(args.armature)
        meshes = None
        if arm is None:
            meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
        written, warnings = exporter.export_model(
            bpy.context, args.output, arm_obj=arm, meshes=meshes, skd_version=args.skd_version,
            anim_mode=args.export_anims, pk3_path=args.pk3, scale=args.scale)
        result["warnings"] = warnings
        result["written"] = written
    elif args.command == "remaster-prepare":
        from io_scene_mohaa import remaster
        clear_scene()
        r = remaster.prepare(bpy.context, args.input, args.folders, args.output, spread=args.spread,
                             check_anims=args.check_anims, size=args.size)
        result["warnings"] = r["warnings"]
        result["written"] = r["views"] + [r["blend"]]
        result["report"] = r
    elif args.command == "remaster-build":
        from io_scene_mohaa import remaster
        opts = remaster.Options(**json.loads(args.options))
        cands = [c for c in args.candidates.split(";") if c]
        r = remaster.build_from_blend(bpy.context, os.path.join(args.input, "source.blend"), cands, args.output,
                                      args.input, opts)
        result["warnings"] = r["warnings"]
        result["written"] = r["written"] + [r["sheet"]]
        result["report"] = r
    else:
        result["warnings"] = preview(args.input, args.output, args)
        result["written"] = [args.output]
    print("MOHAA_RESULT " + json.dumps(result))


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception:
        import traceback
        traceback.print_exc()
        sys.exit(1)
