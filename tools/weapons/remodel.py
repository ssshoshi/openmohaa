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

import numpy as np
from PIL import Image
from scipy.spatial import Delaunay, cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from thompson import (icp, model_like, parts, pose, read_anim, sample, skc, skd,  # noqa: E402
                      split, vertex_normals, vfs, write)
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
    and its embedded maps."""
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
            raw, _ = view(j["images"][k]["bufferView"])
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
        vmap = {i.get("semantic"): i.get("source")[1:] for i in vin.iterfind("c:input", ns)}
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


def via_blender(files, key):
    """Files Blender reads (.fbx, .obj, .blend) as one glTF binary, made once."""
    h = hashlib.sha1()
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
                          "--", conv(out[:-4] + ".tmp.glb")] + [conv(f) for f in files], capture_output=True, text=True)
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
    if "piece_re" in spec:
        out = [p for p in out if re.fullmatch(spec["piece_re"], p.name)]
    if "material_re" in spec:
        out = [p for p in out if re.fullmatch(spec["material_re"], p.material)]
    if "keep_box" in spec:   # connected parts whose centre is in the box (source space)
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
            p.maps[kind] = Image.open(os.path.join(folder, rel))
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
    """Where the arms' aim pose (MOHAIM's aim down the sights) puts the eye, in the weapon
    model's space (the weapon's origin is on the right hand's tag)."""
    fps = pose.Skeleton(skd.read(fs.read(FPS_SKD)).bones)
    body = fps.evaluate(pose.frame_channels(read_anim(fs, AIM_DIR + aim_skc), 0))
    return (np.linalg.inv(np.array(body[WEAPON_TAG])) @ np.array(body[EYE_BONE])[:, 3])[:3]


def sight_frame(eye, target):
    """Unit vectors: along the line from the eye to target, up (the model's -Z, as near as
    square to it) and across."""
    d = (target - eye) / np.linalg.norm(target - eye)
    up = np.array([0.0, 0.0, -1.0])
    up = up - d * (up @ d)
    up /= np.linalg.norm(up)
    return d, up, np.cross(d, up)


def upper_hull(t, u):
    """The upper convex hull of the points (t, u), in t order: indices."""
    order = np.lexsort((u, t))
    hull = []
    for i in order:
        while len(hull) >= 2:
            a, b = hull[-2], hull[-1]
            if (t[b] - t[a]) * (u[i] - u[a]) - (u[b] - u[a]) * (t[i] - t[a]) >= 0:
                hull.pop()
            else:
                break
        hull.append(i)
    return hull


def sight_pair(q, spec):
    """q: (t, u, v) points, v from the gun's middle. The rear and front sights' tops: the
    edge of the centre column's upper hull that ends at the front sight (the highest hull
    point near the muzzle); what is between them is under it, as sights are made."""
    c = q[(np.abs(q[:, 2]) < spec.get("column", 0.3)) & (q[:, 0] > spec.get("rear_from", 4.0))]
    step = 0.25
    bins = np.floor(c[:, 0] / step).astype(int)
    top = {}
    for i, b in enumerate(bins):
        if b not in top or c[i, 1] > c[top[b], 1]:
            top[b] = i
    prof = c[sorted(top.values(), key=lambda i: c[i, 0])]
    hull = upper_hull(prof[:, 0], prof[:, 1])
    length = prof[:, 0].max()
    front_zone = [k for k, i in enumerate(hull) if prof[i, 0] > length * (1 - spec.get("front_zone", 0.15))]
    if not front_zone:
        return None, None
    fk = max(front_zone, key=lambda k: prof[hull[k], 1])
    f = prof[hull[fk]]
    if "rear_t" in spec:     # where the rear sight is, from the eye (cm): its top there
        lo, hi = spec["rear_t"]
        zone = prof[(prof[:, 0] > lo) & (prof[:, 0] < hi)]
        return (zone[np.argmax(zone[:, 1])] if len(zone) else None), f
    # back along the hull to the rear sight: past the front sight's own top, then on while
    # the hull stays as good as straight (a sight's leaf or base runs on from its notch)
    rk = fk - 1
    while rk > 0 and f[0] - prof[hull[rk], 0] < spec.get("min_span", 15.0):
        rk -= 1
    while rk > 0:
        a, b = prof[hull[rk - 1]], prof[hull[rk]]
        s1 = (f[1] - b[1]) / max(f[0] - b[0], 1e-6)
        s2 = (b[1] - a[1]) / max(b[0] - a[0], 1e-6)
        if abs(np.arctan(s2) - np.arctan(s1)) > np.radians(spec.get("straight_deg", 1.0)):
            break
        rk -= 1
    if rk < 0 or f[0] - prof[hull[rk], 0] < spec.get("min_span", 15.0):
        return None, f
    return prof[hull[rk]], f


