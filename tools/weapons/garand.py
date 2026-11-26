#!/usr/bin/env python3
"""The M1 Garand remade from a model of the player's own (opm-garand's generator).

The source is one glTF binary, read in place and never committed:

    <MOHAA_GAME>/models/Garand/*.glb

(--src to read it from elsewhere): the rifle ("M1_Garand", one unrigged mesh) and an en-bloc
clip of eight rounds ("Bullets"), each with its PBR maps (base colour, packed occlusion /
roughness / metalness, normal). The game's Garand already moves: its bolt and operating rod
are on a bone of their own (m1garandchamber), which the animations slide back on each shot
and leave back when the rifle is empty, and its clip on another (gun clip), which the reload
pushes down into the receiver. The new rifle keeps that skeleton and its timing. It is

  1. fitted onto the game's rifle (models/weapons/m1_garand/garand.skd, the copy the game
     loads) by a scaled ICP, as the Thompson is (thompson.py), so the arms' animations, the
     aim pose and the tags carry it unchanged;
  2. cut: the bolt and operating rod (the pieces in MOVING_BOX) go on the chamber bone as the
     surface gun3, the rest is gun1, both in pieces the engine takes;
  3. sighted: its rear aperture and front post turned onto the line MOHAIM's aim pose looks
     down, as the Thompson's are;
  4. loaded: the clip seated in the receiver under the bolt, on the clip bone, as the surface
     material8 the reload shows; the same clip is the one the hand brings in
     (models/ammo/garand_clip_reload.tik) and, without its rounds, the one the last shot
     throws out (models/ammo/garand_clip_empty.tik);
  5. animated: the game's animations with the chamber's channel replaced, the same timing
     along our receiver's line, not as far back (BOLT_TRAVEL) but while the clip goes in;
  6. textured: base colour (with a little of the occlusion), the normal map as it is (its
     green is down the image, as GL2's tangent space has it: checked from the map itself,
     see normal_map_green_down), and GL2's specular map made as the Thompson's is;
  7. the .tik files the game loads, pointed at them. The game hides the loaded clip except
     while it goes in; this rifle shows it while there are rounds (the receiver is open on
     top, the clip is seen through it), hides it when the last shot throws it out, and the
     reload shows the new one from when the hand lets it go.

    tools/weapons/garand.py --out build/opm-garand
"""

import argparse
import glob
import io
import json
import os
import re
import struct
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from thompson import (add_animations, icp, model_like, parts, pieces_in, pose,  # noqa: E402
                      read_anim, sample, skc, skd, split, surface_points, vfs,
                      write)

GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")
PAK = "zzzzzzzzzzzzzzzz-opm-garand.pk3"  # what this builds: never read back from

GUN_SKD = "models/weapons/m1_garand/garand.skd"
GUN_TIK = "models/weapons/m1_garand.tik"
GUN_DIR = "models/weapons/m1_garand"
RELOAD_CLIP_SKD = "models/AMMO/GARANDCLIP/garand_clipreload.skd"
RELOAD_CLIP_TIK = "models/ammo/garand_clip_reload.tik"
EMPTY_CLIP_SKD = "models/AMMO/GARANDCLIP/garand_clip_empty.skd"
EMPTY_CLIP_TIK = "models/ammo/garand_clip_empty.tik"
CLIP_DIR = "models/ammo/garandclip"
GUN_BONE = "m1garand "          # sic: the name ends in a space
CHAMBER_BONE = "m1garandchamber"
CLIP_BONE = "gun clip"
RELOAD_CLIP_BONE = "Clip"
EMPTY_CLIP_BONE = "Box02"
# the game's surfaces, whose names the .tik's frame commands use
BODY, MOVING, CLIP = "gun1", "gun3", "material8"

TEX_DIR = "textures/models/weapons/opm_garand"
GUN_SHADER, CLIP_SHADER = "opm_garand", "opm_garand_clip"
GUN_TEX, CLIP_TEX = 4096, 1024
GUN_SPEC, CLIP_SPEC = 2048, 1024
AO_AMOUNT = 0.5
METAL_SHARE = 0.5   # see thompson.py: the cubemap turns full-reflectance metal into a mirror
MAX_GLOSS = 0.75

