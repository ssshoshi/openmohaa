"""Runs inside Blender for pyro.py: blender -b --factory-startup --python pyro_blender.py -- PRESET.json OUT_DIR [--stage sim|render|all]

Simulates one explosion with Mantaflow (fire + smoke) and renders it, seen from the side
with an orthographic camera, as three image sequences of square RGBA PNGs:

  pos/f####.png  smoke lit by three suns, one per colour channel: red from +X (right),
                 green from +Z (above), blue from +Y (behind it, facing the camera)
  neg/f####.png  the same from -X (left), -Z (below) and -Y (in front, from the camera)
  fire/f####.png the flame's own light, no suns; alpha is the smoke's cover in all three

The smoke is a neutral grey, so the three suns of a render do not mix: each channel is the
smoke lit from one side. That is "six-way lighting": the game shades the smoke from where
its own light is. pyro.py packs the sequences into the game's textures.
"""

import json
import math
import os
import random
import sys
import time

import bpy
from mathutils import Vector


def log(*a):
    print("PYRO", *a, flush=True)


def clear():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)


def add_sphere(name, loc, radius):
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=3, radius=radius, location=loc)
    o = bpy.context.object
    o.name = name
    return o


def build_sim(p, cache):
    sc = bpy.context.scene
    size = p["domain"]  # metres: x/y width, height
    bpy.ops.mesh.primitive_cube_add(size=1, location=(0, 0, size[1] * 0.5))
    dom = bpy.context.object
    dom.name = "domain"
    dom.scale = (size[0], size[0], size[1])
    bpy.ops.object.transform_apply(scale=True)
    m = dom.modifiers.new("Fluid", 'FLUID')
    m.fluid_type = 'DOMAIN'
    d = m.domain_settings
    d.domain_type = 'GAS'
    d.resolution_max = p["res"]
    d.use_adaptive_domain = False
    d.cache_directory = cache
    d.cache_type = 'ALL'
    d.cache_data_format = 'OPENVDB'
    d.cache_frame_start = 1
    d.cache_frame_end = p["frames"]
    d.alpha = p.get("alpha", 0.0)
    d.beta = p.get("beta", 1.5)
    d.vorticity = p.get("vorticity", 0.25)
    d.burning_rate = p.get("burning_rate", 0.6)
    d.flame_smoke = p.get("flame_smoke", 1.5)
    d.flame_vorticity = p.get("flame_vorticity", 0.6)
    d.flame_ignition = p.get("flame_ignition", 1.25)
    d.flame_max_temp = p.get("flame_max_temp", 2.0)
    d.use_dissolve_smoke = False
    d.time_scale = p.get("time_scale", 1.0)
    d.use_noise = p.get("noise", 0) > 0
    if d.use_noise:
        d.noise_scale = p["noise"]
        d.noise_strength = p.get("noise_strength", 1.0)
    # the ground is the domain's floor; the smoke leaves by the sides and the top
    d.use_collision_border_bottom = True
    for side in ("top", "left", "right", "front", "back"):
        setattr(d, "use_collision_border_" + side, False)

    rnd = random.Random(p.get("seed", 1))
    emit_end = p.get("emit_frames", 4)
    blobs = p["blobs"]
    # lumpy fuel: the burst is clumps and gaps, not round balls
    lumps = bpy.data.textures.new("lumps", 'CLOUDS')
    lumps.noise_scale = p.get("lump_scale", 0.35)
    lumps.noise_depth = 2
    lumps.contrast = p.get("lump_contrast", 2.0)
    for i in range(blobs["count"]):
        r = blobs["radius"] * rnd.uniform(0.6, 1.0) if i else blobs["radius"]
        off = blobs["spread"] if i else 0.0
        loc = (rnd.uniform(-off, off), rnd.uniform(-off, off),
               blobs["height"] + rnd.uniform(0, off))
        o = add_sphere("fuel%d" % i, loc, r)
        o.hide_render = True
        fm = o.modifiers.new("Fluid", 'FLUID')
        fm.fluid_type = 'FLOW'
        f = fm.flow_settings
        f.flow_type = 'BOTH'
        f.flow_behavior = 'INFLOW'
        f.flow_source = 'MESH'
        f.surface_distance = r * 0.5
        f.fuel_amount = p.get("fuel", 1.5)
        f.temperature = p.get("temperature", 2.0)
        f.density = p.get("density", 1.0)
        f.subframes = 2
        if p.get("lumps", True):
            f.use_texture = True
            f.noise_texture = lumps
            f.texture_map_type = 'AUTO'
            f.texture_size = 1.0
            f.texture_offset = rnd.uniform(0, 10)
        f.use_initial_velocity = True
        f.velocity_normal = p.get("burst", 6.0) * rnd.uniform(0.8, 1.2)
        f.velocity_random = p.get("burst_random", 1.0)
        f.velocity_coord = Vector(p.get("lift", (0, 0, 2.0)))
        # the burst lasts emit_frames, staggered a little per blob
        start = 1 + (rnd.randint(0, 1) if i else 0)
        f.use_inflow = True
        f.keyframe_insert("use_inflow", frame=start)
        f.use_inflow = False
        f.keyframe_insert("use_inflow", frame=start + emit_end)
        if start > 1:
            f.keyframe_insert("use_inflow", frame=1)
    # smoke that keeps pouring off it after the flash: a source of smoke only, at the
    # base, warm enough to rise and roll
    src = p.get("smoke_source")
    if src:
        o = add_sphere("smoke", (0, 0, src.get("height", 0.4)), src["radius"])
        o.hide_render = True
        fm = o.modifiers.new("Fluid", 'FLUID')
        fm.fluid_type = 'FLOW'
        f = fm.flow_settings
        f.flow_type = 'SMOKE'
        f.flow_behavior = 'INFLOW'
        f.flow_source = 'MESH'
        f.surface_distance = src["radius"] * 0.5
        f.density = src.get("density", 1.0)
        f.temperature = src.get("temperature", 1.0)
        f.subframes = 2
        f.use_texture = True
        f.noise_texture = lumps
        f.texture_map_type = 'AUTO'
        f.use_initial_velocity = True
        f.velocity_normal = src.get("burst", 2.0)
        f.velocity_random = src.get("burst_random", 1.0)
        f.velocity_coord = Vector(src.get("lift", (0, 0, 1.0)))
        start, end = src.get("frames", [3, 40])
        f.use_inflow = False
        f.keyframe_insert("use_inflow", frame=1)
        f.use_inflow = True
        f.keyframe_insert("use_inflow", frame=start)
        f.use_inflow = False
        f.keyframe_insert("use_inflow", frame=end)
    sc.frame_start = 1
    sc.frame_end = p["frames"]
    sc.render.fps = p.get("fps", 24)
    return dom


