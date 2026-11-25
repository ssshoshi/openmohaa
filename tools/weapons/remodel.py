#!/usr/bin/env python3
"""The game's weapons remade from models of the player's own (opm-weapons's generator).

The Thompson and the Garand have tools of their own (thompson.py, garand.py), written while
working out how; the rest go through this one, each weapon described by a spec (WEAPONS,
in weapon_specs.py). The sources are the player's, read in place under

    <MOHAA_GAME>/models/<folder>/

and never committed. They come in whatever form they were published in: glTF binaries,
.obj sets (Sketchfab dumps: one .obj a piece, often each piece twice), .fbx, a Collada .dae
in a .zip, an .fbx in a .7z. All but glTF and .obj are turned into glTF by Blender first
(blender_to_glb.py; BLENDER names its executable), Collada by our own reader (Blender 5
dropped its importer), archives unpacked with Python or 7-Zip; the results are cached
under ~/.cache/openmohaa-weapons.

For each weapon:

  1. the pieces the spec keeps (Notes.txt in a folder says which: a scope or not, a bipod
     off) are fitted onto the game's model as it is posed by its idle animation: a scaled
     ICP from each of the 24 axis-aligned turns, the best kept, so that the sights, the
     grip and the muzzle land where the arms' animations, the aim pose and the tags expect
     them. A spec may first place a piece the source left at its own origin (a scope)
     onto the game's corresponding part, or fold one (BAR sights);
  2. each piece of the new model takes the bone and the surface of the game's model
     nearest it (a connected piece votes as one, so a slide or a magazine moves whole; a
     spec's boxes override). On the game's bones the game's animations move it as they
     moved the old one; under the game's surface names the .tik's frame commands (hide
     the magazine for the reload, and the like) still reach it. The engine takes a
     surface of at most 1000 vertices and 2000 triangles, 32 surfaces a model and 24
     surface lines in a .tik's setup, so ours are named <game surface>_<material
     letter>_<n>, the setup shades each group with one wildcard line, and the frame
     commands are given <game surface>_* to reach them all;
  3. the hand's magazine or round (models/ammo/...) is the new magazine, moved as the game's
     gun magazine is onto the game's hand-held one;
  4. textured as the Thompson is: base colour with a little AO, the normal map with its
     green down the image (turned over when it is not: see garand.normal_map_green_down),
     GL2's specular map from metalness and roughness;
  5. the .tik files pointed at the new model (written here, not patched: without the
     sources the pak is empty and the game keeps its weapons).

    tools/weapons/remodel.py --out build/opm-weapons [--only colt45,p38]

REMODEL_NO_SIGHTS=1 leaves the sights where the fit put them (to look at them with
overlay.py). blender_preview.py and overlay.py draw a source's meshes and a result over the
game's model, for writing a spec.
"""

import argparse
import glob
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile
from urllib.parse import unquote

import numpy as np
from PIL import Image
from scipy.spatial import Delaunay, cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from thompson import (icp, model_like, pose, read_anim, sample, skc, skd,  # noqa: E402
                      split, vertex_normals, vfs, write)
from thompson import parts as welded_parts  # noqa: E402
from scipy.sparse import coo_matrix  # noqa: E402
from scipy.sparse.csgraph import connected_components  # noqa: E402

# Whether connected parts join vertices that share a position (the default) or only those
# the mesh joins (index_parts: a source whose moving parts touch the rest)
WELD = [True]


def parts(pos, tris):
    if WELD[0]:
        return welded_parts(pos, tris)
    n = len(pos)
    g = coo_matrix((np.ones(3 * len(tris)), (tris.ravel(), tris[:, [1, 2, 0]].ravel())), shape=(n, n))
    return connected_components(g, directed=False)[1][tris[:, 0]]
from garand import GLTF_TYPES, GLTF_WIDTH, normal_map_green_down, sized  # noqa: E402

GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")
PAK = "zzzzzzzzzzzzzzzz-opm-weapons.pk3"   # what this builds: never read back from
OUR_PAKS = [PAK, "zzzzzzzzzzzzzzzz-opm-thompson.pk3", "zzzzzzzzzzzzzzzz-opm-garand.pk3"]
CACHE = os.path.expanduser("~/.cache/openmohaa-weapons")
BLENDER = os.environ.get("BLENDER", "/mnt/c/Program Files/Blender Foundation/Blender 5.2/blender.exe")
SEVEN_ZIP = os.environ.get("SEVEN_ZIP", "/mnt/c/Program Files/7-Zip/7z.exe")

AO_AMOUNT = 0.5
METAL_SHARE = 0.5   # see thompson.py: the cubemap turns full-reflectance metal into a mirror
MAX_GLOSS = 0.75
# a material's settings given as values, not images: base_tint (a colour shading a grey base
# map), rough_value (no roughness map), metal_diffuse (the share of diffuse light metal keeps:
# a PBR metal has none), metal_share / max_gloss (METAL_SHARE, MAX_GLOSS for a polished gun)
VALUE_MAPS = ("base_tint", "rough_value", "metal_diffuse", "metal_share", "max_gloss")

Image.MAX_IMAGE_PIXELS = None


# ----------------------------------------------------------------------------- sources

class Piece:
    """One mesh of the source: positions, normals, UVs (t down the image), triangles, and
    its material: a dict of map name -> PIL image (base, normal, rough, metal, ao)."""

    def __init__(self, name, pos, normal, uv, tris, maps=None, material=""):
        self.name, self.pos, self.uv, self.tris = name, pos, uv, tris
        self.normal = normal if normal is not None else vertex_normals(pos, tris)
        self.maps, self.material = maps or {}, material

    def moved(self, m):
        """m: 4x4 (scale allowed)."""
        n = self.normal @ np.linalg.inv(m[:3, :3])
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
        tris = self.tris if np.linalg.det(m[:3, :3]) > 0 else self.tris[:, ::-1]
        return Piece(self.name, self.pos @ m[:3, :3].T + m[:3, 3], n, self.uv, tris, self.maps,
                     self.material)

    def subset(self, keep):
        """keep: a boolean per triangle."""
        tris = self.tris[keep]
        used = np.unique(tris)
        remap = np.full(len(self.pos), -1)
        remap[used] = np.arange(len(used))
        return Piece(self.name, self.pos[used], self.normal[used], self.uv[used], remap[tris],
                     self.maps, self.material)


def read_glb(path):
    """A glTF binary's meshes as Pieces, in the scene's space, each with its node's name
    and its maps (embedded, or files beside it)."""
    data = open(path, "rb").read()
    if data[:4] != b"glTF":
        raise SystemExit("%s is not a glTF binary" % path)
    size = struct.unpack_from("<I", data, 12)[0]
    j = json.loads(data[20:20 + size])
    binary = memoryview(data)[20 + size + 8:]

    def view(index):
        bv = j["bufferViews"][index]
        start = bv.get("byteOffset", 0)
        return binary[start:start + bv["byteLength"]], bv.get("byteStride")

    def accessor(index):
        a = j["accessors"][index]
        raw, stride = view(a["bufferView"])
        dt, width = np.dtype(GLTF_TYPES[a["componentType"]]), GLTF_WIDTH[a["type"]]
        stride = stride or dt.itemsize * width
        buf = np.frombuffer(raw, np.uint8, offset=a.get("byteOffset", 0))
        rows = np.lib.stride_tricks.as_strided(buf, (a["count"], dt.itemsize * width), (stride, 1))
        out = np.ascontiguousarray(rows).view(dt).reshape(a["count"], width).astype(float)
        return out[:, 0] if width == 1 else out

    images = {}

    def image(ref):
        k = j["textures"][ref["index"]]["source"]
        if k not in images:
            img = j["images"][k]
            if "uri" in img:     # a file beside the model (a .glb with its textures folder)
                images[k] = Image.open(os.path.join(os.path.dirname(path), unquote(img["uri"])))
            else:
                raw, _ = view(img["bufferView"])
                images[k] = Image.open(io.BytesIO(bytes(raw)))
            images[k].load()
        return images[k]

    def maps(mat):
        out = {}
        pbr = mat.get("pbrMetallicRoughness", {})
        if "baseColorTexture" in pbr:
            out["base"] = image(pbr["baseColorTexture"])
        if "metallicRoughnessTexture" in pbr:
            orm = image(pbr["metallicRoughnessTexture"]).convert("RGB")
            _, g, b = orm.split()
            out["rough"], out["metal"] = g, b
        if "normalTexture" in mat:
            out["normal"] = image(mat["normalTexture"])
        if "occlusionTexture" in mat:
            out["ao"] = image(mat["occlusionTexture"]).convert("RGB").split()[0]
        return out

    pieces = []

    def trs(node):
        m = np.eye(4)
        if "matrix" in node:
            return np.array(node["matrix"], float).reshape(4, 4).T
        if "rotation" in node:
            x, y, z, w = node["rotation"]
            m[:3, :3] = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
        m[:3, :3] *= np.array(node.get("scale", (1, 1, 1)))[None, :]
        m[:3, 3] = node.get("translation", (0, 0, 0))
        return m

    def walk(index, parent):
        node = j["nodes"][index]
        m = parent @ trs(node)
        for prim in j["meshes"][node["mesh"]]["primitives"] if "mesh" in node else ():
            a = prim["attributes"]
            p = accessor(a["POSITION"])
            n = accessor(a["NORMAL"]) if "NORMAL" in a else None
            uv = accessor(a["TEXCOORD_0"]) if "TEXCOORD_0" in a else np.zeros((len(p), 2))
            t = (accessor(prim["indices"]).astype(int).reshape(-1, 3) if "indices" in prim
                 else np.arange(len(p)).reshape(-1, 3))
            mat = j["materials"][prim["material"]] if "material" in prim else {}
            piece = Piece(node.get("name", "node%d" % index), p, n, uv, t, maps(mat), mat.get("name", ""))
            pieces.append(piece.moved(m))
        for c in node.get("children", ()):
            walk(c, m)

    for root in j["scenes"][j.get("scene", 0)]["nodes"]:
        walk(root, np.eye(4))
    return pieces