# In the game gun's space (cm, X up, Y to the right, Z along the barrel to the muzzle).
# The bolt (Z 9.4-18.3), the bit of the operating rod beside it, and the rod with its handle
# out to the right (to Y 2.5) as far forward as it shows (Z 33): what slides back
MOVING_BOX = ((6.2, 8.6), (-3.5, 2.7), (9.0, 33.0))
# The sights: the rear aperture's ring, and the front sight (its post between two ears)
REAR_SIGHT_BOX = ((8.6, 10.6), (-2.4, -1.2), (5.4, 6.8))
FRONT_SIGHT_BOX = ((8.4, 10.8), (-1.9, -0.2), (74.0, 76.4))
POST_HALF_WIDTH = 0.2           # the post, between the ears: this near the sight's middle
# The line the sights go onto passes this far (cm, X up) over the game gun's post: in game
# the post's tip sat 5 px under the crosshair (the camera sits off the eye bone)
AIM_RAISE = 0.25
STOCK_FRONT_Z = 76.0            # the game gun's front sight is beyond this
# The clip sits this far under the closed bolt
CLIP_GAP = 0.05
# How far the bolt goes back. The game's goes 9.34 cm, as far as the real one; this model's
# receiver heel slopes down behind the rear sight and the bolt's back comes up through it
# past 5 cm. So it goes 5 cm on each shot and when empty, and all the way back only while
# the reload's hand pushes the new clip in (which needs the well open: frames 26-49 of the
# reload, the clip shown from 28 and in by 36, the bolt let go at 49)
BOLT_TRAVEL = 5.0
STOCK_TRAVEL = 9.342
RELOAD_FULL = {24: 0.0, 26: 1.0, 49: 1.0, 50: 0.0}   # frame: share of the full travel, eased between
# The last shot (fire_empty): the shot's kick, the bolt left back, held this long. The
# player's reload waits for it (Weapon::IsFiringEmpty), so the empty clip, thrown out with
# the shot, is seen to fly while the player still aims, and the reload starts after
LAST_SHOT_TIME = 0.4

# MOHAIM's aim down the sights for the Garand, and the arms holding it
FPS_SKD = "models/player/us_army/USarmyplyr.skd"
AIM_SKC = "models/human/animation/viewmodel/garand_stand_aim.skc"
EYE_BONE = "eyes bone"
WEAPON_TAG = "tag_weapon_right"

# The gun's animations (the game loads the processed newanim/ copies where they exist) and
# ours, the same with the chamber's channel along our receiver
ANIMS = {"idle": "garand.skc", "empty": "garand_empty.skc", "fire": "garand_fire_aim.skc",
         "reload": "garand_reload.skc"}
OUR_SKC = "opm_garand_%s.skc"

Image.MAX_IMAGE_PIXELS = None


# ----------------------------------------------------------------------------- source

GLTF_TYPES = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16,
              5125: np.uint32, 5126: np.float32}
GLTF_WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


class Glb:
    """A glTF binary's meshes (by material name: positions, normals, UVs, triangles, in the
    scene's space) and its images."""

    def __init__(self, path):
        data = open(path, "rb").read()
        if data[:4] != b"glTF":
            raise SystemExit("%s is not a glTF binary" % path)
        size = struct.unpack_from("<I", data, 12)[0]
        self.json = j = json.loads(data[20:20 + size])
        self.bin = memoryview(data)[20 + size + 8:]
        self.meshes = {}

        def walk(index, parent):
            node = j["nodes"][index]
            m = parent @ (np.array(node["matrix"], float).reshape(4, 4).T if "matrix" in node
                          else self.trs(node))
            for prim in j["meshes"][node["mesh"]]["primitives"] if "mesh" in node else ():
                a = prim["attributes"]
                p = self.accessor(a["POSITION"]) @ m[:3, :3].T + m[:3, 3]
                n = self.accessor(a["NORMAL"]) @ np.linalg.inv(m[:3, :3])
                n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
                t = self.accessor(prim["indices"]).astype(int).reshape(-1, 3)
                if np.linalg.det(m[:3, :3]) < 0:
                    t = t[:, ::-1]
                mat = j["materials"][prim["material"]]
                self.meshes[mat["name"]] = (p, n, self.accessor(a["TEXCOORD_0"]), t, mat)
            for c in node.get("children", ()):
                walk(c, m)

        for root in j["scenes"][j.get("scene", 0)]["nodes"]:
            walk(root, np.eye(4))

    @staticmethod
    def trs(node):
        m = np.eye(4)
        if "rotation" in node:
            x, y, z, w = node["rotation"]
            m[:3, :3] = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
        m[:3, :3] *= np.array(node.get("scale", (1, 1, 1)))[None, :]
        m[:3, 3] = node.get("translation", (0, 0, 0))
        return m

    def view(self, index):
        bv = self.json["bufferViews"][index]
        start = bv.get("byteOffset", 0)
        return self.bin[start:start + bv["byteLength"]], bv.get("byteStride")

    def accessor(self, index):
        a = self.json["accessors"][index]
        raw, stride = self.view(a["bufferView"])
        dt, width = np.dtype(GLTF_TYPES[a["componentType"]]), GLTF_WIDTH[a["type"]]
        stride = stride or dt.itemsize * width
        buf = np.frombuffer(raw, np.uint8, offset=a.get("byteOffset", 0))
        rows = np.lib.stride_tricks.as_strided(buf, (a["count"], dt.itemsize * width), (stride, 1))
        out = np.ascontiguousarray(rows).view(dt).reshape(a["count"], width).astype(float)
        return out[:, 0] if width == 1 else out

    def image(self, texture_ref):
        img = self.json["images"][self.json["textures"][texture_ref["index"]]["source"]]
        raw, _ = self.view(img["bufferView"])
        return Image.open(io.BytesIO(bytes(raw)))