def bake(dom):
    t = time.time()
    with bpy.context.temp_override(object=dom, active_object=dom, selected_objects=[dom]):
        bpy.ops.fluid.bake_all()
    log("baked in %.0f s" % (time.time() - t))


def material(dom, p):
    mat = bpy.data.materials.new("pyro")
    mat.use_nodes = True
    nt = mat.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    vol = nt.nodes.new("ShaderNodeVolumePrincipled")
    nt.links.new(vol.outputs[0], out.inputs["Volume"])
    vol.inputs["Color"].default_value = (0.5, 0.5, 0.5, 1)
    vol.inputs["Anisotropy"].default_value = p.get("anisotropy", 0.2)
    dens = nt.nodes.new("ShaderNodeAttribute")
    dens.attribute_name = "density"
    dmul = nt.nodes.new("ShaderNodeMath")
    dmul.operation = 'MULTIPLY'
    dmul.inputs[1].default_value = p.get("density_scale", 6.0)
    nt.links.new(dens.outputs["Fac"], dmul.inputs[0])
    nt.links.new(dmul.outputs[0], vol.inputs["Density"])
    flame = nt.nodes.new("ShaderNodeAttribute")
    flame.attribute_name = "flame"
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    cr = ramp.color_ramp
    stops = [tuple(s) for s in p.get("fire_ramp", [
        (0.0, (0, 0, 0)), (0.2, (0.5, 0.04, 0.005)), (0.5, (1.0, 0.25, 0.03)),
        (0.85, (1.0, 0.55, 0.12)), (1.0, (1.0, 0.8, 0.4))])]
    cr.elements[0].position, cr.elements[0].color = stops[0][0], (*stops[0][1], 1)
    cr.elements[1].position, cr.elements[1].color = stops[-1][0], (*stops[-1][1], 1)
    for pos, col in stops[1:-1]:
        e = cr.elements.new(pos)
        e.color = (*col, 1)
    nt.links.new(flame.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], vol.inputs["Emission Color"])
    fmul = nt.nodes.new("ShaderNodeMath")
    fmul.operation = 'MULTIPLY'
    fmul.name = "fire_strength"
    fmul.inputs[1].default_value = p.get("fire_strength", 12.0)
    nt.links.new(flame.outputs["Fac"], fmul.inputs[0])
    nt.links.new(fmul.outputs[0], vol.inputs["Emission Strength"])
    dom.data.materials.clear()
    dom.data.materials.append(mat)
    return fmul


def sun(name, toward, color, strength):
    data = bpy.data.lights.new(name, 'SUN')
    data.color = color
    data.energy = strength
    data.angle = math.radians(8)
    o = bpy.data.objects.new(name, data)
    bpy.context.scene.collection.objects.link(o)
    o.rotation_mode = 'QUATERNION'
    o.rotation_quaternion = Vector((0, 0, -1)).rotation_difference(Vector(toward).normalized())
    return o


