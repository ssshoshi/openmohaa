"""Materials from MOHAA shaders, and image files back out of Blender materials."""

import os

import bpy

from .mohaa import shader as shaderlib


def extract_image(fs, game_path, extract_dir):
    """Write a game image to extract_dir (keeping its game path) and return the file."""
    data = fs.read(game_path)
    if data is None:
        return None
    out = os.path.join(extract_dir, *game_path.replace("\\", "/").split("/"))
    if not os.path.exists(out) or os.path.getsize(out) != len(data):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(data)
    return out


def load_image(fs, game_path, extract_dir, pack):
    for img in bpy.data.images:
        if img.get("mohaa_path", "").lower() == game_path.lower():
            return img
    path = extract_image(fs, game_path, extract_dir)
    if path is None:
        return None
    img = bpy.data.images.load(path, check_existing=True)
    img["mohaa_path"] = game_path
    if pack:
        img.pack()
    return img


def _set_alpha_mode(mat, info):
    if hasattr(mat, "surface_render_method"):  # 4.2+
        mat.surface_render_method = "BLENDED" if info.blend else "DITHERED"
    elif hasattr(mat, "blend_method"):
        mat.blend_method = "BLEND" if info.blend else "CLIP"


def get_material(name, fs, library, extract_dir, load_textures=True, pack=False):
    """The material for a MOHAA shader name, made once."""
    for mat in bpy.data.materials:
        if mat.get("mohaa_shader", None) == name:
            return mat
    info = library.resolve(name) if library else shaderlib.ShaderInfo(name)
    mat = bpy.data.materials.new(name)
    mat["mohaa_shader"] = name
    if info.image:
        mat["mohaa_image"] = info.image
    mat.use_backface_culling = not info.two_sided
    if hasattr(mat, "use_nodes") and not mat.use_nodes:
        mat.use_nodes = True
    nt = mat.node_tree
    bsdf = next((n for n in nt.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf is None:
        return mat
    for key in ("Specular IOR Level", "Specular"):
        if key in bsdf.inputs:
            bsdf.inputs[key].default_value = 0.1
            break
    bsdf.inputs["Roughness"].default_value = 0.8

    img = None
    if load_textures and info.image and fs is not None:
        img = load_image(fs, info.image, extract_dir, pack)
    if img is None:
        return mat
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    tex.location = (-420, 260)
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    if info.alpha_test or info.blend:
        if info.additive:
            # an additive stage: black is see-through
            nt.links.new(tex.outputs["Color"], bsdf.inputs["Emission Color"]
                         if "Emission Color" in bsdf.inputs else bsdf.inputs["Emission"])
        else:
            nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
        _set_alpha_mode(mat, info)
    if info.normal_map:
        nimg = load_image(fs, info.normal_map, extract_dir, pack)
        if nimg is not None:
            nimg.colorspace_settings.name = "Non-Color"
            ntex = nt.nodes.new("ShaderNodeTexImage")
            ntex.image = nimg
            ntex.location = (-420, -60)
            nmap = nt.nodes.new("ShaderNodeNormalMap")
            nmap.location = (-180, -60)
            nt.links.new(ntex.outputs["Color"], nmap.inputs["Color"])
            nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    return mat


def material_image(mat):
    """The image a material shows: the one feeding Base Color, else any image node."""
    if mat is None or not mat.node_tree:
        return None
    nt = mat.node_tree
    bsdf = next((n for n in nt.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf is not None:
        stack = [bsdf.inputs["Base Color"]]
        seen = set()
        while stack:
            sock = stack.pop()
            for link in sock.links:
                node = link.from_node
                if node.name in seen:
                    continue
                seen.add(node.name)
                if node.type == "TEX_IMAGE" and node.image:
                    return node.image
                stack.extend(node.inputs)
    for node in nt.nodes:
        if node.type == "TEX_IMAGE" and node.image:
            return node.image
    return None


def save_image_tga(img, path):
    """Write an image's pixels as an uncompressed 32-bit TGA, the format the engine reads
    best. Pixels are written as stored: byte images keep their exact colours."""
    import numpy as np

    w, h = img.size
    if not w or not h:
        raise RuntimeError("image %r has no pixels" % img.name)
    px = np.empty(w * h * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    if img.is_float:
        rgb = px.reshape(-1, 4)[:, :3]
        px.reshape(-1, 4)[:, :3] = np.where(rgb <= 0.0031308, rgb * 12.92,
                                            1.055 * np.power(np.clip(rgb, 0, None), 1 / 2.4) - 0.055)
    data = (np.clip(px, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8).reshape(h, w, 4)
    bgra = data[:, :, [2, 1, 0, 3]]  # Blender's rows already run bottom up, as TGA's do
    header = bytes([0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, w & 255, w >> 8, h & 255, h >> 8, 32, 8])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(header)
        f.write(bgra.tobytes())
    return path