class Mesh:
    """One of the source's meshes, as the game wants it: one vertex per position, UV and
    normal (glTF has that already)."""

    def __init__(self, pos, normal, uv, tris):
        self.pos, self.normal, self.uv, self.tris = pos, normal, uv, tris

    def transformed(self, s, r, t):
        n = self.normal @ r.T
        return s * self.pos @ r.T + t, n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)


def find_source(src):
    found = sorted(glob.glob(os.path.join(src, "*.glb")))
    if not found:
        raise SystemExit("no source model (*.glb) in %s" % src)
    return found[0]


# ----------------------------------------------------------------------------- fitting

def stock_points(model, skip=()):
    pts, tris = [], []
    for surf in model.surfaces:
        if surf.name.lower() in skip:
            continue
        p, t = surface_points(surf)
        tris.append(t + sum(len(x) for x in pts))
        pts.append(p)
    return np.concatenate(pts), np.concatenate(tris)


def fit_gun(mesh, gun, rng):
    sp, st = stock_points(gun, skip=(CLIP,))  # the stock's clip is not in our rifle's mesh
    stock = sample(sp, st, 40000, rng)
    new = sample(mesh.pos, mesh.tris, 40000, rng)
    k = np.ptp(stock[:, 2]) / np.ptp(new[:, 2])  # glTF is in metres: start near the size
    best = None
    # source Y up -> game X up; its X to the game's Y either way round, and Z (along the
    # barrel) with it so that the turn is a rotation, not a mirror
    for side in (1, -1):
        fit = icp(new * k, stock, np.array([[0, 1, 0], [side, 0, 0], [0, 0, -side]], float))
        if best is None or fit[3] < best[3]:
            best = fit
    s, r, t, err = best
    print("rifle fitted: scale %.4f, %.2f cm apart" % (s, err / 2))
    if not 0.8 < s < 1.25 or err > 2.0:
        raise SystemExit("the model does not fit the game's Garand (scale %.3f, %.2f cm)" % (s, err / 2))
    return s * k, r, t


def fit_rigid(src, dst, rng):
    """r, t putting the points src onto dst, trying every axis-aligned start."""
    best = None
    for perm in ((0, 1, 2), (0, 2, 1), (1, 0, 2), (1, 2, 0), (2, 0, 1), (2, 1, 0)):
        for signs in ((1, 1, 1), (1, -1, -1), (-1, 1, -1), (-1, -1, 1),
                      (-1, -1, -1), (-1, 1, 1), (1, -1, 1), (1, 1, -1)):
            r0 = np.zeros((3, 3))
            for i, p in enumerate(perm):
                r0[i, p] = signs[i]
            if np.linalg.det(r0) < 0:
                continue
            fit = icp(src, dst, r0, scale=False, iters=40)
            if best is None or fit[3] < best[3]:
                best = fit
    return best[1], best[2], best[3] / 2


# ----------------------------------------------------------------------------- sights

def aim_eye(fs, gun, idle):
    """Where the aim pose puts the eye, in the gun bone's space."""
    fps = pose.Skeleton(skd.read(fs.read(FPS_SKD)).bones)
    body = fps.evaluate(pose.frame_channels(read_anim(fs, AIM_SKC), 0))
    held = pose.Skeleton(gun.bones).evaluate(pose.frame_channels(idle, 0))
    to_body = np.array(body[WEAPON_TAG.lower()]) @ np.array(held[GUN_BONE])
    return (np.linalg.inv(to_body) @ np.array(body[EYE_BONE.lower()])[:, 3])[:3]