def read_dae(path):
    """A Collada file's meshes (triangles and polylists) as Pieces, placed by the visual
    scene's nodes, Y up as glTF has it."""
    ns = {"c": "http://www.collada.org/2005/11/COLLADASchema"}
    root = ET.parse(path).getroot()
    up = root.findtext("c:asset/c:up_axis", "Y_UP", ns)
    unit = float(root.find("c:asset/c:unit", ns).get("meter", "1")) if root.find("c:asset/c:unit", ns) is not None else 1.0

    def floats(src):
        arr = np.array(src.findtext("c:float_array", "", ns).split(), float)
        stride = int(src.find("c:technique_common/c:accessor", ns).get("stride", "1"))
        return arr.reshape(-1, stride)

    geoms = {}
    for g in root.iterfind("c:library_geometries/c:geometry", ns):
        mesh = g.find("c:mesh", ns)
        if mesh is None:
            continue
        sources = {s.get("id"): floats(s) for s in mesh.iterfind("c:source", ns)}
        vin = mesh.find("c:vertices", ns)
        vmap = {}
        for i in vin.iterfind("c:input", ns):   # the first of each (a second UV set is not ours)
            vmap.setdefault(i.get("semantic"), i.get("source")[1:])
        out = []
        for prim in list(mesh.iterfind("c:triangles", ns)) + list(mesh.iterfind("c:polylist", ns)):
            inputs = [(i.get("semantic"), i.get("source")[1:], int(i.get("offset")), int(i.get("set", "0")))
                      for i in prim.iterfind("c:input", ns)]
            width = max(i[2] for i in inputs) + 1
            idx = np.array(prim.findtext("c:p", "", ns).split(), int).reshape(-1, width)
            if prim.tag.endswith("polylist"):
                counts = np.array(prim.findtext("c:vcount", "", ns).split(), int)
                starts = np.concatenate([[0], np.cumsum(counts)[:-1]])
                fan = [(s, s + k, s + k + 1) for s, c in zip(starts, counts) for k in range(1, c - 1)]
                idx = idx[np.array(fan).ravel()]
            pos = nrm = uv = None
            for sem, src, off, st in inputs:
                if sem == "VERTEX":
                    pos = sources[vmap["POSITION"]][idx[:, off]]
                    if "NORMAL" in vmap:
                        nrm = sources[vmap["NORMAL"]][idx[:, off]]
                    if "TEXCOORD" in vmap and uv is None:
                        uv = sources[vmap["TEXCOORD"]][idx[:, off]][:, :2]
                elif sem == "NORMAL":
                    nrm = sources[src][idx[:, off]]
                elif sem == "TEXCOORD" and uv is None:
                    uv = sources[src][idx[:, off]][:, :2]
            uv = np.zeros((len(pos), 2)) if uv is None else np.stack([uv[:, 0], 1.0 - uv[:, 1]], 1)
            # Collada indexes each corner: one vertex for each distinct position, normal, UV
            nn = nrm if nrm is not None else np.zeros_like(pos)
            _, first, corner = np.unique(np.round(np.hstack([pos, nn, uv]), 6), axis=0,
                                         return_index=True, return_inverse=True)
            pos, uv = pos[first], uv[first]
            nrm = nrm[first] if nrm is not None else None
            tris = corner.ravel().reshape(-1, 3)
            out.append((pos, nrm, uv, tris, prim.get("material", "")))
        geoms[g.get("id")] = (g.get("name") or g.get("id"), out)

    pieces = []

    def node_matrix(node):
        m = np.eye(4)
        for el in node:
            tag = el.tag.split("}")[1]
            v = np.array(el.text.split(), float) if el.text else None
            if tag == "matrix":
                m = m @ v.reshape(4, 4)
            elif tag == "translate":
                t = np.eye(4)
                t[:3, 3] = v
                m = m @ t
            elif tag == "rotate":
                ax, ang = v[:3] / np.linalg.norm(v[:3]), np.radians(v[3])
                k = np.array([[0, -ax[2], ax[1]], [ax[2], 0, -ax[0]], [-ax[1], ax[0], 0]])
                r = np.eye(4)
                r[:3, :3] = np.eye(3) + np.sin(ang) * k + (1 - np.cos(ang)) * k @ k
                m = m @ r
            elif tag == "scale":
                m = m @ np.diag(list(v) + [1.0])
        return m

    def walk(node, parent):
        m = parent @ node_matrix(node)
        for inst in node.iterfind("c:instance_geometry", ns):
            name, prims = geoms[inst.get("url")[1:]]
            for pos, nrm, uv, tris, mat in prims:
                pieces.append(Piece(node.get("name") or name, pos, nrm, uv, tris, {}, mat).moved(m))
        for child in node.iterfind("c:node", ns):
            walk(child, m)

    scene = root.find("c:library_visual_scenes/c:visual_scene", ns)
    for node in scene.iterfind("c:node", ns):
        walk(node, np.eye(4))
    fix = np.eye(4) * unit
    fix[3, 3] = 1
    if up == "Z_UP":   # to Y up
        fix = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0], [0, 0, 0, 1]], float) @ fix
    return [p.moved(fix) for p in pieces]


def unpack(archive, key):
    """An archive's files, unpacked once into the cache: the folder."""
    dest = os.path.join(CACHE, key, "unpacked", os.path.basename(archive))
    stamp = "%d %d" % (os.path.getsize(archive), int(os.path.getmtime(archive)))
    mark = os.path.join(dest, ".stamp")
    if os.path.exists(mark) and open(mark).read() == stamp:
        return dest
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest)
    if archive.lower().endswith(".zip"):
        zipfile.ZipFile(archive).extractall(dest)
    else:
        seven = SEVEN_ZIP if os.path.exists(SEVEN_ZIP) else shutil.which("7z")
        if not seven:
            raise SystemExit("no 7-Zip to unpack %s (SEVEN_ZIP)" % archive)
        native = seven.endswith(".exe")
        run = subprocess.run([seven, "x", "-y", "-o" + (winpath(dest) if native else dest),
                              winpath(archive) if native else archive], capture_output=True, text=True)
        if run.returncode:
            raise SystemExit("7-Zip could not unpack %s: %s" % (archive, run.stdout[-300:]))
    open(mark, "w").write(stamp)
    return dest


def winpath(path):
    return subprocess.run(["wslpath", "-w", os.path.abspath(path)], capture_output=True,
                          text=True, check=True).stdout.strip()


def via_blender(files, key, decimate=None):
    """Files Blender reads (.fbx, .obj, .blend) as one glTF binary, made once; decimate: the
    share of each mesh's triangles kept (Blender's collapse, UVs kept), for a source heavier
    than the engine takes (32 surfaces of 1000 vertices)."""
    h = hashlib.sha1()
    if decimate:
        h.update(b"decimate %g" % decimate)
    for f in files:
        h.update(("%s %d %d" % (f, os.path.getsize(f), int(os.path.getmtime(f)))).encode())
    h.update(open(os.path.join(HERE, "blender_to_glb.py"), "rb").read())
    out = os.path.join(CACHE, key, "blender-%s.glb" % h.hexdigest()[:12])
    if os.path.exists(out):
        return out
    if not os.path.exists(BLENDER) and not shutil.which(BLENDER):
        raise SystemExit("no Blender to read %s (BLENDER)" % files[0])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    native = BLENDER.endswith(".exe")
    conv = (lambda p: winpath(p)) if native else os.path.abspath
    run = subprocess.run([BLENDER, "-b", "--factory-startup", "--python", conv(os.path.join(HERE, "blender_to_glb.py")),
                          "--", conv(out[:-4] + ".tmp.glb")] + (["--decimate=%g" % decimate] if decimate else [])
                         + [conv(f) for f in files], capture_output=True, text=True)
    if run.returncode or not os.path.exists(out[:-4] + ".tmp.glb"):
        raise SystemExit("Blender could not read %s: %s" % (files[0], (run.stdout + run.stderr)[-400:]))
    os.replace(out[:-4] + ".tmp.glb", out)
    return out