def setup_render(p):
    sc = bpy.context.scene
    size = p["domain"]
    cam_data = bpy.data.cameras.new("cam")
    cam_data.type = 'ORTHO'
    cam_data.ortho_scale = max(size[0], size[1])
    cam_data.clip_end = 200
    cam = bpy.data.objects.new("cam", cam_data)
    sc.collection.objects.link(cam)
    # looking along +Y at the middle of the domain; the frame is square
    cam.location = (0, -50, size[1] * 0.5)
    cam.rotation_euler = (math.radians(90), 0, 0)
    sc.camera = cam
    r = sc.render
    r.engine = 'CYCLES'
    r.resolution_x = r.resolution_y = p.get("px", 256)
    r.resolution_percentage = 100
    r.film_transparent = True
    r.image_settings.file_format = 'PNG'
    r.image_settings.color_mode = 'RGBA'
    r.image_settings.color_depth = '8'
    try:
        sc.view_settings.view_transform = 'Standard'
    except TypeError:
        log("no Standard view transform")
    sc.view_settings.look = 'None'
    sc.view_settings.exposure = 0
    cy = sc.cycles
    cy.device = 'GPU'
    prefs = bpy.context.preferences.addons['cycles'].preferences
    prefs.compute_device_type = 'CUDA'
    prefs.get_devices()
    for dev in prefs.devices:
        dev.use = dev.type == 'CUDA'
    cy.samples = p.get("samples", 64)
    cy.use_denoising = True
    cy.denoising_use_gpu = False
    cy.max_bounces = 4
    cy.volume_bounces = p.get("volume_bounces", 2)
    cy.volume_step_rate = p.get("step_rate", 1.0)
    world = bpy.data.worlds.new("black")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0
    sc.world = world


def render(p, dom, out):
    fire_mul = material(dom, p)
    setup_render(p)
    s = p.get("sun", 3.0)
    suns = {
        "pos": [((-1, 0, 0), (1, 0, 0)), ((0, 0, -1), (0, 1, 0)), ((0, -1, 0), (0, 0, 1))],
        "neg": [((1, 0, 0), (1, 0, 0)), ((0, 0, 1), (0, 1, 0)), ((0, 1, 0), (0, 0, 1))],
    }
    fire = p.get("fire_strength", 12.0)
    sc = bpy.context.scene
    sc.frame_step = p.get("frame_step", 1)
    for mode in p.get("modes", ("pos", "neg", "fire")):
        for o in [o for o in bpy.data.objects if o.type == 'LIGHT']:
            bpy.data.objects.remove(o, do_unlink=True)
        for i, (toward, col) in enumerate(suns.get(mode, [])):
            sun("%s%d" % (mode, i), toward, col, s)
        fire_mul.inputs[1].default_value = fire if mode == "fire" else 0.0
        sc.render.filepath = os.path.join(out, mode, "f####")
        t = time.time()
        bpy.ops.render.render(animation=True)
        log("rendered %s in %.0f s" % (mode, time.time() - t))


def export_grids(p, dom, out):
    """The baked smoke and flame, a frame at a time, halved to the game's grid (pyro.py
    makes the .vol of it): grids.bin, frame after frame of density then flame, float16
    (z, y, x; x fastest), and grids.json with the frames' times."""
    import numpy as np
    sc = bpy.context.scene
    first = p.get("start_frame", 1)
    # every frame while it bursts, then fewer as it slows
    frames = [f for f in range(first, p["frames"] + 1)
              if f - first < 24 or (f - first < 54 and (f - first) % 2 == 0)
              or (f - first < 96 and (f - first) % 3 == 0) or (f - first) % 5 == 0]
    down = p.get("grid_down", 2)
    t = time.time()
    with open(os.path.join(out, "grids.bin"), "wb") as fh:
        for f in frames:
            sc.frame_set(f)
            s = dom.evaluated_get(bpy.context.evaluated_depsgraph_get()).modifiers["Fluid"].domain_settings
            n = round(len(s.density_grid) ** (1 / 3))
            for grid in (s.density_grid, s.flame_grid):
                g = np.array(grid[:], dtype=np.float32).reshape(n, n, n)
                if down > 1:
                    m = n // down
                    g = g[:m * down, :m * down, :m * down].reshape(m, down, m, down, m, down).mean(axis=(1, 3, 5))
                fh.write(g.astype(np.float16).tobytes())
    meta = {"size": n // down, "fps": p.get("fps", 24), "frames": [(f - first) / p.get("fps", 24) for f in frames]}
    with open(os.path.join(out, "grids.json"), "w") as fh:
        json.dump(meta, fh)
    log("exported %d frames of %d^3 in %.0f s" % (len(frames), n // down, time.time() - t))


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    preset, out = argv[0], argv[1]
    stage = argv[argv.index("--stage") + 1] if "--stage" in argv else "all"
    with open(preset) as fh:
        p = json.load(fh)
    os.makedirs(out, exist_ok=True)
    cache = os.path.join(out, "cache")
    blend = os.path.join(out, "pyro.blend")
    if stage in ("render", "grids"):
        # the scene saved after its bake knows its cache is baked
        bpy.ops.wm.open_mainfile(filepath=blend)
        dom = bpy.data.objects["domain"]
    else:
        clear()
        dom = build_sim(p, cache)
        bake(dom)
        bpy.ops.wm.save_as_mainfile(filepath=blend)
    if stage in ("render", "all"):
        render(p, dom, out)
    if stage in ("grids", "all"):
        export_grids(p, dom, out)
    log("done")


main()