def post_tip(p):
    """The top of a front sight's post: the middle of the sight, between its ears."""
    mid = np.median(p[:, 1])
    p = p[np.abs(p[:, 1] - mid) < POST_HALF_WIDTH]
    return p[p[:, 0] > p[:, 0].max() - 0.03].mean(0)


def stock_post(gun):
    p = np.concatenate([surface_points(s)[0] for s in gun.surfaces])
    return post_tip(p[(p[:, 2] > STOCK_FRONT_Z) & (p[:, 0] > 9.0)])


def peep_hole(pos, tris, eye):
    """The middle of the rear aperture, as the eye sees it: the ring drawn from the eye
    (looking along Z), and the centre of the gap it closes round."""
    ring = tris[pieces_in(pos, tris, REAR_SIGHT_BOX)]
    if not 50 <= len(ring) <= 1500:
        raise SystemExit("the rear sight cut took %d triangles" % len(ring))
    rel = pos[ring] - eye
    uv = rel[..., :2] / rel[..., 2:3]
    lo, hi = uv.reshape(-1, 2).min(0), uv.reshape(-1, 2).max(0)
    n = 600
    cell = (hi - lo).max() / (n - 1)
    mask = np.zeros((n, n), bool)
    gy, gx = np.mgrid[0:n, 0:n]
    px = np.stack([gy, gx], -1) * cell + lo
    for a, b, c in uv:
        d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(d) < 1e-15:
            continue
        l0 = ((b[1] - c[1]) * (px[..., 0] - c[0]) + (c[0] - b[0]) * (px[..., 1] - c[1])) / d
        l1 = ((c[1] - a[1]) * (px[..., 0] - c[0]) + (a[0] - c[0]) * (px[..., 1] - c[1])) / d
        mask |= (l0 >= 0) & (l1 >= 0) & (l0 + l1 <= 1)
    hole = ndimage.binary_fill_holes(mask) & ~mask
    if hole.sum() < 20:
        raise SystemExit("no hole in the rear sight")
    d = np.argwhere(hole).mean(0) * cell + lo
    z = pos[ring][..., 2].mean()
    return eye + np.array([d[0], d[1], 1.0]) * (z - eye[2])


def front_post(pos, tris):
    p = pos[np.unique(tris[pieces_in(pos, tris, FRONT_SIGHT_BOX)])]
    if not len(p):
        raise SystemExit("no front sight")
    return post_tip(p)


def sight_fix(pos, tris, eye, target):
    """The rotation and offset putting our aperture and front post on the line from the
    eye to target (the game gun's post): p -> r @ (p - hole) + new_hole."""
    hole, post = peep_hole(pos, tris, eye), front_post(pos, tris)
    want = (target - eye) / np.linalg.norm(target - eye)
    have = (post - hole) / np.linalg.norm(post - hole)
    axis = np.cross(have, want)
    s, c = np.linalg.norm(axis), have @ want
    k = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    r = np.eye(3) + k + k @ k * ((1 - c) / (s * s)) if s > 1e-12 else np.eye(3)
    new_hole = eye + want * ((hole - eye) @ want)
    print("sights: aperture %s, post %s; turned %.2f deg, aperture moved %.2f cm"
          % (np.round(hole, 2), np.round(post, 2), np.degrees(np.arcsin(min(s, 1.0))),
             np.linalg.norm(new_hole - hole)))
    return r, hole, new_hole


# ----------------------------------------------------------------------------- model

def surfaces(named_tris, pos, normal, uv, bone):
    """bone: one bone index for every vertex, or an array of them (pos then in each one's
    space)."""
    bones = np.broadcast_to(np.asarray(bone), (len(pos),))
    out = []
    for name, tris in named_tris:
        for piece, ptris, order in split(name, tris):
            s = skd.Surface(piece)
            s.triangles = [(a, c, b) for a, b, c in ptris]  # the game's front faces wind clockwise
            s.verts = [skd.Vertex(normal[i], (uv[i][0], uv[i][1]),  # glTF's v runs down, as the game's t
                                  [(int(bones[i]), 1.0, tuple(pos[i]))]) for i in order]
            out.append(s)
    return out


def to_bone(held, bone, pos, normal):
    """Gun-space points and normals into bone's space, as the idle pose holds it."""
    m = np.linalg.inv(np.array(held[bone.lower()])) @ np.array(held[GUN_BONE])
    return pos @ m[:3, :3].T + m[:3, 3], normal @ m[:3, :3].T


def full_share(keys, n):
    """keys {frame: share}, linear between them, held outside."""
    frames = sorted(keys)
    return list(np.interp(np.arange(n), frames, [keys[f] for f in frames]))


