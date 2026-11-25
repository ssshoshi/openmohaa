"""Run inside Blender: import a weapon source of any kind and save it as one glTF binary.

    blender -b --factory-startup --python blender_to_glb.py -- OUT.glb [--decimate=R] SRC [SRC ...]

Each SRC is a .fbx, .dae, .obj, .glb/.gltf or .blend (a .blend is opened, the others are
imported into an empty scene). Every mesh keeps its object name; transforms are applied,
so the vertices come out in the scene's space (glTF: Y up, metres). Image textures are
embedded where the source links them. Lights, cameras and empties are dropped. With
--decimate=R each mesh keeps that share of its triangles (collapse, UVs kept).
"""

import math
import os
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
out, srcs = argv[0], argv[1:]
decimate = None
if srcs and srcs[0].startswith("--decimate="):
    decimate = float(srcs.pop(0).split("=", 1)[1])

blend = [s for s in srcs if s.lower().endswith(".blend")]
if blend:
    bpy.ops.wm.open_mainfile(filepath=blend[0])
else:
    bpy.ops.wm.read_factory_settings(use_empty=True)
for src in srcs:
    ext = os.path.splitext(src)[1].lower()
    if ext == ".blend":
        continue
    if ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=src)
    elif ext == ".dae":
        bpy.ops.wm.collada_import(filepath=src)
    elif ext == ".obj":
        bpy.ops.wm.obj_import(filepath=src)
    elif ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=src)
    else:
        raise SystemExit("cannot read %s" % src)

for ob in list(bpy.data.objects):
    if ob.type != "MESH":
        if ob.type in ("LIGHT", "CAMERA"):
            bpy.data.objects.remove(ob)
        continue
    for mod in list(ob.modifiers):  # armatures etc. would pose it: keep the rest shape
        if mod.type == "ARMATURE":
            ob.modifiers.remove(mod)
for ob in bpy.data.objects:
    ob.hide_set(False)
    ob.hide_viewport = False
    ob.hide_render = False
bpy.ops.object.select_all(action="DESELECT")
meshes = [ob for ob in bpy.context.scene.objects if ob.type == "MESH"]
for ob in meshes:
    ob.select_set(True)
if meshes:
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
    bpy.ops.object.make_single_user(object=True, obdata=True)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    if decimate:
        for ob in meshes:
            mod = ob.modifiers.new("decimate", "DECIMATE")
            mod.decimate_type = "COLLAPSE"
            mod.ratio = decimate
            mod.use_collapse_triangulate = True
        # the collapse leaves the source's sharp edges smoothed over (a flat top shaded across
        # long diagonal triangles): smooth only between faces under 30 degrees apart, and each
        # vertex's normal mostly its biggest faces' (a broad flat top over its thin bevels)
        for ob in meshes:
            bpy.context.view_layer.objects.active = ob
            bpy.ops.object.modifier_apply(modifier="decimate")
        bpy.ops.object.shade_smooth_by_angle(angle=math.radians(30))
        for ob in meshes:
            wn = ob.modifiers.new("weighted", "WEIGHTED_NORMAL")
            wn.mode = "FACE_AREA"
            wn.weight = 100
            wn.keep_sharp = True
            bpy.context.view_layer.objects.active = ob
            bpy.ops.object.modifier_apply(modifier="weighted")
print("meshes: %s" % ", ".join("%s(%d)" % (ob.name, len(ob.data.polygons)) for ob in meshes))
bpy.ops.export_scene.gltf(filepath=out, export_format="GLB", use_selection=True,
                          export_apply=True, export_animations=False, export_skins=False,
                          export_morph=False, export_yup=True)