def sight_fix(spec, fs, stock, pieces, rng):
    """Our sights onto the line from the aim pose's eye toward the game gun's front sight:
    the move (a turn and a shift) as a 4x4."""
    eye = aim_eye(fs, spec["aim"])
    sp = sample(stock.pos, stock.tris, 80000, rng)
    ours = np.concatenate([sample(p.pos, p.tris, max(4000, len(p.tris) * 8), rng) for p in pieces])
    muzzle = sp[np.argmax(sp[:, 0])]
    d0, up0, ac0 = sight_frame(eye, muzzle)

    def tuv(p, d, up, ac):
        rel = p - eye
        q = np.stack([rel @ d, rel @ up, rel @ ac], 1)
        upper = q[q[:, 1] > np.percentile(q[:, 1], 80)]
        mid = np.median(upper[:, 2])
        q[:, 2] -= mid
        return q, mid

    q, smid = tuv(sp, d0, up0, ac0)
    _, sf = sight_pair(q, spec)
    if sf is None:
        raise SystemExit("no front sight on the game's model")
    target = eye + d0 * sf[0] + up0 * sf[1] + ac0 * smid
    d, up, ac = sight_frame(eye, target)
    q, mid = tuv(ours, d, up, ac)
    r, f = sight_pair(q, spec)
    if r is None:
        raise SystemExit("no sights found along the aim line")
    slope = np.arctan2(f[1] - r[1], f[0] - r[0])
    k = np.array([[0, -ac[2], ac[1]], [ac[2], 0, -ac[0]], [-ac[1], ac[0], 0]])
    rot = np.eye(3) + np.sin(-slope) * k + (1 - np.cos(-slope)) * k @ k   # r->f onto d
    rp = eye + d * r[0] + up * r[1] + ac * mid
    m = np.eye(4)
    m[:3, :3] = rot
    m[:3, 3] = (eye + d * (r[0] * np.cos(slope) + r[1] * np.sin(slope))) - rot @ rp
    print("  sights: rear %.2f cm off the line at %.1f cm, front %.2f cm at %.1f cm, %.2f cm "
          "across; turned %.2f deg" % (r[1], r[0], f[1], f[0], mid, np.degrees(slope)))
    if abs(np.degrees(slope)) > spec.get("max_turn", 4.0) or abs(r[1]) > 4.0:
        raise SystemExit("the sights would move too far (%.1f deg, %.1f cm): check the sight zones"
                         % (np.degrees(slope), r[1]))
    return m


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
    os.makedirs(os.path.join(out, tex_dir), exist_ok=True)
    base = np.asarray(sized(maps["base"], size, "RGB")).astype(np.float32) / 255.0
    ao = chan(maps.get("ao"), size, 1.0)[..., None]
    diffuse = base * (1.0 - AO_AMOUNT + AO_AMOUNT * ao)
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
    rough = chan(maps.get("rough"), spec_size, 0.6)
    sao = chan(maps.get("ao"), spec_size, 1.0)[..., None]
    spec = np.empty((spec_size, spec_size, 4), np.float32)
    share = METAL_SHARE * metal
    spec[..., :3] = (0.04 * (1.0 - share) + sbase * share) * (0.5 + 0.5 * sao)
    spec[..., 3] = np.minimum(1.0 - rough, MAX_GLOSS)
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
    pieces = load_source(spec, folder)
    print("  source: %s" % ", ".join("%s %d" % (p.name, len(p.tris)) for p in pieces))
    if "prep" in spec:
        pieces = spec["prep"](pieces, stock)

    # 1. fit (the pieces the spec places onto a part of the game's first, the rest whole)
    placed = spec.get("place", {})
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
    for name, surf_name in placed.items():
        p = next(q for q in pieces if q.name == name)
        if isinstance(surf_name, str):
            target = stock.surface_mask(surf_name)
        else:                      # a box in the game's model space
            lo, hi = np.array(surf_name, float).T
            target = ((stock.pos > lo) & (stock.pos < hi)).all(1)
            surf_name = "the box"
        tt = stock.tris[target[stock.tris].all(1)]
        p = p.moved(similarity_matrix(s, np.eye(3), np.zeros(3)))   # the gun's scale: same units
        ps, pr, pt, _ = fit(sample(p.pos, p.tris, 8000, rng), sample(stock.pos, tt, 8000, rng),
                            scale=spec.get("place_scale", False), label="%s onto %s: " % (name, surf_name))
        pieces[[q.name for q in pieces].index(name)] = p.moved(similarity_matrix(ps, pr, pt))
    if "after_fit" in spec:
        pieces = spec["after_fit"](pieces, stock)
    pieces = drop_strays(pieces, stock, spec.get("stray_cm", 2.0))
    if "aim" in spec and not os.environ.get("REMODEL_NO_SIGHTS"):
        sm = sight_fix(spec, fs, stock, pieces, rng)
        pieces = [p.moved(sm) for p in pieces]

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
        mk = spec.get("material_of", {}).get(p.name, p.material or p.name)
        p.material_key = mk
        mats.setdefault(mk, p.maps)
    shader_text = "// %s remade (tools/weapons/remodel.py)\n" % key
    for i, (mk, maps) in enumerate(sorted(mats.items())):
        tex = "m%d" % i
        shader = "opm_%s_%s" % (key, tex)
        shader_of[mk] = shader
        size = spec.get("tex_size", 2048)
        write_textures(out, tex_dir, tex, maps, size, max(512, size // 2))
        shader_text += ("%s\n{\n\tqer_editorimage %s/%s.tga\n\t{\n\t\tmap %s/%s.tga\n"
                        "\t\trgbGen lightingSpherical\n\t}\n}\n" % (shader, tex_dir, tex, tex_dir, tex))
    write(out, "scripts/opm_%s.shader" % key, shader_text.replace("\n", "\r\n").encode("latin-1"))

    gun_surfs, shaders, wild = build_surfaces(stock, pieces, bones, surfs, shader_of)
    kept = [x for x in stock.model.surfaces if x.name.lower() in {k.lower() for k in spec.get("keep_stock", ())}]
    gun_surfs += kept
    skd_name = "opm_%s.skd" % key
    model = model_like(stock.model, skd_name, gun_surfs)
    write(out, "%s/%s" % (stock.dir, skd_name), skd.write(model))
    print("  %s: %d surfaces, %d vertices, %d triangles" % (
        skd_name, len(gun_surfs), sum(len(x.verts) for x in gun_surfs), sum(len(x.triangles) for x in gun_surfs)))

    for tik in [spec["tik"]] + spec.get("more_tiks", []):
        text = retarget_tik(tik_text(fs, tik), skd_name, shaders, wild, [x.name for x in kept])
        if "tik_edit" in spec:
            text = spec["tik_edit"](text, tik)
        put_tik(out, tik, text)

    # 3. the hand's magazines: the new magazine moved as the game's is onto the hand's model
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
    print("made %s%s" % (", ".join(made) or "nothing", "; failed %s" % ", ".join(failed) if failed else ""))
    if failed and only:
        sys.exit(1)


if __name__ == "__main__":
    main()