def last_shot(fire):
    """fire, its last frame held to LAST_SHOT_TIME, the chamber back from its second frame
    on (with_chamber then sets how far)."""
    n = max(fire.num_frames, int(round(LAST_SHOT_TIME / fire.frame_time)))
    a = skc.Animation()
    a.flags, a.frame_time = fire.flags, fire.frame_time
    a.total_delta, a.total_angle_delta = fire.total_delta, fire.total_angle_delta
    a.frames = [fire.frames[min(f, fire.num_frames - 1)] for f in range(n)]
    a.channels = list(fire.channels)
    at = a.channels.index(CHAMBER_BONE + " pos")
    a.values = []
    for f in range(n):
        row = list(fire.values[min(f, fire.num_frames - 1)])
        v = row[at]
        row[at] = (v[0], v[1], 0.0 if f == 0 else -STOCK_TRAVEL, 0.0)
        a.values.append(row)
    return a


def with_chamber(src, forward, full=None):
    """src with the chamber bone's position along forward (a unit vector in the gun's
    space), at the game chamber's times: as far back as BOLT_TRAVEL is of the game's
    travel, or, by full[frame] (0 to 1), up to the whole of it."""
    a = skc.Animation()
    a.flags, a.frame_time = src.flags, src.frame_time
    a.total_delta, a.total_angle_delta = src.total_delta, src.total_angle_delta
    a.frames = list(src.frames)
    a.channels = list(src.channels)
    at = a.channels.index(CHAMBER_BONE + " pos")
    short = BOLT_TRAVEL / STOCK_TRAVEL
    a.values = []
    for f, row in enumerate(src.values):
        row = [tuple(v[:3]) + (0.0,) if n.endswith(" pos") else tuple(v)
               for n, v in zip(src.channels, row)]
        k = short + (1.0 - short) * (full[f] if full else 0.0)
        p = row[at][2] * k * np.asarray(forward)
        row[at] = (float(p[0]), float(p[1]), float(p[2]), 0.0)
        a.values.append(row)
    return a


# ----------------------------------------------------------------------------- textures

def normal_map_green_down(img):
    """Whether a tangent-space normal map's green points down the image. A normal map is
    the slope of a height field, so with the right sign its two components' cross
    derivatives agree (dG/dx = dR/dy); with green the other way they are opposed."""
    nm = np.asarray(img.convert("RGB").resize((1024, 1024), Image.BILINEAR)).astype(np.float32) / 127.5 - 1
    z = np.maximum(nm[..., 2], 0.2)
    r, g = (ndimage.gaussian_filter(nm[..., k] / z, 1.5) for k in (0, 1))
    gx, ry = g[1:-1, 2:] - g[1:-1, :-2], r[2:, 1:-1] - r[:-2, 1:-1]
    keep = (np.abs(gx) + np.abs(ry) > 0.02) & (np.abs(gx) + np.abs(ry) < 0.5)
    return np.corrcoef(gx[keep], ry[keep])[0, 1] > 0


def sized(img, size, mode):
    img = img.convert(mode)
    return img.resize((size, size), Image.LANCZOS) if img.size != (size, size) else img


def textures(src, out, material, name, size, spec_size):
    """material: the glTF material. Writes <name>.jpg, <name>_n.jpg, <name>_s.png."""
    pbr = material["pbrMetallicRoughness"]
    base_img, orm_img = src.image(pbr["baseColorTexture"]), src.image(pbr["metallicRoughnessTexture"])
    occ_img = src.image(material["occlusionTexture"]) if "occlusionTexture" in material else None
    nrm_img = src.image(material["normalTexture"])

    def channels(s):
        base = np.asarray(sized(base_img, s, "RGB")).astype(np.float32) / 255.0
        orm = np.asarray(sized(orm_img, s, "RGB")).astype(np.float32) / 255.0
        ao = (np.asarray(sized(occ_img, s, "RGB")).astype(np.float32)[..., 0] / 255.0
              if occ_img is not None else np.ones(base.shape[:2], np.float32))
        return base, ao[..., None], orm[..., 1], orm[..., 2:3]  # glTF: G roughness, B metal

    os.makedirs(os.path.join(out, TEX_DIR), exist_ok=True)
    base, ao, _, _ = channels(size)
    diffuse = base * (1.0 - AO_AMOUNT + AO_AMOUNT * ao)
    Image.fromarray(np.clip(diffuse * 255 + 0.5, 0, 255).astype(np.uint8)).save(
        os.path.join(out, TEX_DIR, name + ".jpg"), quality=92, subsampling=0)
    normal = sized(nrm_img, size, "RGB")
    if not normal_map_green_down(normal):
        r, g, b = normal.split()
        normal = Image.merge("RGB", (r, g.point(lambda v: 255 - v), b))
        print("%s: normal map's green flipped to point down the image" % name)
    normal.save(os.path.join(out, TEX_DIR, name + "_n.jpg"), quality=95, subsampling=0)

    base, ao, rough, metal = channels(spec_size)
    spec = np.empty(base.shape[:2] + (4,), np.float32)
    share = METAL_SHARE * metal
    spec[..., :3] = (0.04 * (1.0 - share) + base * share) * (0.5 + 0.5 * ao)
    spec[..., 3] = np.minimum(1.0 - rough, MAX_GLOSS)
    Image.fromarray(np.clip(spec * 255 + 0.5, 0, 255).astype(np.uint8), "RGBA").save(
        os.path.join(out, TEX_DIR, name + "_s.png"), optimize=True)