def load_source(spec, folder):
    """The spec's source files as Pieces (duplicates dropped)."""
    files = []
    for pattern in spec["source"]:
        if "!" in pattern:                         # archive!member
            archive, member = pattern.split("!", 1)
            got = sorted(glob.glob(os.path.join(folder, archive)))
            if not got:
                raise SystemExit("no %s in %s" % (archive, folder))
            files += sorted(glob.glob(os.path.join(unpack(got[0], spec["key"]), member)))
        else:
            got = sorted(glob.glob(os.path.join(folder, pattern)),
                         key=lambda p: [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", p)])
            files += got
    if not files:
        raise SystemExit("no source model in %s" % folder)
    ext = {os.path.splitext(f)[1].lower() for f in files}
    if ext == {".glb"}:
        pieces = [p for f in files for p in read_glb(f)]
    elif ext == {".dae"}:
        pieces = [p for f in files for p in read_dae(f)]
    else:
        pieces = read_glb(via_blender(files, spec["key"]))
        if ext == {".obj"}:  # Blender names each mesh after its file
            pass
    seen, out = set(), []
    for p in pieces:
        sig = (len(p.tris), tuple(np.round(p.pos.min(0), 3)), tuple(np.round(p.pos.max(0), 3)))
        if sig in seen:
            continue
        seen.add(sig)
        out.append(p)
    for pattern, name in (spec.get("rename") or {}).items():   # {regex: our name for the mesh}
        for p in out:
            if re.match(pattern, p.name):
                if p.material == p.name:   # a glTF mesh named after its material: both renamed
                    p.material = name
                p.name = name
    if spec.get("piece_re"):
        out = [p for p in out if re.fullmatch(spec["piece_re"], p.name)]
    if spec.get("material_re"):
        out = [p for p in out if re.fullmatch(spec["material_re"], p.material)]
    if spec.get("keep_box"):   # connected parts whose centre is in the box (source space)
        lo, hi = np.array(spec["keep_box"], float).T
        kept = []
        for p in out:
            piece = parts(p.pos, p.tris)
            keep = np.zeros(len(p.tris), bool)
            for k in np.unique(piece):
                c = p.pos[np.unique(p.tris[piece == k])].mean(0)
                if (c > lo).all() and (c < hi).all():
                    keep |= piece == k
            if keep.any():
                kept.append(p.subset(keep))
        out = kept
    for box in spec.get("drop") or ():   # connected parts centred in a box (source space) go
        lo, hi = np.array(box, float).T
        kept = []
        for p in out:
            piece = parts(p.pos, p.tris)
            keep = np.ones(len(p.tris), bool)
            for k in np.unique(piece):
                c = p.pos[np.unique(p.tris[piece == k])].mean(0)
                if (c > lo).all() and (c < hi).all():
                    keep &= piece != k
            kept.append(p.subset(keep) if not keep.all() else p)
        out = kept
    keep = spec.get("pieces")
    if keep is not None:
        names = [p.name for p in out]
        missing = [k for k in keep if k not in names]
        if missing:
            raise SystemExit("the source has no %s (it has %s)" % (", ".join(missing), ", ".join(names)))
        out = [p for p in out if p.name in keep]
    for p in out:
        m = spec.get("maps", {})
        for kind, rel in m.get(p.name, m.get(p.material, m.get("*", {}))).items():
            p.maps[kind] = rel if kind in VALUE_MAPS else Image.open(os.path.join(folder, rel))
    return out



# ----------------------------------------------------------------------------- the game's model

class Stock:
    """The game's weapon model posed by its idle animation: its bones' matrices, and its
    surfaces' vertices in model space with their bone and surface."""

    def __init__(self, fs, tik_path):
        text = fs.read_text(tik_path)
        path = re.search(r"^\s*path\s+(\S+)", text, re.M | re.I)
        self.dir = path.group(1) if path else os.path.dirname(tik_path)
        name = re.search(r"^\s*skelmodel\s+(\S+)", text, re.M | re.I).group(1)
        self.skd_path = self.dir + "/" + name
        self.model = skd.read(fs.read(self.skd_path))
        idle = re.search(r"^\s*idle\s+(\S+\.skc)", text, re.M | re.I)
        self.idle_anim = read_anim(fs, self.dir + "/" + idle.group(1)) if idle else None
        self.held = {k: np.array(v) for k, v in pose.Skeleton(self.model.bones).evaluate(
            pose.frame_channels(self.idle_anim, 0)).items()}
        names = [b.name for b in self.model.bones]
        pts, tris, vbone, vsurf = [], [], [], []
        for si, s in enumerate(self.model.surfaces):
            base = len(pts)
            for v in s.verts:
                p = np.zeros(3)
                main = max(v.weights, key=lambda w: w[1])[0]
                for bi, bw, off in v.weights:
                    m = self.held[names[bi].lower()]
                    p += bw * (m[:3, :3] @ np.array(off) + m[:3, 3])
                pts.append(p)
                vbone.append(main)
                vsurf.append(si)
            tris += [(base + a, base + b, base + c) for a, b, c in s.triangles]
        self.pos, self.tris = np.array(pts), np.array(tris, int)
        self.vbone, self.vsurf = np.array(vbone), np.array(vsurf)

    def surface_mask(self, name):
        idx = [i for i, s in enumerate(self.model.surfaces) if s.name.lower() == name.lower()]
        return np.isin(self.vsurf, idx)

    def to_bone(self, bone, pos, normal):
        m = np.linalg.inv(self.held[self.model.bones[bone].name.lower()])
        return pos @ m[:3, :3].T + m[:3, 3], normal @ m[:3, :3].T


# ----------------------------------------------------------------------------- fitting

def turns():
    out = []
    for perm in ((0, 1, 2), (0, 2, 1), (1, 0, 2), (1, 2, 0), (2, 0, 1), (2, 1, 0)):
        for signs in np.array(np.meshgrid([1, -1], [1, -1], [1, -1])).T.reshape(-1, 3):
            r = np.zeros((3, 3))
            for i, p in enumerate(perm):
                r[i, p] = signs[i]
            if np.linalg.det(r) > 0:
                out.append(r)
    return out


def principal_axes(p):
    """Columns: the axes of p's spread, longest first (a rotation)."""
    _, vec = np.linalg.eigh(np.cov((p - p.mean(0)).T))
    vec = vec[:, ::-1]
    if np.linalg.det(vec) < 0:
        vec[:, 2] *= -1
    return vec


def fit(src_pts, dst_pts, scale=True, first=None, label=""):
    """s, r, t with dst ~ s r src + t: ICP from each axis-aligned turn (or the turns in
    first), the closest kept."""
    rng = np.random.default_rng(3)
    a = src_pts[rng.choice(len(src_pts), min(6000, len(src_pts)), replace=False)]
    b = dst_pts[rng.choice(len(dst_pts), min(6000, len(dst_pts)), replace=False)]
    s0 = np.ptp(b, 0).max() / np.ptp(a, 0).max() if scale else 1.0
    best = None
    # the turns, and the turns between the two shapes' principal axes (a source lying at
    # an angle)
    pa, pb = principal_axes(a), principal_axes(b)
    starts = first or turns() + [pb @ t @ pa.T for t in turns()]
    for r0 in starts:
        s, r, t, err = icp(a * s0, b, r0, scale=scale, iters=25)
        if best is None or err < best[3]:
            best = (s * s0, r, t, err, r0)
    s, r, t, err = icp(src_pts * best[0], dst_pts, best[1], scale=scale, iters=40)[:4]
    s *= best[0]
    print("  %sfitted: scale %.4f, %.2f cm apart" % (label, s, err / 2))
    return s, r, t, err / 2


def similarity_matrix(s, r, t):
    m = np.eye(4)
    m[:3, :3] = s * r
    m[:3, 3] = t
    return m


FPS_SKD = "models/player/us_army/USarmyplyr.skd"
AIM_DIR = "models/human/animation/viewmodel/"
EYE_BONE, WEAPON_TAG = "eyes bone", "tag_weapon_right"


def aim_eye(fs, aim_skc):
    """The camera in the weapon model's space while aiming: where the arms' aim pose
    (MOHAIM's aim down the sights) puts the eye (the game puts the camera at the eye bone),
    and the view's forward, up and right. The view looks along the arms model's +X; its
    pitch against the weapons is calibrated (AIM_PITCH) on the two whose sights the player
    approved in game (the Garand, the Thompson)."""
    fps = pose.Skeleton(skd.read(fs.read(FPS_SKD)).bones)
    body = fps.evaluate(pose.frame_channels(read_anim(fs, AIM_DIR + aim_skc), 0))
    inv = np.linalg.inv(np.array(body[WEAPON_TAG]))
    eye = (inv @ np.array(body[EYE_BONE])[:, 3])[:3]
    fwd, left, up = inv[:3, 0], inv[:3, 1], inv[:3, 2]
    c, s = np.cos(AIM_PITCH), np.sin(AIM_PITCH)
    fwd, up = fwd * c + up * s, up * c - fwd * s
    return eye, fwd, up, -left


AIM_PITCH = np.radians(7.75)


def sight_fix(spec, fs):
    """The move (a turn and a shift, 4x4) that puts our rear and front sights on the line
    the camera looks down. spec["sight_at"]: the two as (depth, right, up) in cm in the
    camera's frame, measured on the model as the fit leaves it (REMODEL_NO_SIGHTS=1): a
    notch's bottom, a post's tip, an aperture's or an eyehole's middle."""
    eye, fwd, up, right = aim_eye(fs, spec["aim"])
    if "sight_pts" in spec:    # or the two in the gun's space (cm), as REMODEL_PARTS gives them
        r, f = (np.asarray(q, float) for q in spec["sight_pts"])
    else:
        r, f = (eye + fwd * d + right * x + up * y for d, x, y in spec["sight_at"])
    have = (f - r) / np.linalg.norm(f - r)
    axis = np.cross(have, fwd)
    sn, cs = np.linalg.norm(axis), have @ fwd
    k = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    rot = np.eye(3) + k + k @ k * ((1 - cs) / (sn * sn)) if sn > 1e-12 else np.eye(3)
    # the rear sight as far from the eye as it is, or (eye_relief, cm) brought to the eye: a
    # peep sight close enough to see the whole front sight through it
    new_r = eye + fwd * spec.get("eye_relief", (r - eye) @ fwd)
    # (sight_shift: cm right, up: where the game's camera sits off the eye bone shows at a
    # peep held this close)
    new_r = new_r + right * spec.get("sight_shift", (0, 0))[0] + up * spec.get("sight_shift", (0, 0))[1]
    m = np.eye(4)
    m[:3, :3] = rot
    m[:3, 3] = new_r - rot @ r
    print("  sights: turned %.2f deg, the rear sight moved %.2f cm" % (np.degrees(np.arcsin(min(sn, 1.0))),
                                                                       np.linalg.norm(new_r - r)))
    drop = spec.get("sight_drop", 0.0)
    if drop:    # the butt dropped: a turn about the front sight, the rear sight this far under the line
        fp = m[:3, :3] @ f + m[:3, 3]
        span = (f - r) @ fwd
        ang = np.arctan2(drop, span)
        for sign in (1.0, -1.0):
            k = np.array([[0, -right[2], right[1]], [right[2], 0, -right[0]], [-right[1], right[0], 0]])
            t = sign * ang
            rot2 = np.eye(3) + np.sin(t) * k + (1 - np.cos(t)) * k @ k
            d = np.eye(4)
            d[:3, :3] = rot2
            d[:3, 3] = fp - rot2 @ fp
            rp = (d @ m @ np.append(r, 1.0))[:3]
            if (rp - eye) @ up < 0:
                m = d @ m
                break
        print("  the butt dropped: the rear sight %.2f cm under the line" % drop)
    if spec.get("level"):    # no roll: the gun's up (its -Z) turned onto the view's up, about the line
        g = m[:3, :3] @ np.array([0.0, 0.0, -1.0])
        g = g - fwd * (g @ fwd)
        t = -np.arctan2(g @ right, g @ up)
        k = np.array([[0, -fwd[2], fwd[1]], [fwd[2], 0, -fwd[0]], [-fwd[1], fwd[0], 0]])
        best = None
        for sign in (1.0, -1.0):
            rot2 = np.eye(3) + np.sin(sign * t) * k + (1 - np.cos(sign * t)) * k @ k
            g2 = rot2 @ g
            if best is None or abs(g2 @ right) < best[0]:
                best = (abs(g2 @ right), rot2)
        d = np.eye(4)
        d[:3, :3] = best[1]
        d[:3, 3] = eye - best[1] @ eye
        m = d @ m
        print("  levelled: rolled %.1f deg" % np.degrees(abs(t)))
    if spec.get("roll"):     # and rolled this far right (degrees) about the line: the top to the right
        d = np.eye(4)
        d[:3, :3] = rotation(fwd, spec["roll"])     # about the view's forward, clockwise as seen: its top goes right
        d[:3, 3] = eye - d[:3, :3] @ eye
        m = d @ m
        print("  rolled %.1f deg right" % spec["roll"])
    return m


def resampled(src, frame_time):
    """src at another frame rate: positions linear, rotations normalised-linear between the
    frames around each new one (the per-frame bounds from the nearest)."""
    n = max(2, int(round((src.num_frames - 1) * src.frame_time / frame_time)) + 1)
    a = skc.Animation()
    a.flags, a.frame_time = src.flags, frame_time
    a.total_delta, a.total_angle_delta = src.total_delta, src.total_angle_delta
    a.channels = list(src.channels)
    a.frames, a.values = [], []
    for f in range(n):
        t = min(f * frame_time / src.frame_time, src.num_frames - 1)
        i = min(int(t), src.num_frames - 2) if src.num_frames > 1 else 0
        w = t - i if src.num_frames > 1 else 0.0
        j = min(i + 1, src.num_frames - 1)
        a.frames.append(src.frames[i if w < 0.5 else j])
        row = []
        for c, name in enumerate(src.channels):
            v0, v1 = np.array(src.values[i][c], float), np.array(src.values[j][c], float)
            if name.endswith(" rot"):
                if v0 @ v1 < 0:
                    v1 = -v1
                v = v0 * (1 - w) + v1 * w
                v /= max(np.linalg.norm(v), 1e-9)
            else:
                v = v0 * (1 - w) + v1 * w
            row.append(tuple(float(x) for x in v))
        a.values.append(row)
    return a


def with_slide(src, bone, axis, curve):
    """src with bone's position along axis (its parent's space) by curve[frame] (cm, held
    and linear between keys), its rotation none."""
    a = skc.Animation()
    a.flags, a.frame_time = src.flags, src.frame_time
    a.total_delta, a.total_angle_delta = src.total_delta, src.total_angle_delta
    a.frames = list(src.frames)
    a.channels = list(src.channels)
    for ch in (bone + " pos", bone + " rot"):
        if ch not in a.channels:
            a.channels.append(ch)
    keys = sorted(curve)
    d = np.interp(np.arange(src.num_frames), keys, [curve[k] for k in keys])
    a.values = []
    for f, row in enumerate(src.values):
        row = list(row) + [None] * (len(a.channels) - len(row))
        p = np.asarray(axis) * d[f]
        row[a.channels.index(bone + " pos")] = (float(p[0]), float(p[1]), float(p[2]), 0.0)
        row[a.channels.index(bone + " rot")] = (0.0, 0.0, 0.0, 1.0)
        a.values.append(row)
    return a


def read_written(out, path):
    return open(os.path.join(out, path), "rb").read()


def rotation(axis, degrees):
    a = np.asarray(axis, float) / np.linalg.norm(axis)
    k = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
    t = np.radians(degrees)
    return np.eye(3) + np.sin(t) * k + (1 - np.cos(t)) * k @ k


def with_turn(src, bone, pin, axis, curve):
    """src with bone turned about pin (its parent's space) by curve[frame] degrees about
    axis (held and linear between keys): its position the pin's offset, so it turns in place."""
    a = skc.Animation()
    a.flags, a.frame_time = src.flags, src.frame_time
    a.total_delta, a.total_angle_delta = src.total_delta, src.total_angle_delta
    a.frames = list(src.frames)
    a.channels = list(src.channels)
    for ch in (bone + " pos", bone + " rot"):
        if ch not in a.channels:
            a.channels.append(ch)
    keys = sorted(curve)
    d = np.interp(np.arange(src.num_frames), keys, [curve[k] for k in keys])
    u = np.asarray(axis, float) / np.linalg.norm(axis)
    a.values = []
    for f, row in enumerate(src.values):
        row = list(row) + [None] * (len(a.channels) - len(row))
        r = rotation(u, d[f])
        p = pin - r @ pin
        q = pose.matrix_to_quat(r)
        row[a.channels.index(bone + " pos")] = (float(p[0]), float(p[1]), float(p[2]), 0.0)
        row[a.channels.index(bone + " rot")] = tuple(float(x) for x in q)
        a.values.append(row)
    return a


def mk_moved(p, m):
    q = p.moved(m)
    q.material_key = p.material_key
    return q


FPS_ANIM_LISTS = ["models/player/base/fps_anims_bazooka.txt", "models/player/base/fps_anims_rifle.txt",
                  "models/player/base/fps_anims_smg.txt", "models/player/base/fps_anims_mg.txt",
                  "models/player/base/fps_anims_pistol.txt", "models/player/base/fps_anims_shotgun.txt"]
AIM_POSES = {}    # the arms' aim poses made over, {their file: ours}: fps_anims lists repointed


def aim_pose_moved(fs, out, aim_skc, m):
    """The arms' aim pose changed so that the camera sees the weapon as if the weapon had been
    moved by m (its own space): the whole skeleton turned about the eye (the camera keeps
    the view's axes, so the weapon turns in view), and the eye bone (where the camera is)
    shifted. The weapon itself stays where the game's other poses have it."""
    fps_model = skd.read(fs.read(FPS_SKD))
    sk = pose.Skeleton(fps_model.bones)
    a = read_anim(fs, AIM_DIR + aim_skc)
    ch = {n.lower(): i for i, n in enumerate(a.channels)}
    out_anim = skc.Animation()
    out_anim.flags, out_anim.frame_time = a.flags, a.frame_time
    out_anim.total_delta, out_anim.total_angle_delta = a.total_delta, a.total_angle_delta
    out_anim.frames, out_anim.channels = list(a.frames), list(a.channels)
    out_anim.values = []
    root = next(b.name for b in fps_model.bones if not b.parent)
    for f in range(a.num_frames):
        row = [tuple(v) for v in a.values[f]]
        body = {k: np.array(v) for k, v in sk.evaluate(pose.frame_channels(a, f)).items()}
        t, e = body[WEAPON_TAG], body[EYE_BONE][:3, 3]
        rt = t[:3, :3]
        rg = rt @ m[:3, :3] @ rt.T
        g = np.eye(4)
        g[:3, :3] = rg
        g[:3, 3] = e - rg @ e
        target_eye = rg @ (t[:3, 3] - e) + e - rt @ m[:3, 3] - t[:3, 3] + e
        w = g @ body[root.lower()]
        row[ch[root.lower() + " pos"]] = tuple(w[:3, 3]) + (0.0,)
        row[ch[root.lower() + " rot"]] = tuple(pose.matrix_to_quat(w[:3, :3].tolist()))
        tmp = {n: v for n, v in zip([c.lower() for c in a.channels], row)}
        body2 = {k: np.array(v) for k, v in sk.evaluate(tmp).items()}
        head = body2[next(b.parent for b in fps_model.bones if b.name.lower() == EYE_BONE).lower()]
        local = head[:3, :3].T @ (target_eye - head[:3, 3])
        row[ch[EYE_BONE + " pos"]] = tuple(local) + (0.0,)
        out_anim.values.append(row)
        # check: the weapon as the camera sees it, as if moved by m
        tmp = {n: v for n, v in zip([c.lower() for c in a.channels], row)}
        body3 = {k: np.array(v) for k, v in sk.evaluate(tmp).items()}
        p = np.array([30.0, 5.0, -3.0, 1.0])
        want = (t @ m @ p)[:3] - e
        got = (body3[WEAPON_TAG] @ p)[:3] - body3[EYE_BONE][:3, 3]
        if np.linalg.norm(want - got) > 0.05:
            raise SystemExit("the aim pose could not be made over (%.2f cm off)" % np.linalg.norm(want - got))
    name = "opm_" + aim_skc
    write(out, AIM_DIR + name, skc.write(out_anim))
    AIM_POSES["viewmodel/" + aim_skc] = "viewmodel/" + name
    print("  the arms' aim pose %s made over (the weapon left where the game holds it)" % aim_skc)


def write_aim_lists(fs, out):
    """The arms' animation lists that name an aim pose we made over, pointed at ours."""
    for path in FPS_ANIM_LISTS:
        try:
            text = fs.read_text(path).replace("\r\n", "\n")
        except Exception:
            continue
        new = text
        poses = dict(AIM_POSES)    # and those an earlier run made (a build of some weapons only)
        made = os.path.join(out, "models/human/animation/viewmodel")
        for f in sorted(os.listdir(made)) if os.path.isdir(made) else ():
            if f.startswith("opm_") and f.endswith(".skc"):
                poses.setdefault("viewmodel/" + f[len("opm_"):], "viewmodel/" + f)
        for old, ours in poses.items():
            new = re.sub(r"(?<![\w/])%s(?=\s)" % re.escape(old), ours, new)
        if new != text:
            put_tik(out, path, new)


TORSO_STATES = "global/mike_torso.st"
SHELL_STATE = "RELOAD_SHOTGUN_SINGLE"


def write_shell_reload(fs, out):
    """The shotgun's hand loading each shell. MOHAIM plays the arms' reload_single slot as
    its aim pose, so its torso states load each shell with `viewmodelanim reload`, which
    only holds the start of the reload (the hand never comes in). The shell loading
    (shotgun_reload_single_1) goes in the shotgun's unused rechamber slot instead, played
    from the start for each shell."""
    text = fs.read_text(TORSO_STATES).replace("\r\n", "\n")
    m = re.search(r"state\s+%s\s*\{.*?entrycommands\s*\{(.*?)\}" % SHELL_STATE, text, re.S)
    if not m:
        raise SystemExit("%s has no %s state" % (TORSO_STATES, SHELL_STATE))
    body = re.sub(r"(?m)^(\s*)viewmodelanim\s+reload[ \t]*$", r"\1viewmodelanim rechamber 1", m.group(1))
    if body == m.group(1):
        if "viewmodelanim rechamber" not in body:
            print("  %s: %s plays no `viewmodelanim reload`, left as it is" % (TORSO_STATES, SHELL_STATE))
        return
    put_tik(out, TORSO_STATES, text[:m.start(1)] + body + text[m.end(1):])
    path = "models/player/base/fps_anims_shotgun.txt"
    lists = fs.read_text(path).replace("\r\n", "\n")
    fill = re.search(r"(?m)^\s*shotgun_reload_single_1\s+(\S+)", lists)
    if not fill:
        raise SystemExit("%s has no shotgun_reload_single_1" % path)
    if not re.search(r"(?m)^\s*shotgun_rechamber\s", lists):
        lists = lists[:fill.end()] + lists[fill.end():].split("\n", 1)[0] + \
            "\n\tshotgun_rechamber\t\t%s\tcrossblend 0\n" % fill.group(1) + lists[fill.end():].split("\n", 1)[1]
    put_tik(out, path, lists)


def clip_in_gun(fs, out, stock, spec, skd_name, p, shader, ig):
    """The hand's ammunition (p) also in the gun, where the game's original model had it: the
    Kar98k's stripper clip in the receiver's guides while the thumb pushes the rounds down.
    The game's .tik and reload animation still show and move that clip (surfaces material7
    and material8 on bones Clip01 and Object01), but the model the game loads (MOHAIM's)
    lost it. ig: {original: (pak, its .skd), anim (the .tik's animation), frame (one where
    the clip is in), surfaces [the original's clip surfaces]}; the bones are the spec's
    new_bones. Our clip fitted onto the original's at that frame, each part on the nearer of
    its surfaces and that surface's bone."""
    import zipfile
    orig = skd.read(zipfile.ZipFile(os.path.join(GAME, "main", ig["original"][0])).read(ig["original"][1]))
    text = tik_text(fs, spec["tik"])
    anim = read_anim(fs, stock.dir + "/" + re.search(r"^\s*%s\s+(\S+\.skc)" % re.escape(ig["anim"]), text, re.M).group(1))
    ch = pose.frame_channels(anim, ig["frame"])
    oheld = {k: np.array(v) for k, v in pose.Skeleton(orig.bones).evaluate(ch).items()}
    rng = np.random.default_rng(5)
    tpts, tlab, bones = [], [], []
    for i, name in enumerate(ig["surfaces"]):
        srf = next(x for x in orig.surfaces if x.name == name)
        pts = np.array([sum(w * (oheld[orig.bones[bi].name.lower()][:3, :3] @ np.array(off)
                                 + oheld[orig.bones[bi].name.lower()][:3, 3]) for bi, w, off in v.weights)
                        for v in srf.verts])
        tpts.append(sample(pts, np.array(srf.triangles, int), 4000, rng))
        tlab.append(np.full(4000, i))
        bones.append(orig.bones[srf.verts[0].weights[0][0]].name)
    tpts, tlab = np.concatenate(tpts), np.concatenate(tlab)
    _, r, t, _ = fit(sample(p.pos, p.tris, 6000, rng), tpts, scale=False, label="the clip in the gun: ")
    p = p.moved(similarity_matrix(1.0, r, t))
    trees = [cKDTree(tpts[tlab == i]) for i in range(len(ig["surfaces"]))]
    lab = parts(p.pos, p.tris)
    which = np.zeros(len(p.tris), int)
    for k in np.unique(lab):     # each part on the surface it lies closest to on the whole
        v = p.pos[np.unique(p.tris[lab == k])]
        which[lab == k] = int(np.argmin([tr.query(v)[0].mean() for tr in trees]))
    held = {k: np.array(v) for k, v in pose.Skeleton(stock.model.bones).evaluate(ch).items()}
    path = os.path.join(out, stock.dir, skd_name)
    model = skd.read(open(path, "rb").read())
    lines = []
    for i, name in enumerate(ig["surfaces"]):
        bm = np.linalg.inv(held[bones[i].lower()])
        pos = p.pos @ bm[:3, :3].T + bm[:3, 3]
        nrm = p.normal @ bm[:3, :3].T
        bi = model.bone_index(bones[i])
        pieces = split(name, p.tris[which == i].tolist())
        if len(pieces) != 1:
            raise SystemExit("%s: %s is %d surfaces (its frame commands take one)" % (spec["key"], name, len(pieces)))
        for sname, ptris, order in pieces:
            srf = skd.Surface(sname)
            srf.triangles = [(a, c, b) for a, b, c in ptris]   # the game's front faces wind clockwise
            srf.verts = [skd.Vertex(nrm[j], (p.uv[j][0], p.uv[j][1]), [(bi, 1.0, tuple(pos[j]))]) for j in order]
            model.surfaces.append(srf)
        lines.append("\tsurface %s shader %s" % (name, shader))
        print("  the clip in the gun: %s on %s, %d triangles" % (name, bones[i], (which == i).sum()))
    write(out, "%s/%s" % (stock.dir, skd_name), skd.write(model))
    tik_path = os.path.join(out, spec["tik"])
    t = open(tik_path, "rb").read().decode("latin-1").replace("\r\n", "\n")
    t = re.sub(r"(\bskelmodel\s+\S+\n)", lambda mm: mm.group(1) + "\n".join(lines) + "\n", t, count=1)
    put_tik(out, spec["tik"], t)


def cut_hole(p, h):
    """p with a round hole through its faces across h's axis: h = {centre, axis, radius,
    depth (cm along the axis either side of the centre)}. Each face crossing the circle is
    triangulated again (its corners, where the circle crosses its edges, a 16-sided circle's
    points inside it) and the triangles inside the circle go; positions, normals and UVs are
    the face's, by barycentric weights."""
    from scipy.spatial import Delaunay
    c = np.asarray(h["centre"], float)
    ax = np.asarray(h["axis"], float) / np.linalg.norm(h["axis"])
    r, depth = h["radius"], h["depth"]
    e1 = np.cross(ax, [1.0, 0, 0] if abs(ax[0]) < 0.9 else [0, 1.0, 0])
    e1 /= np.linalg.norm(e1)
    e2 = np.cross(ax, e1)
    flat = lambda x: np.stack([(x - c) @ e1, (x - c) @ e2], -1)
    ring = r * np.stack([np.cos(np.linspace(0, 2 * np.pi, 16, endpoint=False)),
                         np.sin(np.linspace(0, 2 * np.pi, 16, endpoint=False))], 1)
    inside_ring = lambda q: np.linalg.norm(q, axis=-1) < r * np.cos(np.pi / 16)   # the 16-gon's inner circle

    a_, b_, cc_ = (p.pos[p.tris[:, k]] for k in range(3))
    fn = np.cross(b_ - a_, cc_ - a_)
    fn /= np.linalg.norm(fn, axis=1, keepdims=True) + 1e-12
    along = ((a_ + b_ + cc_) / 3 - c) @ ax
    cand = (np.abs(fn @ ax) > 0.9) & (np.abs(along) < depth)
    pos, nrm, uv = list(p.pos), list(p.normal), list(p.uv)
    out, cut = [t for t in p.tris[~cand]], 0
    for t in p.tris[cand]:
        q = flat(p.pos[t])
        # the face's own corners as a 2D triangle; barycentric weights of any point in it
        T = np.array([q[0] - q[2], q[1] - q[2]]).T
        if abs(np.linalg.det(T)) < 1e-12:
            out.append(t)
            continue
        Ti = np.linalg.inv(T)
        bary = lambda x: (lambda l: np.concatenate([l, 1 - l.sum(-1, keepdims=True)], -1))((x - q[2]) @ Ti.T)
        pts = [q[0], q[1], q[2]]
        for i in range(3):                     # where the circle crosses each edge
            p0, d = q[i], q[(i + 1) % 3] - q[i]
            A, B, C = d @ d, 2 * p0 @ d, p0 @ p0 - r * r
            disc = B * B - 4 * A * C
            if disc > 0:
                for s_ in ((-B - np.sqrt(disc)) / (2 * A), (-B + np.sqrt(disc)) / (2 * A)):
                    if 0 < s_ < 1:
                        pts.append(p0 + s_ * d)
        pts += [x for x in ring if (bary(x) > 1e-6).all()]
        if len(pts) == 3:                      # the circle misses it
            out.append(t)
            continue
        pts = np.array(pts)
        tri = Delaunay(pts).simplices
        keep = []
        for k in tri:
            m = pts[k].mean(0)
            if (bary(m) > -1e-9).all() and not inside_ring(m):
                keep.append(k)
        cut += 1
        base = len(pos)
        w = bary(pts)
        for wi in w:
            pos.append(wi @ p.pos[t]); nrm.append(wi @ p.normal[t]); uv.append(wi @ p.uv[t])
        for k in keep:   # the face's winding kept
            k = list(k)
            if np.cross(np.append(pts[k[1]] - pts[k[0]], 0), np.append(pts[k[2]] - pts[k[0]], 0))[2] * \
               np.cross(np.append(q[1] - q[0], 0), np.append(q[2] - q[0], 0))[2] < 0:
                k = [k[0], k[2], k[1]]
            out.append([base + k[0], base + k[1], base + k[2]])
    if not cut:
        return p
    print("  a hole of %.2f cm cut through %d faces of %s" % (r, cut, p.name))
    n = np.array(nrm)
    q = Piece(p.name, np.array(pos), n / (np.linalg.norm(n, axis=1, keepdims=True) + 1e-12), np.array(uv),
              np.array(out, int), p.maps, p.material)
    if hasattr(p, "material_key"):
        q.material_key = p.material_key
    return q


def touch_clusters(p, gap):
    """Each triangle's cluster: connected parts within gap of each other joined (one object
    of a display: a bolt and its firing pin, a clip and its rounds)."""
    lab = parts(p.pos, p.tris)
    ks = np.unique(lab)
    pts = {k: p.pos[np.unique(p.tris[lab == k])] for k in ks}
    trees = {k: cKDTree(x) for k, x in pts.items()}
    rows, cols = [], []
    for i, a in enumerate(ks):
        for j in range(i + 1, len(ks)):
            b = ks[j]
            if (pts[a].min(0) - gap > pts[b].max(0)).any() or (pts[b].min(0) - gap > pts[a].max(0)).any():
                continue
            if np.isfinite(trees[b].query(pts[a], distance_upper_bound=gap)[0]).any():
                rows.append(i)
                cols.append(j)
    c = connected_components(coo_matrix((np.ones(len(rows)), (rows, cols)), shape=(len(ks), len(ks))),
                             directed=False)[1]
    return c[np.searchsorted(ks, lab)]


def pick(pieces, sel):
    """sel: {mesh, box (the source's space; parts centred in it), material?}: those parts of
    that mesh as a Piece."""
    p = next(q for q in pieces if q.name == sel["mesh"]
             and ("material" not in sel or q.material == sel["material"]))
    lo, hi = np.array(sel["box"], float).T
    lab = parts(p.pos, p.tris)
    keep = np.zeros(len(p.tris), bool)
    for k in np.unique(lab):
        m = lab == k
        c = p.pos[np.unique(p.tris[m])].mean(0)
        if (c > lo).all() and (c < hi).all():
            keep |= m
    if not keep.any():
        raise SystemExit("nothing of %s in %s" % (sel["mesh"], sel["box"]))
    out = p.subset(keep)
    out.name = sel.get("name", sel["mesh"] + "_pick")
    out.material = p.material or p.name
    print("  picked %s: %d triangles of %s" % (out.name, len(out.tris), sel["mesh"]))
    return out


def target_points(stock, target):
    """The game's points a placed piece is fitted onto: a surface's, a bone's ("bone:<name>"),
    or those in a box."""
    if isinstance(target, str) and target.startswith("bone:"):
        m = stock.vbone == stock.model.bone_index(target[5:])
    elif isinstance(target, str):
        m = stock.surface_mask(target)
    else:
        lo, hi = np.array(target, float).T
        m = ((stock.pos > lo) & (stock.pos < hi)).all(1)
    tt = stock.tris[m[stock.tris].all(1)]
    if len(tt) >= 4:
        return sample(stock.pos, tt, 8000, np.random.default_rng(4))
    return stock.pos[m]


def drop_strays(pieces, stock, limit):
    """Our connected parts whose every vertex is over limit cm from the game's model:
    bits the source has beside the weapon (a lever off a magazine, a display stand)."""
    tree = cKDTree(np.concatenate([stock.pos, sample(stock.pos, stock.tris, 60000, np.random.default_rng(2))]))
    out = []
    for p in pieces:
        d = tree.query(p.pos)[0]
        piece = parts(p.pos, p.tris)
        keep = np.ones(len(p.tris), bool)
        for k in np.unique(piece):
            sel = piece == k
            if d[np.unique(p.tris[sel])].min() > limit:
                keep &= ~sel
                print("  dropped a part of %s: %d triangles, %.1f cm off" % (
                    p.name, sel.sum(), d[np.unique(p.tris[sel])].min()))
        if keep.any():
            out.append(p.subset(keep) if not keep.all() else p)
    return out


def label_pieces(stock, pos, tris, overrides=(), hulls=(), exclude=()):
    """For each triangle: the game's (bone, surface index) of the nearest of its vertices,
    a connected piece voting as one; overrides [(box, bone name, surface name)] win for
    pieces wholly in their box."""
    names = [s.name.lower() for s in stock.model.surfaces]
    skip = np.isin(stock.vsurf, [names.index(k.lower()) for k in exclude])
    tree = cKDTree(stock.pos[~skip])
    # vertex labels by the nearest of the game's vertices
    _, nv = tree.query(pos)
    vb, vs = stock.vbone[~skip][nv], stock.vsurf[~skip][nv]
    piece = parts(pos, tris)
    bone = np.empty(len(tris), int)
    surf = np.empty(len(tris), int)
    for k in np.unique(piece):
        sel = piece == k
        v = tris[sel].ravel()
        pairs = vb[v] * 1000 + vs[v]
        vals, counts = np.unique(pairs, return_counts=True)
        top = vals[counts.argmax()]
        bone[sel], surf[sel] = top // 1000, top % 1000
    names = [s.name.lower() for s in stock.model.surfaces]
    # parts mostly inside a hull surface's convex hull (a magazine inside the grip) are it
    for surf_name in hulls:
        si = names.index(surf_name.lower())
        hp = stock.pos[stock.vsurf == si]
        c = hp.mean(0)
        hull = Delaunay(c + (hp - c) * 1.1)
        hb = np.bincount(stock.vbone[stock.vsurf == si]).argmax()
        inside = hull.find_simplex(pos) >= 0
        for k in np.unique(piece):
            sel = piece == k
            v = np.unique(tris[sel])
            if inside[v].mean() > 0.6:
                bone[sel], surf[sel] = hb, si
    for box, bone_name, surf_name in overrides:
        lo, hi = np.array(box, float).T
        for k in np.unique(piece):
            sel = piece == k
            c = pos[tris[sel]].reshape(-1, 3)
            if (c.min(0) > lo).all() and (c.max(0) < hi).all():
                bone[sel] = stock.model.bone_index(bone_name)
                surf[sel] = names.index(surf_name.lower())
    return bone, surf


# ----------------------------------------------------------------------------- textures

def write_textures(out, tex_dir, name, maps, size, spec_size):
    """maps: base, normal, rough, metal, ao (PIL images, any missing). Writes <name>.jpg and,
    when there is a normal map, <name>_n.jpg, and <name>_s.png."""
    def chan(img, s, default):
        if img is None:
            return np.full((s, s), default, np.float32)
        return np.asarray(sized(img, s, "L")).astype(np.float32) / 255.0

    if "base" not in maps:
        raise SystemExit("%s has no base colour map" % name)
    if "base_tint" in maps:   # a grey map shading one colour (a source with no colour map)
        grey = np.asarray(maps["base"].convert("L")).astype(np.float32)[..., None] / 255.0
        lo, hi = np.percentile(grey, (2, 98))     # its detail stretched to the full range
        grey = 0.55 + 0.45 * np.clip((grey - lo) / max(hi - lo, 1e-3), 0, 1)
        maps = dict(maps, base=Image.fromarray(np.clip(grey * np.array(maps["base_tint"]) * 255, 0, 255).astype(np.uint8)))
    os.makedirs(os.path.join(out, tex_dir), exist_ok=True)
    base = np.asarray(sized(maps["base"], size, "RGB")).astype(np.float32) / 255.0
    ao = chan(maps.get("ao"), size, 1.0)[..., None]
    diffuse = base * (1.0 - AO_AMOUNT + AO_AMOUNT * ao)
    if "metal_diffuse" in maps:   # a PBR metal takes no diffuse light: its share of it kept
        diffuse *= 1.0 - (1.0 - maps["metal_diffuse"]) * chan(maps.get("metal"), size, 0.0)[..., None]
    Image.fromarray(np.clip(diffuse * 255 + 0.5, 0, 255).astype(np.uint8)).save(
        os.path.join(out, tex_dir, name + ".jpg"), quality=92, subsampling=0)
    if "normal" in maps:
        normal = sized(maps["normal"], size, "RGB")
        if not normal_map_green_down(normal):
            r, g, b = normal.split()
            normal = Image.merge("RGB", (r, g.point(lambda v: 255 - v), b))
            print("  %s: normal map's green turned to point down the image" % name)
        normal.save(os.path.join(out, tex_dir, name + "_n.jpg"), quality=95, subsampling=0)
    sbase = np.asarray(sized(maps["base"], spec_size, "RGB")).astype(np.float32) / 255.0
    metal = chan(maps.get("metal"), spec_size, 0.0)[..., None]
    rough = chan(maps.get("rough"), spec_size, maps.get("rough_value", 0.6))
    sao = chan(maps.get("ao"), spec_size, 1.0)[..., None]
    spec = np.empty((spec_size, spec_size, 4), np.float32)
    share = maps.get("metal_share", METAL_SHARE) * metal
    spec[..., :3] = (0.04 * (1.0 - share) + sbase * share) * (0.5 + 0.5 * sao)
    spec[..., 3] = np.minimum(1.0 - rough, maps.get("max_gloss", MAX_GLOSS))
    Image.fromarray(np.clip(spec * 255 + 0.5, 0, 255).astype(np.uint8), "RGBA").save(
        os.path.join(out, tex_dir, name + "_s.png"), optimize=True)


# ----------------------------------------------------------------------------- tiki

def retarget_tik(text, skd_name, shaders, wild, keep=()):
    """The game's .tik on our model: its skelmodel line, its surface shader lines replaced
    by ours ({surface: shader}) but for the game's surfaces we keep (keep), and its frame
    commands' surface names given _* where our surfaces stand for them (wild; * where a
    kept surface of the game's has the name too). Text with LF line ends."""
    out, done = [], False
    keep = {k.lower() for k in keep}
    for line in text.splitlines():
        words = line.split()
        indent = line[:len(line) - len(line.lstrip())]
        if words[:1] == ["skelmodel"]:
            line = "%sskelmodel %s" % (indent, skd_name)
        elif words[:1] == ["surface"] and len(words) >= 3 and words[2].lower() == "shader":
            if not done:
                out.extend("%ssurface %s shader %s" % (indent, s, sh) for s, sh in shaders.items())
                done = True
            if words[1].lower() not in keep:
                continue
        else:
            def star(m):
                name = m.group(2)
                if name.lower() in wild:
                    return m.group(1) + name + ("*" if name.lower() in keep else "_*")
                return m.group(0)
            line = re.sub(r"(\bsurface\s+)([^\s*]+)(?=\s+[+-])", star, line)
        out.append(line)
    if not done:
        raise SystemExit("no surface lines in the game's .tik")
    return "\n".join(out) + "\n"


def commanded_surfaces(texts):
    """The surface names a .tik's frame commands name (lower case)."""
    out = set()
    for text in texts:
        for line in text.splitlines():
            code = line.split("//")[0]
            for m in re.finditer(r"\bsurface\s+([^\s*]+)\s+[+-]", code):
                out.add(m.group(1).lower())
    return out


def tik_text(fs, path):
    return fs.read_text(path).replace("\r\n", "\n")


def put_tik(out, path, text):
    write(out, path, text.replace("\n", "\r\n").encode("latin-1"))


# ----------------------------------------------------------------------------- building

def build_surfaces(stock, pieces, bone, surf, shader_of):
    """Our triangles grouped by the game's surface and our material, as skd surfaces (each
    vertex in its bone's space) named <game surface>_<material letter>_<n>; the .tik's
    shader lines for them ({"<surface>_<letter>_*": shader}: the engine's setup takes at
    most 24 surface lines, so one a group, a wildcard); and the game's surface names our
    surfaces stand for (frame commands are given <name>_*)."""
    groups = {}
    for pi, p in enumerate(pieces):
        for ti in range(len(p.tris)):
            groups.setdefault((surf[pi][ti], p.material_key, bone[pi][ti]), []).append((pi, ti))
    letters = {k: "abcdefghijklmnopqrstuvwxyz"[i] for i, k in enumerate(sorted(shader_of))}
    out, shaders, count = [], {}, {}
    stock_names = [s.name for s in stock.model.surfaces]
    for (si, mkey, b), members in sorted(groups.items()):
        prefix = "%s_%s_" % (stock_names[si], letters[mkey])
        by_piece = {}
        for pi, ti in members:
            by_piece.setdefault(pi, []).append(ti)
        # the group's pieces as one vertex list, so that the engine's surfaces are packed full
        pos, nrm, uv, tris, base = [], [], [], [], 0
        for pi, tis in by_piece.items():
            p = pieces[pi]
            pos.append(p.pos)
            nrm.append(p.normal)
            uv.append(p.uv)
            tris.append(p.tris[tis] + base)
            base += len(p.pos)
        pos, nrm, uv, tris = np.concatenate(pos), np.concatenate(nrm), np.concatenate(uv), np.concatenate(tris)
        lp, ln = stock.to_bone(b, pos, nrm)
        for _, ptris, order in split(prefix, tris.tolist()):
            n = count.get(prefix, 0)
            count[prefix] = n + 1
            s = skd.Surface("%s%d" % (prefix, n))
            s.triangles = [(a, c, bb) for a, bb, c in ptris]   # the game's front faces wind clockwise
            s.verts = [skd.Vertex(ln[i], (uv[i][0], uv[i][1]), [(int(b), 1.0, tuple(lp[i]))])
                       for i in order]
            out.append(s)
            shaders[prefix + "*"] = shader_of[mkey]
    if len(out) > 32:
        raise SystemExit("%d surfaces: the engine takes 32 (MAX_MODEL_SURFACES)" % len(out))
    if len(shaders) > 24:
        raise SystemExit("%d surface lines: the engine's setup takes 24" % len(shaders))
    used = {stock_names[si].lower() for si, _, _ in groups}
    return out, shaders, used


def remodel(spec, fs, out, folder):
    key = spec["key"]
    print("%s (%s)" % (key, folder))
    stock = Stock(fs, spec["tik"])
    for name, parent in spec.get("new_bones", ()):   # a moving part the game's model has no bone for
        stock.model.bones.append(skd.Bone(name, parent=parent, type=skd.POSROT))
        stock.held[name.lower()] = stock.held[parent.lower()].copy()
    pieces = load_source(spec, folder)
    print("  source: %s" % ", ".join("%s %d" % (p.name, len(p.tris)) for p in pieces))
    if "prep" in spec:
        pieces = spec["prep"](pieces, stock)

    WELD[0] = not spec.get("index_parts", False)
    # pieces taken out of a mesh (a display's bolt, a loose magazine) and placed like a
    # loose mesh; their mesh then goes unless the spec keeps it
    if spec.get("picks"):
        raw = load_source(dict(spec, pieces=None, keep_box=None, drop=None, piece_re=".*", material_re=".*"), folder)
        pieces += [pick(raw, sel) for sel in spec["picks"]]
    pick_scale = {sel.get("name", sel["mesh"] + "_pick"): sel.get("scale", 1.0) for sel in spec.get("picks", ())}
    placed = dict(spec.get("place", {}))
    placed.update({sel.get("name", sel["mesh"] + "_pick"): sel["target"] for sel in spec.get("picks", ())})
    for sel in spec.get("picks", ()):
        if "assign" in sel:
            spec.setdefault("assign", {})[sel.get("name", sel["mesh"] + "_pick")] = sel["assign"]

    # 1. fit (the pieces the spec places onto a part of the game's first, the rest whole)
    whole = [p for p in pieces if p.name not in placed]
    rng = np.random.default_rng(1)
    src_pts = np.concatenate([sample(p.pos, p.tris, max(200, int(40000 * len(p.tris) / sum(len(q.tris) for q in whole))), rng)
                              for p in whole])
    fit_mask = np.ones(len(stock.pos), bool)
    for surf_name in spec.get("fit_without", ()):
        if isinstance(surf_name, str):
            fit_mask &= ~stock.surface_mask(surf_name)
        else:
            lo, hi = np.array(surf_name, float).T
            fit_mask &= ~((stock.pos > lo) & (stock.pos < hi)).all(1)
    tri_keep = fit_mask[stock.tris].all(1)
    dst_pts = sample(stock.pos, stock.tris[tri_keep], 40000, rng)
    if spec.get("fit_scale") == "length":    # the scale from the length, then a rigid fit
        k = np.ptp(dst_pts, 0).max() / np.ptp(src_pts, 0).max()
        s, r, t, err = fit(src_pts * k, dst_pts, scale=False, label="gun (scaled by length) ")
        s *= k
    else:
        s, r, t, err = fit(src_pts, dst_pts, label="gun ")
    if not 0.5 < s * spec.get("expect_scale", 1.0) < 2.0 and "expect_scale" in spec:
        raise SystemExit("%s: the model does not fit the game's (scale %.3f)" % (key, s))
    t = t + np.asarray(spec.get("nudge", (0.0, 0.0, 0.0)), float)
    m = similarity_matrix(s, r, t)
    pieces = [p.moved(m) if p.name not in placed else p for p in pieces]
    for name, target in placed.items():
        p = next(q for q in pieces if q.name == name)
        p = p.moved(similarity_matrix(s * pick_scale.get(name, 1.0), np.eye(3), np.zeros(3)))  # the gun's units
        ps, pr, pt, _ = fit(sample(p.pos, p.tris, 8000, rng), target_points(stock, target),
                            scale=spec.get("place_scale", False),
                            label="%s onto %s: " % (name, target if isinstance(target, str) else "the box"))
        off = next((sel.get("offset", (0, 0, 0)) for sel in spec.get("picks", ())
                    if sel.get("name", sel["mesh"] + "_pick") == name), (0, 0, 0))
        pieces[[q.name for q in pieces].index(name)] = p.moved(similarity_matrix(ps, pr, pt + np.asarray(off, float)))
    if "after_fit" in spec:
        pieces = spec["after_fit"](pieces, stock)
    pieces = drop_strays(pieces, stock, spec.get("stray_cm", 2.0))
    for box in spec.get("remove", ()):   # parts centred in a box of the gun's space (REMODEL_PARTS') go
        lo, hi = np.array(box, float).T
        kept = []
        for p in pieces:
            lab = parts(p.pos, p.tris)
            keep = np.ones(len(p.tris), bool)
            for k in np.unique(lab):
                c = p.pos[np.unique(p.tris[lab == k])].mean(0)
                if (c > lo).all() and (c < hi).all():
                    keep &= lab != k
            if not keep.all():
                print("  removed %d triangles of %s" % ((~keep).sum(), p.name))
            kept.append(p.subset(keep) if not keep.all() else p)
        pieces = kept
    for h in spec.get("holes", ()):    # a hole cut through flat faces (a removed bolt's bore)
        pieces = [cut_hole(p, h) for p in pieces]
    if os.environ.get("REMODEL_PARTS"):          # for writing a spec's boxes
        for p in pieces:
            lab = parts(p.pos, p.tris)
            for k in np.unique(lab):
                if (lab == k).sum() >= int(os.environ["REMODEL_PARTS"]):
                    v = p.pos[np.unique(p.tris[lab == k])]
                    print("  part %s/%d: %d tris, centre %s, %s..%s" % (p.name, k, (lab == k).sum(),
                          np.round(v.mean(0), 1), np.round(v.min(0), 1), np.round(v.max(0), 1)))
    if ("sight_at" in spec or "sight_pts" in spec) and not os.environ.get("REMODEL_NO_SIGHTS"):
        sm = sight_fix(spec, fs)
        if spec.get("aim_pose_fix"):     # the arms' aim pose moves instead of the weapon
            aim_pose_moved(fs, out, spec["aim"], sm)
            sm = None
    else:
        sm = None

    # parts the source has somewhere else than they belong (a slide locked back): moved
    # along the barrel, onto their bone
    shifted = []
    for sh in spec.get("shift", ()):
        barrel = stock.held["tag_barrel"][:3, 0] / np.linalg.norm(stock.held["tag_barrel"][:3, 0])  # out of the muzzle
        lo, hi = np.array(sh["box"], float).T
        for i, p in enumerate(pieces):
            lab = parts(p.pos, p.tris)
            mask = np.zeros(len(p.tris), bool)
            for k in np.unique(lab):
                c = p.pos[np.unique(p.tris[lab == k])].mean(0)
                if (c > lo).all() and (c < hi).all():
                    mask |= lab == k
            if mask.any():
                v = np.unique(p.tris[mask])
                p.pos = p.pos.copy()
                p.pos[v] += barrel * sh["forward"]
                shifted.append((i, mask, sh["assign"]))
                print("  moved %d triangles of %s %.1f cm forward" % (mask.sum(), p.name, sh["forward"]))

    # a part reshaped (a peep sight's ring thinned and its hole widened, to see the whole
    # front sight through it): the vertices within radius of centre scaled about it
    for rs in spec.get("reshape", ()):
        c = np.asarray(rs["centre"], float)
        for p in pieces:
            near = np.linalg.norm(p.pos - c, axis=1) < rs["radius"]
            if near.any():
                k = np.asarray(rs["scale"], float)
                p.pos = p.pos.copy()
                p.pos[near] = c + (p.pos[near] - c) * k
                n = p.normal[near] / k
                p.normal = p.normal.copy()
                p.normal[near] = n / np.linalg.norm(n, axis=1, keepdims=True)
                print("  reshaped %d vertices of %s" % (near.sum(), p.name))

    # parts the source has turned to where they rest (a cocked hammer): turned about a pin
    # (the gun's space; axis, degrees), onto their bone
    for tu in spec.get("turn", ()):
        lo, hi = np.array(tu["box"], float).T
        rot = rotation(tu["axis"], tu["angle"])
        pin = np.array(tu["pivot"], float)
        for i, p in enumerate(pieces):
            lab = parts(p.pos, p.tris)
            mask = np.zeros(len(p.tris), bool)
            for k in np.unique(lab):
                c = p.pos[np.unique(p.tris[lab == k])].mean(0)
                if (c > lo).all() and (c < hi).all():
                    mask |= lab == k
            if mask.any():
                v = np.unique(p.tris[mask])
                p.pos, p.normal = p.pos.copy(), p.normal.copy()
                p.pos[v] = (p.pos[v] - pin) @ rot.T + pin
                p.normal[v] = p.normal[v] @ rot.T
                shifted.append((i, mask, tu["assign"]))
                print("  turned %d triangles of %s %.1f deg" % (mask.sum(), p.name, tu["angle"]))

    # 2. bones and surfaces from the game's model
    overrides = spec.get("overrides", ())
    bones, surfs = [], []
    allpos = np.concatenate([p.pos for p in pieces])
    offs = np.cumsum([0] + [len(p.pos) for p in pieces])
    alltris = np.concatenate([p.tris + o for p, o in zip(pieces, offs)])
    b, sf = label_pieces(stock, allpos, alltris, overrides, spec.get("hulls", ()), spec.get("keep_stock", ()))
    toffs = np.cumsum([0] + [len(p.tris) for p in pieces])
    for i in range(len(pieces)):
        bones.append(b[toffs[i]:toffs[i + 1]])
        surfs.append(sf[toffs[i]:toffs[i + 1]])
    names = [x.name for x in stock.model.surfaces]
    lower = [n.lower() for n in names]
    # the game's surfaces no frame command names (nor the hand's magazine) are one: on each
    # bone, the one of them with the most of ours
    named = commanded_surfaces([tik_text(fs, t) for t in [spec["tik"]] + spec.get("more_tiks", [])])
    named |= {k.lower() for k in spec.get("ammo", {}).values()}
    named |= {v[1].lower() for v in spec.get("assign", {}).values()}
    free = np.array([n not in named for n in lower])
    for bi in np.unique(np.concatenate(bones)):
        sel = [(b == bi) & free[sf] for b, sf in zip(bones, surfs)]
        counts = np.bincount(np.concatenate([sf[m] for sf, m in zip(surfs, sel)]), minlength=len(names))
        if counts.sum():
            for sf, m in zip(surfs, sel):
                sf[m] = counts.argmax()
    for i, mask, (bone_name, surf_name) in shifted:
        bones[i][mask] = stock.model.bone_index(bone_name)
        surfs[i][mask] = lower.index(surf_name.lower())
    for bone_name, box in spec.get("confine", {}).items():
        bi = stock.model.bone_index(bone_name)
        lo, hi = np.array(box, float).T
        for i, p in enumerate(pieces):
            piece = parts(p.pos, p.tris)
            for k in np.unique(piece):
                sel = piece == k
                if not (bones[i][sel] == bi).any():
                    continue
                c = p.pos[np.unique(p.tris[sel])].mean(0)
                inside = (c > lo).all() and (c < hi).all()
                if not inside:
                    other = np.concatenate([b[b != bi] for b in bones])
                    ob = np.bincount(other).argmax()
                    osurf = np.bincount(np.concatenate([sf[b == ob] for b, sf in zip(bones, surfs)])).argmax()
                    bones[i][sel], surfs[i][sel] = ob, osurf
        for i, p in enumerate(pieces):   # and the parts in the box onto it
            piece = parts(p.pos, p.tris)
            for k in np.unique(piece):
                sel = piece == k
                c = p.pos[np.unique(p.tris[sel])]
                if (c.min(0) > lo).all() and (c.max(0) < hi).all() and not (bones[i][sel] == bi).all():
                    keep = {k.lower() for k in spec.get("keep_stock", ())}
                    own = [si for si in stock.vsurf[stock.vbone == bi] if lower[si] not in keep]
                    bones[i][sel], surfs[i][sel] = bi, np.bincount(own).argmax()
    # bones whose parts the source welded into a bigger one: triangle by triangle, those
    # whose nearest of the game's vertices (within near_cm) is on the bone, in its box
    for bone_name, box in spec.get("carve", {}).items():
        bi = stock.model.bone_index(bone_name)
        lo, hi = np.array(box, float).T
        on = stock.vbone == bi
        tree = cKDTree(stock.pos)
        keep = {k.lower() for k in spec.get("keep_stock", ())}
        own = [si for si in stock.vsurf[on] if lower[si] not in keep]
        for i, p in enumerate(pieces):
            c = p.pos[p.tris].mean(1)
            d, nv = tree.query(c)
            sel = on[nv] & (d < spec.get("near_cm", 1.0)) & ((c > lo) & (c < hi)).all(1)
            bones[i][sel], surfs[i][sel] = bi, np.bincount(own).argmax()
    for pattern, (bone_name, surf_name) in spec.get("assign", {}).items():
        for i, p in enumerate(pieces):
            if re.fullmatch(pattern, p.name):
                bones[i][:] = stock.model.bone_index(bone_name)
                surfs[i][:] = lower.index(surf_name.lower())
    if sm is not None:    # the sights' move, once the parts have their bones (from the fit)
        pieces = [mk_moved(p, sm) if hasattr(p, "material_key") else p.moved(sm) for p in pieces]
    summary = {}
    for bb, ss in zip(bones, surfs):
        for x, y in zip(bb, ss):
            k = "%s/%s" % (stock.model.bones[x].name, names[y])
            summary[k] = summary.get(k, 0) + 1
    print("  cut: %s" % ", ".join("%s %d" % kv for kv in sorted(summary.items())))

    # materials: one shader each
    tex_dir = "textures/models/weapons/opm_%s" % key
    shader_of, mats = {}, {}
    for p in pieces:
        mk = spec.get("material_of", {}).get(p.name, spec.get("material_of", {}).get(p.material, p.material or p.name))
        p.material_key = mk
        if p.maps:
            mats.setdefault(mk, p.maps)
    shader_text = "// %s remade (tools/weapons/remodel.py)\n" % key
    for i, (mk, maps) in enumerate(sorted(mats.items())):
        tex = "m%d" % i
        shader = "opm_%s_%s" % (key, tex)
        shader_of[mk] = shader
        size = spec.get("tex_size", 2048)
        write_textures(out, tex_dir, tex, maps, size, max(512, size // 2))
        cull = "\tcull none\n" if spec.get("two_sided") else ""   # thin parts seen from behind
        shader_text += ("%s\n{\n\tqer_editorimage %s/%s.tga\n%s\t{\n\t\tmap %s/%s.tga\n"
                        "\t\trgbGen lightingSpherical\n\t}\n}\n" % (shader, tex_dir, tex, cull, tex_dir, tex))
    write(out, "scripts/opm_%s.shader" % key, shader_text.replace("\n", "\r\n").encode("latin-1"))

    gun_surfs, shaders, wild = build_surfaces(stock, pieces, bones, surfs, shader_of)
    kept = [x for x in stock.model.surfaces if x.name.lower() in {k.lower() for k in spec.get("keep_stock", ())}]
    gun_surfs += kept
    skd_name = "opm_%s.skd" % key
    model = model_like(stock.model, skd_name, gun_surfs)
    write(out, "%s/%s" % (stock.dir, skd_name), skd.write(model))
    print("  %s: %d surfaces, %d vertices, %d triangles" % (
        skd_name, len(gun_surfs), sum(len(x.verts) for x in gun_surfs), sum(len(x.triangles) for x in gun_surfs)))

    # a moving part the game's animations leave still (the Hi-Standard's slide): those
    # animations copied with it added, back along the barrel and home again
    renamed = {}
    if "slide" in spec:
        sl = spec["slide"]
        bone = sl["bone"]
        parent = next(b.parent for b in stock.model.bones if b.name == bone)
        pm = stock.held[parent.lower()]
        muzzle = stock.held["tag_barrel"][:3, 3] - stock.held[bone.lower()][:3, 3]
        back = -(pm[:3, :3].T @ muzzle) / np.linalg.norm(muzzle)
        text0 = tik_text(fs, spec["tik"])
        for anim, curve in sl["anims"].items():
            m = re.search(r"^\s*%s\s+(\S+\.skc)" % re.escape(anim), text0, re.M)
            src = resampled(read_anim(fs, stock.dir + "/" + m.group(1)), sl.get("frame_time", 0.05))
            name = "opm_%s_%s.skc" % (key, anim)
            write(out, "%s/%s" % (stock.dir, name),
                  skc.write(with_slide(src, bone, back, {k: v * sl["travel"] for k, v in curve.items()})))
            renamed[anim] = name
        print("  %s slides %.1f cm in %s" % (bone, sl["travel"], ", ".join(sl["anims"])))
    for tu in spec.get("turn", ()):   # and back to where the source had it in these animations
        if "anims" not in tu:
            continue
        bone = tu["assign"][0]
        parent = next(b.parent for b in stock.model.bones if b.name == bone)
        pm = stock.held[parent.lower()]
        pin = (np.linalg.inv(pm) @ np.append(np.asarray(tu["pivot"], float), 1.0))[:3]
        axis = pm[:3, :3].T @ np.asarray(tu["axis"], float)
        text0 = tik_text(fs, spec["tik"])
        for anim, curve in tu["anims"].items():
            if anim in renamed:
                src = skc.read(read_written(out, "%s/%s" % (stock.dir, renamed[anim])))
            else:
                m = re.search(r"^\s*%s\s+(\S+\.skc)" % re.escape(anim), text0, re.M)
                src = resampled(read_anim(fs, stock.dir + "/" + m.group(1)), tu.get("frame_time", 0.05))
            name = "opm_%s_%s.skc" % (key, anim)
            write(out, "%s/%s" % (stock.dir, name),
                  skc.write(with_turn(src, bone, pin, axis, {k: -v * tu["angle"] for k, v in curve.items()})))
            renamed[anim] = name
        print("  %s turns %.1f deg in %s" % (bone, -tu["angle"], ", ".join(tu["anims"])))

    for tik in [spec["tik"]] + spec.get("more_tiks", []):
        text = retarget_tik(tik_text(fs, tik), skd_name, shaders, wild, [x.name for x in kept])
        for anim, name in renamed.items():
            text = re.sub(r"^(\s*%s\s+)\S+\.skc" % re.escape(anim), lambda mm: mm.group(1) + name, text, flags=re.M)
        if "tik_edit" in spec:
            text = spec["tik_edit"](text, tik)
        put_tik(out, tik, text)

    # the game's own models drawn two-sided (ammo_two_sided: {tik: (its shader, its image)}):
    # the Hi-Standard's hand magazine has faces wound inward, holes in it
    for tik, (shader, image) in spec.get("ammo_two_sided", {}).items():
        name = "opm_%s_2s" % shader
        put_tik(out, tik, re.sub(r"(?m)^(\s*surface\s+\S+\s+shader\s+)%s\b" % re.escape(shader),
                                 lambda mm: mm.group(1) + name, tik_text(fs, tik)))
        write(out, "scripts/opm_%s_2s.shader" % key,
              ("// %s's %s drawn two-sided (tools/weapons/remodel.py)\n%s\n{\n\tcull none\n"
               "\tqer_editorimage %s\n\t{\n\t\tmap %s\n\t\trgbGen lightingSpherical\n\t}\n}\n"
               % (tik, shader, name, image, image)).replace("\n", "\r\n").encode("latin-1"))
        print("  %s: drawn two-sided" % tik)

    # 3. the hand's ammunition taken from the source (a stripper clip, a round), fitted onto
    # the game's model of it at the gun's scale
    src_all = None
    for ammo_tik, sel in spec.get("ammo_picks", {}).items():
        if src_all is None:
            src_all = load_source(dict(spec, pieces=None, piece_re=".*"), folder)
        ammo = Stock(fs, ammo_tik)
        p = pick(src_all, sel).moved(similarity_matrix(s * sel.get("scale", 1.0), np.eye(3), np.zeros(3)))
        _, ar, at, _ = fit(sample(p.pos, p.tris, 6000, rng), sample(ammo.pos, ammo.tris, 6000, rng),
                           scale=False, label="%s onto %s: " % (p.name, ammo_tik))
        p = p.moved(similarity_matrix(1.0, ar, at))
        mk = spec.get("material_of", {}).get(p.material, p.material)
        p.material_key = mk
        if mk not in shader_of:
            raise SystemExit("%s: the ammunition's material %s has no shader" % (key, mk))
        abone = int(np.bincount(ammo.vbone).argmax())
        # each part on the game's surface nearest it (a stripper clip's strip and its rounds:
        # the reload shows the strip again once the rounds are in)
        # (by points spread evenly over its surfaces: a thin strip has few vertices)
        tsurf = ammo.vsurf[ammo.tris[:, 0]]
        spread, which = [], []
        for si in np.unique(tsurf):
            spread.append(sample(ammo.pos, ammo.tris[tsurf == si], 3000, rng))
            which.append(np.full(3000, si))
        near = np.concatenate(which)[cKDTree(np.concatenate(spread)).query(p.pos)[1]]
        lab = parts(p.pos, p.tris)
        asurf = np.zeros(len(p.tris), int)
        for k in np.unique(lab):
            asurf[lab == k] = np.bincount(near[np.unique(p.tris[lab == k])]).argmax()
        print("  %s: %s" % (ammo_tik, ", ".join("%s %d" % (ammo.model.surfaces[i].name, (asurf == i).sum())
                                                for i in np.unique(asurf))))
        asurfs, ashaders, awild = build_surfaces(ammo, [p], [np.full(len(p.tris), abone)],
                                                 [asurf], shader_of)
        aname = "opm_%s_%s" % (key, os.path.splitext(os.path.basename(ammo.skd_path))[0].lower())
        write(out, "%s/%s.skd" % (ammo.dir, aname), skd.write(model_like(ammo.model, aname + ".skd", asurfs)))
        put_tik(out, ammo_tik, retarget_tik(tik_text(fs, ammo_tik), aname + ".skd", ashaders, awild))
        if "in_gun" in sel:
            clip_in_gun(fs, out, stock, spec, skd_name, p, shader_of[mk], sel["in_gun"])

    # the hand's magazines: the new magazine moved as the game's is onto the hand's model
    for ammo_tik, gun_surface in spec.get("ammo", {}).items():
        ammo = Stock(fs, ammo_tik)
        gs = stock.surface_mask(gun_surface)
        gt = stock.tris[gs[stock.tris].all(1)]
        if not len(gt):
            raise SystemExit("%s: the game's model has no surface %s" % (key, gun_surface))
        _, ar, at, aerr = fit(sample(stock.pos, gt, 6000, rng), sample(ammo.pos, ammo.tris, 6000, rng),
                              scale=False, label="%s's magazine onto %s: " % (key, ammo_tik))
        am = similarity_matrix(1.0, ar, at)
        target = names.index(next(n for n in names if n.lower() == gun_surface.lower()))
        mag = []
        for p, ss in zip(pieces, surfs):
            sel = ss == target
            if sel.any():
                mp = p.subset(sel).moved(am)
                mp.material_key = p.material_key
                mag.append(mp)
        if not mag:
            print("  nothing of the new model is its magazine (%s): %s stays the game's" % (gun_surface, ammo_tik))
            continue
        if spec.get("ammo_fit") == "hand":   # onto the hand's model itself (ours sits elsewhere in the gun)
            mp = np.concatenate([sample(p.pos, p.tris, 3000, rng) for p in mag])
            _, hr, ht, _ = fit(mp, sample(ammo.pos, ammo.tris, 6000, rng), scale=False,
                               label="%s's magazine onto the hand's: " % key)
            mag = [mk_moved(p, similarity_matrix(1.0, hr, ht)) for p in mag]
        abone = int(np.bincount(ammo.vbone).argmax())
        ab = [np.full(len(p.tris), abone) for p in mag]
        asf = [np.zeros(len(p.tris), int) for p in mag]
        asurfs, ashaders, awild = build_surfaces(ammo, mag, ab, asf, shader_of)
        aname = "opm_%s_%s" % (key, os.path.splitext(os.path.basename(ammo.skd_path))[0].lower())
        write(out, "%s/%s.skd" % (ammo.dir, aname), skd.write(model_like(ammo.model, aname + ".skd", asurfs)))
        put_tik(out, ammo_tik, retarget_tik(tik_text(fs, ammo_tik), aname + ".skd", ashaders, awild))


def remodel_all(spec, fs, out, folder):
    remodel(spec, fs, out, folder)
    for i, extra in enumerate(spec.get("also", ())):
        sub = dict(spec)
        sub.pop("also")
        sub.pop("ammo", None)
        sub.update(extra)
        sub.setdefault("key", "%s%d" % (spec["key"], i + 2))
        remodel(sub, fs, out, folder)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--game", default=GAME)
    ap.add_argument("--only", help="weapons by key, comma separated")
    args = ap.parse_args()
    from weapon_specs import WEAPONS
    fs = vfs.GameFS([os.path.join(args.game, "main")], skip_paks=OUR_PAKS)
    only = set(args.only.split(",")) if args.only else None
    made, failed = [], []
    for spec in WEAPONS:
        if only and spec["key"] not in only:
            continue
        folder = os.path.join(args.game, "models", spec["folder"])
        if not os.path.isdir(folder):
            print("%s: no %s, the game keeps its own" % (spec["key"], folder))
            continue
        try:
            remodel_all(spec, fs, args.out, folder)
            made.append(spec["key"])
        except SystemExit as e:
            print("%s: %s" % (spec["key"], e))
            failed.append(spec["key"])
    write_aim_lists(fs, args.out)
    if "shotgun" in made:
        write_shell_reload(fs, args.out)
    print("made %s%s" % (", ".join(made) or "nothing", "; failed %s" % ", ".join(failed) if failed else ""))
    if failed and only:
        sys.exit(1)


if __name__ == "__main__":
    main()
