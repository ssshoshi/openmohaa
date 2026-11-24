"""Run inside Blender: orthographic views of a glTF binary, one colour per mesh.

    blender -b --factory-startup --python blender_preview.py -- IN.glb OUT_PREFIX [AXES [tex|- [NAME,NAME...]]]

Renders OUT_PREFIX_<view>.png for the views in AXES (default "xyz": looking along +X, -Y,
-Z of glTF's space), the workbench engine, each mesh its own random colour, every mesh's
name and index printed with its bounds (for telling the pieces apart).
"""

import math
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
src, prefix = argv[0], argv[1]
axes = argv[2] if len(argv) > 2 else "xyz"
textured = len(argv) > 3 and argv[3] == "tex"
only = argv[4].split(",") if len(argv) > 4 and argv[4] else None

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
meshes = [ob for ob in bpy.context.scene.objects if ob.type == "MESH"]
if only:
    for ob in meshes:
        if ob.name not in only:
            bpy.data.objects.remove(ob)
    meshes = [ob for ob in bpy.context.scene.objects if ob.type == "MESH"]
lo = Vector((1e9, 1e9, 1e9))
hi = -lo
for i, ob in enumerate(meshes):
    pts = [ob.matrix_world @ Vector(c) for c in ob.bound_box]
    olo = Vector(map(min, *[p[:] for p in pts]))
    ohi = Vector(map(max, *[p[:] for p in pts]))
    lo = Vector(map(min, lo, olo))
    hi = Vector(map(max, hi, ohi))
    print("piece %d %s lo=%s hi=%s" % (i, ob.name, tuple(round(x, 3) for x in olo),
                                       tuple(round(x, 3) for x in ohi)))
centre, size = (lo + hi) / 2, hi - lo

scene = bpy.context.scene
scene.render.engine = "BLENDER_WORKBENCH"
shading = scene.display.shading
shading.light = "STUDIO"
shading.color_type = "TEXTURE" if textured else "RANDOM"
shading.show_cavity = True
scene.render.resolution_x, scene.render.resolution_y = 1600, 900
scene.render.film_transparent = False
world = bpy.data.worlds.new("w")
scene.world = world

cam_data = bpy.data.cameras.new("cam")
cam_data.type = "ORTHO"
cam = bpy.data.objects.new("cam", cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
# Blender is Z up: glTF +Y up became Blender +Z, glTF +Z became Blender -Y
views = {"x": ((1, 0, 0), (math.pi / 2, 0, math.pi / 2)),      # from +X
         "y": ((0, -1, 0), (math.pi / 2, 0, 0)),                # from -Y (glTF +Z, the front)
         "z": ((0, 0, 1), (0, 0, 0)),                           # from above
         "X": ((-1, 0, 0), (math.pi / 2, 0, -math.pi / 2)),     # from -X
         }
for a in axes:
    d, rot = views[a]
    span = max(size) * 1.1
    cam.location = centre + Vector(d) * span * 2
    cam.rotation_euler = rot
    cam_data.ortho_scale = span
    cam_data.clip_end = span * 10
    scene.render.filepath = "%s_%s.png" % (prefix, a)
    bpy.ops.render.render(write_still=True)