def shaders(out):
    text = "// the M1 Garand remade (tools/weapons/garand.py)\n"
    for shader, tex in ((GUN_SHADER, "garand"), (CLIP_SHADER, "clip")):
        text += ("%s\n{\n\tqer_editorimage %s/%s.tga\n\t{\n\t\tmap %s/%s.tga\n"
                 "\t\trgbGen lightingSpherical\n\t}\n}\n" % (shader, TEX_DIR, tex, TEX_DIR, tex))
    write(out, "scripts/opm_garand.shader", text.replace("\n", "\r\n").encode("latin-1"))


# ----------------------------------------------------------------------------- tiki

def retarget_tik(text, skd_name, shaders_by_surface, anims=None):
    """The game's .tik on our model: its skelmodel and surface lines, and (anims: {animation
    name: our .skc}) those animations' files; the rest as it was. Text with LF line ends."""
    out, done, in_anims, renamed = [], False, False, set()
    for line in text.splitlines():
        words = line.split()
        indent = line[:len(line) - len(line.lstrip())]
        if words[:1] == ["animations"]:
            in_anims = True
        if anims and in_anims and len(words) >= 2 and words[0] in anims and words[1].lower().endswith(".skc"):
            start = line.index(words[1], len(indent) + len(words[0]))
            line = line[:start] + anims[words[0]] + line[start + len(words[1]):]
            renamed.add(words[0])
        elif words[:1] == ["skelmodel"]:
            line = "%sskelmodel %s" % (indent, skd_name)
        elif words[:1] == ["surface"] and len(words) >= 3 and words[2] == "shader":
            if not done:
                out.extend("%ssurface %s shader %s" % (indent, s, sh) for s, sh in shaders_by_surface.items())
                done = True
            continue
        out.append(line)
    if not done:
        raise SystemExit("no surface lines in the game's .tik")
    if anims and renamed != set(anims):
        raise SystemExit("the game's .tik has no animation %s" % ", ".join(sorted(set(anims) - renamed)))
    return "\n".join(out) + "\n"


def edit(text, old, new):
    """text with the one line whose words are old's replaced by new's lines (new may be
    empty: the line goes), at its indent."""
    lines = text.split("\n")
    hits = [i for i, l in enumerate(lines) if l.split() == old.split()]
    if len(hits) != 1:
        raise SystemExit("the game's .tik has %d lines '%s'" % (len(hits), old))
    i = hits[0]
    indent = lines[i][:len(lines[i]) - len(lines[i].lstrip())]
    lines[i:i + 1] = [indent + n for n in new]
    return "\n".join(lines)


def clip_shown(text):
    """The loaded clip shown while there are rounds (the game hides it but while the reload
    pushes it in). The commands name it with a wildcard: the engine's surface command takes
    only the first surface of a name, and the clip is in pieces."""
    every = CLIP + "*"
    text = edit(text, "surface %s +nodraw" % CLIP, [])                       # init: shown
    text = edit(text, "28 surface %s -nodraw" % CLIP,                         # reload: the
                ["entry surface %s +nodraw" % every, "28 surface %s -nodraw" % every])  # old one out
    text = edit(text, "48 surface %s +nodraw" % CLIP, [])                     # the new one stays
    # the last shot: out with the clip at once (the empty pose has the clip bone up, where
    # the reload brings the new one in from)
    lines = text.split("\n")
    start = next(i for i, l in enumerate(lines) if l.split()[:1] == ["fire_empty"])
    at = next(i for i in range(start, len(lines)) if lines[i].split() == ["entry", "shoot"])
    indent = lines[at][:len(lines[at]) - len(lines[at].lstrip())]
    lines.insert(at + 1, indent + "entry surface %s +nodraw" % every)
    text = "\n".join(lines)
    # the clip thrown out and its ping with the last shot, as the real rifle's (the game's
    # come 0.3 s after)
    text = edit(text, "entry commanddelay 0.3 tagspawn tag_eject", ["entry tagspawn tag_eject"])
    text = edit(text, "entry commanddelay 0.3 sound snd_ping", ["entry sound snd_ping"])
    text = edit(text, "idle_empty %s" % OUR_SKC % "empty",                    # empty: none
                ["idle_empty %s" % OUR_SKC % "empty", "{", "\tserver", "\t{",
                 "\t\tentry surface %s +nodraw" % every, "\t}", "}"])
    return text


# -----------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--src", default=os.path.join(GAME, "models", "Garand"))
    ap.add_argument("--game", default=GAME)
    args = ap.parse_args()

    src = Glb(find_source(args.src))
    if "M1_Garand" not in src.meshes or "Bullets" not in src.meshes:
        raise SystemExit("the source has no M1_Garand and Bullets meshes (%s)" % ", ".join(src.meshes))
    fs = vfs.GameFS([os.path.join(args.game, "main")], skip_paks=[PAK])
    gun = skd.read(fs.read(GUN_SKD))
    reload_clip = skd.read(fs.read(RELOAD_CLIP_SKD))
    empty_clip = skd.read(fs.read(EMPTY_CLIP_SKD))
    stock = {k: read_anim(fs, "%s/%s" % (GUN_DIR, v)) for k, v in ANIMS.items()}
    stock["last"] = last_shot(stock["fire"])
    held = pose.Skeleton(gun.bones).evaluate(pose.frame_channels(stock["idle"], 0))
    rng = np.random.default_rng(1)

    p, n, uv, t, gun_mat = src.meshes["M1_Garand"]
    mesh = Mesh(p, n, uv, t)
    s, r, tr = fit_gun(mesh, gun, rng)
    pos, normal = mesh.transformed(s, r, tr)

    moving = pieces_in(pos, mesh.tris, MOVING_BOX)
    if not 400 <= moving.sum() <= 2000:
        raise SystemExit("the bolt and operating rod cut took %d triangles" % moving.sum())
    print("%d triangles: %d rifle, %d bolt and operating rod"
          % (len(mesh.tris), (~moving).sum(), moving.sum()))

    # the sights onto the aim pose's line (after the cut, whose box is the fit's)
    eye = aim_eye(fs, gun, stock["idle"])
    sr, hole, new_hole = sight_fix(pos, mesh.tris, eye, stock_post(gun) + np.array([AIM_RAISE, 0.0, 0.0]))
    pos = (pos - hole) @ sr.T + new_hole
    normal = normal @ sr.T
    forward = sr @ np.array([0.0, 0.0, 1.0])  # along the receiver

    # the clip, fitted with the rifle, then seated straight down under the closed bolt
    cp, cn, cuv, ct, clip_mat = src.meshes["Bullets"]
    clip = Mesh(cp, cn, cuv, ct)
    cpos, cnormal = clip.transformed(s, r, tr)
    cpos, cnormal = (cpos - hole) @ sr.T + new_hole, cnormal @ sr.T
    bolt = pos[np.unique(mesh.tris[moving])]
    lo, hi = cpos.min(0), cpos.max(0)
    over = bolt[(bolt[:, 1] > lo[1]) & (bolt[:, 1] < hi[1]) & (bolt[:, 2] > lo[2]) & (bolt[:, 2] < hi[2])]
    if not len(over):
        raise SystemExit("the clip is not under the bolt")
    drop = over[:, 0].min() - CLIP_GAP - hi[0]
    cpos = cpos + drop * (sr @ np.array([1.0, 0.0, 0.0]))
    print("clip seated %.2f cm down, its top %.2f cm under the bolt" % (-drop, CLIP_GAP))

    # the rifle: bolt and rod on the chamber bone (at the gun's origin in the idle pose, so
    # their gun-space positions are theirs), the rest on the gun bone
    bone_of = np.full(len(pos), gun.bone_index(GUN_BONE))
    bone_of[np.unique(mesh.tris[moving])] = gun.bone_index(CHAMBER_BONE)
    if not np.allclose(held[CHAMBER_BONE], held[GUN_BONE], atol=1e-4):
        raise SystemExit("the game's idle pose moves the chamber off the gun")
    surfs = surfaces([(BODY, mesh.tris[~moving].tolist()), (MOVING, mesh.tris[moving].tolist())],
                     pos, normal, mesh.uv, bone_of)
    lpos, lnormal = to_bone(held, CLIP_BONE, cpos, cnormal)
    surfs += surfaces([(CLIP, clip.tris.tolist())], lpos, lnormal, clip.uv, gun.bone_index(CLIP_BONE))
    write(args.out, "%s/opm_garand.skd" % GUN_DIR, skd.write(model_like(gun, "opm_garand.skd", surfs)))
    print("opm_garand.skd: %d surfaces, %d vertices" % (len(surfs), sum(len(x.verts) for x in surfs)))

    # the clip in the hand: the reload clip's bone holds it as the gun's clip bone does
    # (the game's two clips are the same mesh in the same place on their bones)
    rc = surfaces([("clip", clip.tris.tolist())], lpos, lnormal, clip.uv,
                  reload_clip.bone_index(RELOAD_CLIP_BONE))
    write(args.out, "%s/opm_garand_clipreload.skd" % CLIP_DIR,
          skd.write(model_like(reload_clip, "opm_garand_clipreload.skd", rc)))

    # the empty clip thrown out: the clip without its rounds (the piece as tall as the
    # whole stack: each round is a piece of its own, a round high), fitted onto the game's
    piece = parts(cpos, clip.tris)
    height = {k: np.ptp(cpos[np.unique(clip.tris[piece == k])][:, 0]) for k in np.unique(piece)}
    metal = max(height, key=height.get)
    if len(height) != 9 or sorted(height.values())[-2] > 0.6 * height[metal]:
        raise SystemExit("no clip among the rounds (%d pieces)" % len(height))
    ctris = clip.tris[piece == metal]
    sp, st = stock_points(empty_clip)
    er, et, err = fit_rigid(sample(lpos, ctris, 6000, rng), sample(sp, st, 6000, rng), rng)
    print("empty clip fitted onto the game's: %.2f cm apart" % err)
    epos, enormal = lpos @ er.T + et, lnormal @ er.T
    ec = surfaces([("clip", ctris.tolist())], epos, enormal, clip.uv, empty_clip.bone_index(EMPTY_CLIP_BONE))
    write(args.out, "%s/opm_garand_clip_empty.skd" % CLIP_DIR,
          skd.write(model_like(empty_clip, "opm_garand_clip_empty.skd", ec)))

    # the animations, the chamber along our receiver
    for kind, anim in stock.items():
        full = full_share(RELOAD_FULL, anim.num_frames) if kind == "reload" else None
        write(args.out, "%s/%s" % (GUN_DIR, OUR_SKC % kind), skc.write(with_chamber(anim, forward, full)))
    print("animations: %s" % ", ".join("%s %d frames" % (k, a.num_frames) for k, a in stock.items()))

    # the .tik files
    ours = {k: OUR_SKC % k for k in stock}
    anims = {"idle": ours["idle"], "secondaryfire": ours["idle"], "idle_empty": ours["empty"],
             "reload": ours["reload"], "fire": ours["fire"], "fire_empty": ours["last"]}
    tik = retarget_tik(fs.read_text(GUN_TIK).replace("\r\n", "\n"), "opm_garand.skd",
                       {BODY: GUN_SHADER, MOVING: GUN_SHADER, CLIP: CLIP_SHADER}, anims)
    tik = clip_shown(tik)
    # reloaded only when emptied, as the real en-bloc clip is (the game's own .tik says so;
    # the HRRTM mods commented it out)
    tik = re.sub(r"(?m)^(\s*)//\s*(dm)?cantpartialreload", r"\1\2cantpartialreload", tik)
    if "\ncantpartialreload" not in "\n" + "\n".join(l.strip() for l in tik.splitlines()):
        raise SystemExit("the Garand's .tik has no cantpartialreload line to put back")
    tik = add_animations(tik.replace("\n", "\r\n"),  # the empty rifle's bash: bolt back
                         {"secondaryfire_empty": ("secondaryfire", ours["empty"])})
    write(args.out, GUN_TIK, tik.encode("latin-1"))
    for path, name in ((RELOAD_CLIP_TIK, "opm_garand_clipreload.skd"),
                       (EMPTY_CLIP_TIK, "opm_garand_clip_empty.skd")):
        text = retarget_tik(fs.read_text(path).replace("\r\n", "\n"), name, {"all": CLIP_SHADER})
        write(args.out, path, text.replace("\n", "\r\n").encode("latin-1"))

    textures(src, args.out, gun_mat, "garand", GUN_TEX, GUN_SPEC)
    textures(src, args.out, clip_mat, "clip", CLIP_TEX, CLIP_SPEC)
    shaders(args.out)
    print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
