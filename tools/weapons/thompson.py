#!/usr/bin/env python3
"""The Thompson remade from a model of the player's own (opm-thompson's generator).

The source is one unrigged mesh and its PBR maps, read in place and never committed:

    <MOHAA_GAME>/models/Thompson/source/*.rar (one FBX mesh), textures/m1a1_*.png
    (or the first download: model_0.obj, Thompson_{BASE,NM,ROUGH,MET,AO}.png)

(--src to read them from elsewhere). The game's Thompson is rigid: every vertex is on one
bone ("tommy gun new"), the arms' animations carry it, and the reload only hides its Clip
surface while the hand holds a separate magazine model (models/ammo/thompson_clip.tik). The
new gun keeps all that and adds one moving part, its cocking knob (see BOLT_BOX). It is

  1. fitted onto the game's gun (models/weapons/thompsonsmg/ThompsonSMG.skd, the copy the
     game loads): the axes swapped (the source is Y up, Z along the barrel; the game's gun is
     X up), then a scaled ICP of points sampled over both surfaces, so the sights, the grip
     and the muzzle land where the arm animations, the aim poses and the barrel tag expect;
  2. cut: the magazine (a box under the magazine well) becomes the surface "Clip", the rest
     "ThompsonSMG1", each split into pieces of at most 1000 vertices and 2000 triangles under
     the same name, so the .tik still shades them and the reload still hides the magazine;
  3. written as opm_thompson.skd, with the game's bones and hit boxes; the magazine again, on
     the reload magazine's bone and in its place (fitted the same way from the game's two
     magazines), as opm_thompsonclip.skd;
  4. textured: base colour (with a little of the AO), the normal map as it is (its green is
     down the image, as GL2's tangent space has it), and GL2's specular map: reflectance from
     metalness (4% for the dielectric parts, up to half the base colour for bare
     metal), gloss 1 - roughness, capped;
  5. given the knob: its piece on a bone of its own (opm_bolt), and the gun's animations
     copied with that bone's channels added, open bolt: back at rest, forward and back on
     each shot; the empty gun's (fire_empty, idle_empty, reload_empty, which the game picks
     when the magazine is empty) leave it forward until the reload's left hand pulls it;
  6. the two .tik files the game loads, pointed at them (written here and not patched, so
     that without the source the pak is empty and the game keeps its own Thompson).

    tools/weapons/thompson.py --out build/opm-thompson
"""

import argparse
import os
import re
import struct
import sys

import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "blender", "io_scene_mohaa"))
from mohaa import huffman, pose, skc, skd, vfs  # noqa: E402

GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")
PAK = "zzzzzzzzzzzzzzzz-opm-thompson.pk3"  # what this builds: never read back from

GUN_SKD = "models/weapons/thompsonsmg/ThompsonSMG.skd"
GUN_TIK = "models/weapons/thompsonsmg.tik"
CLIP_SKD = "models/AMMO/THOMPSONCLIP/thompsonclip.skd"
CLIP_TIK = "models/ammo/thompson_clip.tik"
GUN_BONE = "tommy gun new"
CLIP_BONE = "Thompson clip left hand"

TEX_DIR = "textures/models/weapons/opm_thompson"
SHADER = "opm_thompson"
TEX_SIZE = 2048
SPEC_SIZE = 1024
AO_AMOUNT = 0.5
# Bare metal's base colour is its reflectance, which a PBR renderer shows only as
# reflections. Here (r_pbr 0, r_cubeMapping 1) a full reflectance turns the metal into a
# mirror of the area's coarse cubemap, flat grey whatever the texture says; so, as matgen
# does for metal, the specular takes only this share of it, the diffuse keeps the rest
# (lightall darkens the diffuse by the specular), and the gloss is capped
METAL_SHARE = 0.5
MAX_GLOSS = 0.75

# The magazine, in the game gun's space (cm, X up, Z along the barrel): narrower than the
# frame around its well (Y 0.32-3.87), and up to where it goes into the well (X 4.79)
MAG_BOX = ((-20.0, 4.9), (0.7, 3.25), (27.8, 32.3))

# The sights. The arms' aim pose (MOHAIM's aim down the sights: thompson_charge) puts the
# eye on the game gun's sight line, through its rear notch to its front post's tip. The
# fitted model, close everywhere else, has its own sights a degree off that line, so it is
# turned and moved, a little, to put its peep hole and front post on it
FPS_SKD = "models/player/us_army/USarmyplyr.skd"
AIM_SKC = "models/human/animation/viewmodel/idle_tommy_stand_aim.skc"
EYE_BONE = "eyes bone"
WEAPON_TAG = "tag_weapon_right"
STOCK_POST_Z = 58.0         # the game gun's front post: its highest vertices beyond this
REAR_SIGHT_BOX = ((8.4, 11.3), (1.3, 2.7), (5.8, 6.4))     # our peep sight's plate
FRONT_POST_BOX = ((6.5, 11.5), (0.6, 3.5), (62.8, 64.8))   # our front sight
# The whole gun moved this far (cm, X up, Y) in the aim, the line kept: in game it sat a
# little low and left of the crosshair (the camera sits off the eye bone): the peep hole
# round the crosshair; then the post onto it, turned about the hole (the line's far end
# moved, the post being much further from the eye moves more)
AIM_SHIFT = np.array([0.0, 0.0, 0.0])
AIM_POST = np.array([0.38, 0.15, 0.0])

# The FBX download's sights are each one piece with their guards and sleeve: when no piece
# lies in the boxes above, the triangles in these (its leaf, between the ears; its blade)
REAR_LEAF_BOX = ((9.3, 10.95), (1.35, 2.9), (6.9, 7.4))
FRONT_BLADE_BOX = ((9.0, 10.6), (1.5, 2.7), (61.5, 63.4))

# The cocking knob: the one piece standing out of the receiver's right side, modelled at the
# back of its slot (Z 15-16.6; the slot runs on to Z 26.3). It goes on a bone of its own and
# moves as the real M1A1's does, an open-bolt gun: held back while there are rounds, forward
# to fire each and back again, left forward by the shot that empties the magazine, and
# pulled back by the left hand when an emptied gun is reloaded
BOLT_BOX = ((6.5, 9.0), (3.3, 5.8), (14.5, 17.0))
# The forend (and the sling swivel on it), and the game's forend's size (see filled_forend)
FOREND_BOX = ((2.5, 8.4), (0.3, 4.1), (36.5, 60.0))
FOREND_WIDTH = 4.4
FOREND_BOTTOM = 3.6
KNOB_OUT = ((6.5, 9.0), (4.6, 5.8), (14.5, 17.0))   # the knob, past the receiver's side (Y 4.58)
BOLT_BONE = "opm_bolt"
BOLT_TRAVEL = 9.5
# The gun's animations (the game loads the processed newanim/ copies where they exist) and
# ours, the bolt added: 0 forward, 1 back
GUN_IDLE_SKC = "models/weapons/thompsonsmg/ThompsonSMG.skc"
GUN_FIRE_SKC = "models/weapons/thompsonsmg/ThompsonSMG_fire_aim.skc"
OUR_SKC = "models/weapons/thompsonsmg/opm_thompson_%s.skc"
# A shot: forward in one frame (32 ms), back over the next two, within the 90 ms fire delay
FIRE_BOLT = (1.0, 0.0, 0.55, 1.0, 1.0)
FIRE_EMPTY_BOLT = (1.0, 0.0, 0.0, 0.0, 0.0)
# The reload runs with the arms' viewmodel/smg/reload_tommy_stand.skc (30 fps, started
# together): the left fingers take the knob at frame 58 and pull it back from 64 to 65 (Z
# 28.2 -> 18.1, the knob's travel), where the sear holds it. With rounds left the bolt is
# back already and stays there (the game plays reload_empty, ours, only for an empty gun)
RELOAD_EMPTY_PULL = {0: 0.0, 64: 0.08, 65: 1.0}

# The FBX download is 57k triangles (36k vertices), over the engine's 32 surfaces of 1000
# vertices: Blender's collapse decimation keeps this share
FBX_DECIMATE = 0.55

Image.MAX_IMAGE_PIXELS = None


# ----------------------------------------------------------------------------- geometry

def read_obj(path):
    v, vt, vn, faces = [], [], [], []
    for line in open(path):
        if line.startswith("v "):
            v.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("vt "):
            vt.append([float(x) for x in line.split()[1:3]])
        elif line.startswith("vn "):
            vn.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("f "):
            corners = []
            for tok in line.split()[1:]:
                parts = (tok.split("/") + ["", ""])[:3]
                corners.append(tuple(int(p) - 1 if p else -1 for p in parts))
            for k in range(1, len(corners) - 1):
                faces.append((corners[0], corners[k], corners[k + 1]))
    return np.array(v), np.array(vt), np.array(vn), faces


def sample(points, tris, n, rng):
    a, b, c = points[tris[:, 0]], points[tris[:, 1]], points[tris[:, 2]]
    area = 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1)
    idx = rng.choice(len(tris), n, p=area / area.sum())
    u, w = rng.random(n), rng.random(n)
    flip = u + w > 1
    u[flip], w[flip] = 1 - u[flip], 1 - w[flip]
    return a[idx] + (b[idx] - a[idx]) * u[:, None] + (c[idx] - a[idx]) * w[:, None]


def similarity(src, dst, scale=True):
    """Least-squares s, R, t with dst ~ s R src + t (Umeyama)."""
    ms, md = src.mean(0), dst.mean(0)
    s0, d0 = src - ms, dst - md
    u, d, vt = np.linalg.svd(d0.T @ s0 / len(src))
    fix = np.eye(3)
    if np.linalg.det(u @ vt) < 0:
        fix[2, 2] = -1
    r = u @ fix @ vt
    s = np.trace(np.diag(d) @ fix) / s0.var(0).sum() if scale else 1.0
    return s, r, md - s * r @ ms


def icp(src, dst, r0, scale=True, iters=60):
    tree = cKDTree(dst)
    s, r = 1.0, r0
    t = dst.mean(0) - r @ src.mean(0)
    for _ in range(iters):
        d, i = tree.query(s * src @ r.T + t)
        keep = d < np.percentile(d, 90)
        s, r, t = similarity(src[keep], dst[i[keep]], scale)
    moved = s * src @ r.T + t
    err = tree.query(moved)[0].mean() + cKDTree(moved).query(dst)[0].mean()
    return s, r, t, err


def surface_points(surf):
    """A rigid surface's vertices in its bone's space, and its triangles."""
    return (np.array([v.weights[0][2] for v in surf.verts], float),
            np.array(surf.triangles, int))


# ----------------------------------------------------------------------------- the model

class Mesh:
    """The source mesh as the game wants it: one vertex per position, UV and normal."""

    def __init__(self, obj_path):
        v, vt, vn, faces = read_obj(obj_path)
        self.raw_positions = v
        self.raw_tris = np.array([[c[0] for c in f] for f in faces if len({c[0] for c in f}) == 3])
        keyed, self.tris = {}, []
        for f in faces:
            if len({c[0] for c in f}) < 3:  # half the source's triangles repeat a vertex
                continue
            self.tris.append([keyed.setdefault(c, len(keyed)) for c in f])
        self.tris = np.array(self.tris)
        corners = sorted(keyed, key=keyed.get)
        self.pos = np.array([v[c[0]] for c in corners])
        self.uv = np.array([vt[c[1]] if c[1] >= 0 else (0.0, 0.0) for c in corners])
        if vn.size and all(c[2] >= 0 for c in corners):
            self.normal = np.array([vn[c[2]] for c in corners])
        else:
            self.normal = vertex_normals(self.pos, self.tris)

    @classmethod
    def from_piece(cls, piece):
        """A mesh read by remodel.read_glb (its t runs down the image: turned back)."""
        self = cls.__new__(cls)
        self.pos, self.tris, self.normal = piece.pos, piece.tris, piece.normal
        self.uv = np.stack([piece.uv[:, 0], 1.0 - piece.uv[:, 1]], 1)
        self.raw_positions, self.raw_tris = self.pos, self.tris
        return self

    def transformed(self, s, r, t):
        pos = s * self.pos @ r.T + t
        n = self.normal @ r.T
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
        return pos, n


def vertex_normals(pos, tris):
    n = np.zeros_like(pos)
    fn = np.cross(pos[tris[:, 1]] - pos[tris[:, 0]], pos[tris[:, 2]] - pos[tris[:, 0]])
    for k in range(3):
        np.add.at(n, tris[:, k], fn)
    return n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)


def fit_gun(mesh, gun, rng):
    stock_p, stock_t = [], []
    for surf in gun.surfaces:
        if surf.name.lower() == "site":  # the sight blade's alpha card
            continue
        p, t = surface_points(surf)
        stock_t.append(t + sum(len(x) for x in stock_p))
        stock_p.append(p)
    stock = sample(np.concatenate(stock_p), np.concatenate(stock_t), 40000, rng)
    new = sample(mesh.raw_positions, mesh.raw_tris, 40000, rng)
    k = np.ptp(stock, 0).max() / np.ptp(new, 0).max()   # a source in other units: sized first
    new = new * k
    best = None
    # source Y up -> game X up; its X to the game's Y either way round, and Z (along the
    # barrel) with it so that the turn is a rotation, not a mirror
    for side in (1, -1):
        fit = icp(new, stock, np.array([[0, 1, 0], [side, 0, 0], [0, 0, -side]], float))
        if best is None or fit[3] < best[3]:
            best = fit
    s, r, t, err = best
    print("gun fitted: scale %.4f, %.2f cm apart" % (s * k, err / 2))
    if not 0.8 < s < 1.25 or err > 2.0:
        raise SystemExit("the model does not fit the game's Thompson (scale %.3f, %.2f cm)" % (s, err / 2))
    return s * k, r, t


def fit_clip(gun, clip, rng):
    """Where the game's gun magazine sits on the reload magazine's bone."""
    gp, gt = surface_points(next(s for s in gun.surfaces if s.name.lower() == "clip"))
    cp, ct = surface_points(clip.surfaces[0])
    a, b = sample(gp, gt, 8000, rng), sample(cp, ct, 8000, rng)
    best = None
    for perm in ((0, 1, 2), (0, 2, 1), (1, 0, 2), (1, 2, 0), (2, 0, 1), (2, 1, 0)):
        for signs in ((1, 1, 1), (1, -1, -1), (-1, 1, -1), (-1, -1, 1),
                      (-1, -1, -1), (-1, 1, 1), (1, -1, 1), (1, 1, -1)):
            r0 = np.zeros((3, 3))
            for i, p in enumerate(perm):
                r0[i, p] = signs[i]
            if np.linalg.det(r0) < 0:
                continue
            fit = icp(a, b, r0, scale=False, iters=40)
            if best is None or fit[3] < best[3]:
                best = fit
    _, r, t, err = best
    print("magazine placed on the reload bone: %.2f cm apart" % (err / 2))
    return r, t


def split(name, tris):
    """Triangles in pieces the engine takes, each with its own vertex list (taken in order
    of their lowest vertex, so that neighbours share a piece: a vertex in two pieces is two)."""
    pieces, cur, remap = [], [], {}
    for tri in sorted(tris, key=min):
        new = [i for i in tri if i not in remap]
        if len(cur) + 1 > skd.MAX_TRIS or len(remap) + len(new) > skd.MAX_VERTS:
            pieces.append((cur, remap))
            cur, remap = [], {}
        for i in tri:
            remap.setdefault(i, len(remap))
        cur.append(tuple(remap[i] for i in tri))
    if cur:
        pieces.append((cur, remap))
    return [(name, t, sorted(m, key=m.get)) for t, m in pieces]


def surfaces(named_tris, pos, normal, uv, bone):
    """bone: one bone index for every vertex, or an array of them (pos then in each one's
    space)."""
    bones = np.broadcast_to(np.asarray(bone), (len(pos),))
    out = []
    for name, tris in named_tris:
        for piece, ptris, order in split(name, tris):
            s = skd.Surface(piece)
            s.triangles = [(a, c, b) for a, b, c in ptris]  # the game's front faces wind clockwise
            s.verts = [skd.Vertex(normal[i], (uv[i][0], 1.0 - uv[i][1]),  # the game's t runs down
                                  [(int(bones[i]), 1.0, tuple(pos[i]))]) for i in order]
            out.append(s)
    return out


def parts(pos, tris):
    """Each triangle's connected piece, vertices that share a position joined."""
    _, welded = np.unique(np.round(pos, 4), axis=0, return_inverse=True)
    t = welded.ravel()[tris]
    n = t.max() + 1
    edges = coo_matrix((np.ones(3 * len(t)), (t.ravel(), t[:, [1, 2, 0]].ravel())), shape=(n, n))
    return connected_components(edges, directed=False)[1][t[:, 0]]


def pieces_in(pos, tris, box, piece=None):
    """The triangles of the pieces lying wholly inside box."""
    piece = parts(pos, tris) if piece is None else piece
    lo, hi = np.array(box).T
    chosen = np.zeros(len(tris), bool)
    for p in np.unique(piece):
        c = pos[tris[piece == p]].reshape(-1, 3)
        if (c.min(0) > lo).all() and (c.max(0) < hi).all():
            chosen |= piece == p
    return chosen


def in_box(pos, tris, box, fallback):
    """pieces_in(box), or, when no piece lies there, the triangles wholly inside fallback."""
    chosen = pieces_in(pos, tris, box)
    if not chosen.any():
        lo, hi = np.array(fallback).T
        chosen = ((pos[tris] > lo) & (pos[tris] < hi)).all((1, 2))
    return chosen


def filled_forend(pos, normal, tris):
    """The forend (and the sling swivel under it) filled out to the game's: the arms' left
    hand was posed round the game's forend, 4.6 cm wide with its bottom at X 3.57; the FBX
    download's is 3.1 cm wide and tapers up, and the fingers did not touch it. Wider about
    its middle, deeper from its top (where it meets the barrel). The first download's forend
    was the game's size: no piece lies in FOREND_BOX there."""
    chosen = pieces_in(pos, tris, FOREND_BOX)
    if not chosen.any():
        return pos, normal
    verts = np.unique(tris[chosen])
    lab = parts(pos, tris)
    wood = pos[np.unique(tris[lab == np.bincount(lab[chosen]).argmax()])]   # measured on the wood
    top, mid = wood[:, 0].max(), (wood[:, 1].min() + wood[:, 1].max()) / 2
    deep = (top - FOREND_BOTTOM) / (top - wood[:, 0].min())
    wide = FOREND_WIDTH / (wood[:, 1].max() - wood[:, 1].min())
    p = pos[verts]
    pos, normal = pos.copy(), normal.copy()
    pos[verts, 0] = top - (top - p[:, 0]) * deep
    pos[verts, 1] = mid + (p[:, 1] - mid) * wide
    n = normal[verts] / np.array([deep, wide, 1.0])
    normal[verts] = n / np.linalg.norm(n, axis=1, keepdims=True)
    print("forend filled out: %d triangles, %.2fx wider, %.2fx deeper" % (chosen.sum(), wide, deep))
    return pos, normal


def bolt_triangles(pos, tris):
    """The knob: the pieces lying wholly inside BOLT_BOX; in a model whose knob is one piece
    with the bolt (the FBX download), that piece, the one standing out of the receiver's
    right side (KNOB_OUT): the whole bolt moves, as the real one does."""
    chosen = pieces_in(pos, tris, BOLT_BOX)
    if not chosen.any():
        lab = parts(pos, tris)
        lo, hi = np.array(KNOB_OUT, float).T
        inside = ((pos > lo) & (pos < hi)).all(1)
        hit = np.unique(lab[inside[tris].any(1)])
        chosen = np.isin(lab, hit)
        if len(hit) != 1 or not 50 <= chosen.sum() <= 4000:
            raise SystemExit("the bolt cut took %d pieces, %d triangles" % (len(hit), chosen.sum()))
        return chosen
    if not 50 <= chosen.sum() <= 400:
        raise SystemExit("the cocking knob cut took %d triangles" % chosen.sum())
    return chosen


# ----------------------------------------------------------------------------- sights

def aim_eye(fs, gun):
    """Where the aim pose puts the eye, in the gun bone's space."""
    fps = pose.Skeleton(skd.read(fs.read(FPS_SKD)).bones)
    body = fps.evaluate(pose.frame_channels(read_anim(fs, AIM_SKC), 0))
    held = pose.Skeleton(gun.bones).evaluate(pose.frame_channels(read_anim(fs, GUN_IDLE_SKC), 0))
    to_body = np.array(body[WEAPON_TAG.lower()]) @ np.array(held[GUN_BONE.lower()])
    return (np.linalg.inv(to_body) @ np.array(body[EYE_BONE.lower()])[:, 3])[:3]


def stock_post(gun):
    p = np.concatenate([surface_points(s)[0] for s in gun.surfaces])
    p = p[p[:, 2] > STOCK_POST_Z]
    return p[p[:, 0] > p[:, 0].max() - 0.02].mean(0)


def peep_hole(pos, tris, eye):
    """The middle of the rear sight's hole, as the eye sees it: the plate drawn from the eye
    (looking along Z), and the centre of the gap it closes round."""
    plate = tris[in_box(pos, tris, REAR_SIGHT_BOX, REAR_LEAF_BOX)]
    if not 50 <= len(plate) <= 3000:
        raise SystemExit("the rear sight cut took %d triangles" % len(plate))
    rel = pos[plate] - eye
    uv = rel[..., :2] / rel[..., 2:3]                  # X/Z, Y/Z: directions from the eye
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
    z = pos[plate][..., 2].mean()
    return eye + np.array([d[0], d[1], 1.0]) * (z - eye[2])


def front_post(pos, tris):
    p = pos[np.unique(tris[in_box(pos, tris, FRONT_POST_BOX, FRONT_BLADE_BOX)])]
    if not len(p):
        raise SystemExit("no front sight")
    return p[p[:, 0] > p[:, 0].max() - 0.03].mean(0)


def sight_fix(pos, tris, eye, target):
    """The rotation and offset putting our peep hole and front post on the line from the
    eye to target (the game gun's post): p -> r @ (p - hole) + new_hole."""
    hole, post = peep_hole(pos, tris, eye), front_post(pos, tris)
    want = (target - eye) / np.linalg.norm(target - eye)
    have = (post - hole) / np.linalg.norm(post - hole)
    axis = np.cross(have, want)
    s, c = np.linalg.norm(axis), have @ want
    k = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    r = np.eye(3) + k + k @ k * ((1 - c) / (s * s)) if s > 1e-12 else np.eye(3)
    new_hole = eye + want * ((hole - eye) @ want)
    print("sights: hole %s, post %s; turned %.2f deg, hole moved %.2f cm"
          % (np.round(hole, 2), np.round(post, 2), np.degrees(np.arcsin(min(s, 1.0))),
             np.linalg.norm(new_hole - hole)))
    return r, hole, new_hole


# ----------------------------------------------------------------------------- animation

def read_anim(fs, path):
    """An animation as the game loads it: the processed newanim/ copy when there is one."""
    if fs.exists("newanim/" + path):
        return read_processed(fs.read("newanim/" + path))
    return skc.read(fs.read(path))


def read_processed(data):
    """skeletor_c::LoadProcessedAnim's format (newanim/*.skc): Huffman coded, the channels
    as keys."""
    b = huffman.decode(data)
    o = 0

    def take(fmt):
        nonlocal o
        v = struct.unpack_from("<" + fmt, b, o)
        o += struct.calcsize("<" + fmt)
        return v

    _, flags, frame_time = take("iif")
    total_delta = take("3f")
    total_angle_delta, num_frames, num_keyed = take("fih")
    take("3B")
    a = skc.Animation()
    a.flags, a.frame_time, a.total_delta, a.total_angle_delta = flags, frame_time, total_delta, total_angle_delta
    for _ in range(num_frames):
        v = take("11f")
        a.frames.append(skc.Frame((v[0:3], v[3:6]), v[6], v[7:10], v[10]))
    take("6f")
    take("ih")
    keys = []
    for _ in range(num_keyed):
        count = take("h")[0]
        keys.append([(take("hh")[0], take("4f")) for _ in range(count)])
    take("i")
    for _ in range(take("i")[0]):
        end = b.index(b"\0", o)
        a.channels.append(b[o:end].decode("latin-1"))
        o = end + 1
    a.values = [[None] * len(a.channels) for _ in range(num_frames)]
    for c, ks in enumerate(keys[:len(a.channels)]):
        k, cur = 0, ks[0][1] if ks else (0.0, 0.0, 0.0, 1.0)
        for f in range(num_frames):
            while k < len(ks) and ks[k][0] <= f:
                cur = ks[k][1]
                k += 1
            a.values[f][c] = cur
    return a


def with_bolt(src, bolt, back_pos, slide):
    """src with the bolt's channels added, bolt[frame] from 0 (forward) to 1 (back); slide
    is the unit direction forward along the receiver."""
    a = skc.Animation()
    a.flags, a.frame_time = src.flags, src.frame_time
    a.total_delta, a.total_angle_delta = src.total_delta, src.total_angle_delta
    a.frames = list(src.frames)
    a.channels = list(src.channels) + [BOLT_BONE + " pos", BOLT_BONE + " rot"]
    a.values = []
    for f, row in enumerate(src.values):
        row = [tuple(v[:3]) + (0.0,) if n.endswith(" pos") else tuple(v)
               for n, v in zip(src.channels, row)]
        at = np.asarray(back_pos) + (1.0 - bolt[f]) * BOLT_TRAVEL * np.asarray(slide)
        a.values.append(row + [(float(at[0]), float(at[1]), float(at[2]), 0.0), (0.0, 0.0, 0.0, 1.0)])
    return a


def held(keys, n):
    """keys {frame: value} held until the next, 0 before the first."""
    out, cur = [], 0.0
    for f in range(n):
        cur = keys.get(f, cur)
        out.append(cur)
    return out


def model_like(stock, name, surfs, extra_bones=()):
    m = skd.Model(name)
    m.bones = list(stock.bones) + list(extra_bones)
    m.boxes = stock.boxes
    m.surfaces = surfs
    errors = skd.validate(m)
    if errors:
        raise SystemExit("; ".join(errors))
    return m


# ----------------------------------------------------------------------------- textures

# the maps' file names: the first OBJ download's, then the FBX download's (in textures/)
MAP_NAMES = ({"BASE": "Thompson_BASE.png", "NM": "Thompson_NM.png", "ROUGH": "Thompson_ROUGH.png",
              "MET": "Thompson_MET.png", "AO": "Thompson_AO.png"},
             {"BASE": "textures/m1a1_Base_Color.png", "NM": "textures/m1a1_Normal_OpenGL.png",
              "ROUGH": "textures/m1a1_Roughness.png", "MET": "textures/m1a1_Metallic.png",
              "AO": "textures/m1a1_Mixed_AO.png"})


def load_map(src, name, size, mode):
    names = next(n for n in MAP_NAMES if os.path.exists(os.path.join(src, n["BASE"])))
    im = Image.open(os.path.join(src, names[name])).convert(mode)
    return im.resize((size, size), Image.LANCZOS) if im.size != (size, size) else im


def textures(src, out):
    os.makedirs(os.path.join(out, TEX_DIR), exist_ok=True)
    base = np.asarray(load_map(src, "BASE", TEX_SIZE, "RGB")).astype(np.float32) / 255.0
    ao = np.asarray(load_map(src, "AO", TEX_SIZE, "L")).astype(np.float32)[..., None] / 255.0
    diffuse = base * (1.0 - AO_AMOUNT + AO_AMOUNT * ao)
    Image.fromarray(np.clip(diffuse * 255 + 0.5, 0, 255).astype(np.uint8)).save(
        os.path.join(out, TEX_DIR, "thompson.jpg"), quality=92, subsampling=0)
    from garand import normal_map_green_down
    normal = load_map(src, "NM", TEX_SIZE, "RGB")
    if not normal_map_green_down(normal):   # GL2's tangent space has green down the image
        r, g, b = normal.split()
        normal = Image.merge("RGB", (r, g.point(lambda v: 255 - v), b))
        print("normal map's green turned to point down the image")
    normal.save(os.path.join(out, TEX_DIR, "thompson_n.jpg"), quality=95, subsampling=0)

    sbase = np.asarray(load_map(src, "BASE", SPEC_SIZE, "RGB")).astype(np.float32) / 255.0
    metal = np.asarray(load_map(src, "MET", SPEC_SIZE, "L")).astype(np.float32)[..., None] / 255.0
    rough = np.asarray(load_map(src, "ROUGH", SPEC_SIZE, "L")).astype(np.float32) / 255.0
    sao = np.asarray(load_map(src, "AO", SPEC_SIZE, "L")).astype(np.float32)[..., None] / 255.0
    spec = np.empty((SPEC_SIZE, SPEC_SIZE, 4), np.float32)
    share = METAL_SHARE * metal
    spec[..., :3] = (0.04 * (1.0 - share) + sbase * share) * (0.5 + 0.5 * sao)
    spec[..., 3] = np.minimum(1.0 - rough, MAX_GLOSS)
    Image.fromarray(np.clip(spec * 255 + 0.5, 0, 255).astype(np.uint8), "RGBA").save(
        os.path.join(out, TEX_DIR, "thompson_s.png"), optimize=True)

    path = os.path.join(out, "scripts", "opm_thompson.shader")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\r\n") as f:
        f.write("// the Thompson remade (tools/weapons/thompson.py)\n"
                "%s\n{\n\tqer_editorimage %s/thompson.tga\n\t{\n\t\tmap %s/thompson.tga\n"
                "\t\trgbGen lightingSpherical\n\t}\n}\n" % (SHADER, TEX_DIR, TEX_DIR))


# ----------------------------------------------------------------------------- tiki

def retarget_tik(text, skd_name, keep_surfaces, anims=None):
    """The game's .tik on our model: its skelmodel and surface lines, and (anims: {animation
    name: our .skc}) those animations' files; the rest as it was."""
    out, done, in_anims, renamed = [], False, False, set()
    for line in text.splitlines():
        words = line.split()
        if words[:1] == ["animations"]:
            in_anims = True
        if in_anims and "surface" in words:   # a frame command hides only the first surface
            for name in keep_surfaces:           # of a name: ours may be split in several
                line = re.sub(r"(\bsurface\s+)%s(?=\s+[+-])" % re.escape(name), r"\g<1>%s*" % name, line)
            words = line.split()
        if anims and in_anims and len(words) >= 2 and words[0] in anims and words[1].lower().endswith(".skc"):
            start = line.index(words[1], len(line) - len(line.lstrip()) + len(words[0]))
            line = line[:start] + anims[words[0]] + line[start + len(words[1]):]
            renamed.add(words[0])
        elif words[:1] == ["skelmodel"]:
            indent = line[:len(line) - len(line.lstrip())]
            line = "%sskelmodel %s" % (indent, skd_name)
        elif words[:1] == ["surface"] and len(words) >= 2:
            if done:
                continue
            indent = line[:len(line) - len(line.lstrip())]
            out.extend("%ssurface %s shader %s" % (indent, s, SHADER) for s in keep_surfaces)
            done = True
            continue
        out.append(line)
    if not done:
        raise SystemExit("no surface lines in the game's .tik")
    if anims and renamed != set(anims):
        raise SystemExit("the game's .tik has no animation %s" % ", ".join(sorted(set(anims) - renamed)))
    return "\r\n".join(out) + "\r\n"


def add_animations(text, copies):
    """copies {new name: (animation, .skc)}: each a copy of that animation of the .tik, its
    block of frame commands included, under the new name and playing the new file; put
    after the animation it copies."""
    lines = text.split("\r\n")
    start = next(i for i, l in enumerate(lines) if l.split()[:1] == ["animations"])
    for new, (old, skc_file) in copies.items():
        at = next((i for i in range(start, len(lines)) if lines[i].split()[:1] == [old]), None)
        if at is None:
            raise SystemExit("the game's .tik has no animation %s" % old)
        end = at + 1
        nxt = next((i for i in range(at + 1, len(lines)) if lines[i].strip()), None)
        if nxt is not None and lines[nxt].strip().startswith("{"):
            depth, end = 0, nxt
            while True:
                code = lines[end].split("//")[0]
                depth += code.count("{") - code.count("}")
                end += 1
                if depth <= 0:
                    break
        words = lines[at].split()
        head = lines[at]
        indent = head[:len(head) - len(head.lstrip())]
        rest = head.lstrip()[len(words[0]):]
        rest = rest.replace(words[1], skc_file, 1)
        lines[end:end] = [indent + new + rest] + lines[at + 1:end]
    return "\r\n".join(lines)


def write(out, path, data):
    full = os.path.join(out, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as f:
        f.write(data)


# -----------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--src", default=os.path.join(GAME, "models", "Thompson"))
    ap.add_argument("--game", default=GAME)
    args = ap.parse_args()

    obj = os.path.join(args.src, "model_0.obj")
    if os.path.exists(obj):
        mesh = Mesh(obj)
    else:   # the FBX download: source/*.rar, its one mesh
        import glob
        from remodel import read_glb, unpack, via_blender
        got = sorted(glob.glob(os.path.join(args.src, "source", "*.rar")))
        fbx = sorted(glob.glob(os.path.join(unpack(got[0], "thompson"), "*.fbx"))) if got else []
        if not fbx:
            raise SystemExit("no source model in %s" % args.src)
        pieces = read_glb(via_blender(fbx, "thompson", decimate=FBX_DECIMATE))
        if len(pieces) != 1:
            raise SystemExit("%s has %d meshes, not one" % (fbx[0], len(pieces)))
        mesh = Mesh.from_piece(pieces[0])
    fs = vfs.GameFS([os.path.join(args.game, "main")], skip_paks=[PAK])
    gun = skd.read(fs.read(GUN_SKD))
    clip = skd.read(fs.read(CLIP_SKD))
    rng = np.random.default_rng(1)

    s, r, t = fit_gun(mesh, gun, rng)
    pos, normal = mesh.transformed(s, r, t)

    pos, normal = filled_forend(pos, normal, mesh.tris)

    corners = pos[mesh.tris]
    (x0, x1), (y0, y1), (z0, z1) = MAG_BOX
    in_mag = ((corners[..., 0] > x0) & (corners[..., 0] < x1) & (corners[..., 1] > y0)
              & (corners[..., 1] < y1) & (corners[..., 2] > z0) & (corners[..., 2] < z1)).all(1)
    # and the pieces centred in the box whole (the FBX's magazine body runs on up into the
    # receiver, its floor plate past the box's sides)
    lab = parts(pos, mesh.tris)
    lo, hi = np.array(MAG_BOX).T
    for k in np.unique(lab):
        c = pos[np.unique(mesh.tris[lab == k])].mean(0)
        if (c > lo).all() and (c < hi).all():
            in_mag |= lab == k
    if not 40 <= in_mag.sum() <= 3000:
        raise SystemExit("the magazine cut took %d triangles" % in_mag.sum())
    body, mag = mesh.tris[~in_mag].tolist(), mesh.tris[in_mag].tolist()
    print("%d triangles: %d gun, %d magazine" % (len(mesh.tris), len(body), len(mag)))

    in_bolt = bolt_triangles(pos, mesh.tris)
    fitted = pos.copy()  # where the game's magazine is: the reload's magazine goes there

    # the sights onto the aim pose's line (after the cuts, whose boxes are the fit's)
    eye = aim_eye(fs, gun)
    sr, hole, new_hole = sight_fix(pos, mesh.tris, eye + AIM_SHIFT, stock_post(gun) + AIM_SHIFT + AIM_POST)
    pos = (pos - hole) @ sr.T + new_hole
    normal = normal @ sr.T
    slide = sr @ np.array([0.0, 0.0, 1.0])  # along the receiver

    # the knob on its own bone, its vertices in that bone's space (the bone at the knob's
    # centre as modelled, back; the animations place it)
    bolt_verts = np.unique(mesh.tris[in_bolt])
    back = pos[bolt_verts].mean(0)
    gun_bone = gun.bone_index(GUN_BONE)
    bone_of = np.full(len(pos), gun_bone)
    bone_of[bolt_verts] = len(gun.bones)
    gun_pos = pos.copy()
    gun_pos[bolt_verts] -= back
    print("cocking knob: %d triangles, at %s" % (in_bolt.sum(), np.round(back, 2)))

    gun_surfs = surfaces([("ThompsonSMG1", body), ("Clip", mag)], gun_pos, normal, mesh.uv, bone_of)
    bolt = skd.Bone(BOLT_BONE, parent=GUN_BONE, type=skd.POSROT)
    gun_model = model_like(gun, "opm_thompson.skd", gun_surfs, [bolt])
    write(args.out, "models/weapons/thompsonsmg/opm_thompson.skd", skd.write(gun_model))
    print("opm_thompson.skd: %d surfaces, %d vertices" % (len(gun_surfs), sum(len(x.verts) for x in gun_surfs)))

    idle, fire = read_anim(fs, GUN_IDLE_SKC), read_anim(fs, GUN_FIRE_SKC)

    def shot(curve):
        return (list(curve) + [curve[-1]] * fire.num_frames)[:fire.num_frames]

    ours = {
        "idle": with_bolt(idle, [1.0] * idle.num_frames, back, slide),
        "idle_empty": with_bolt(idle, [0.0] * idle.num_frames, back, slide),
        "reload_empty": with_bolt(idle, held(RELOAD_EMPTY_PULL, idle.num_frames), back, slide),
        "fire": with_bolt(fire, shot(FIRE_BOLT), back, slide),
        "fire_empty": with_bolt(fire, shot(FIRE_EMPTY_BOLT), back, slide),
    }
    for kind, anim in ours.items():
        write(args.out, OUR_SKC % kind, skc.write(anim))
    print("animations: idle and reload %d frames, fire %d" % (idle.num_frames, fire.num_frames))
    skc_name = {k: os.path.basename(OUR_SKC % k) for k in ours}

    cr, ct = fit_clip(gun, clip, rng)
    cpos, cnormal = fitted @ cr.T + ct, (normal @ sr) @ cr.T
    clip_surfs = surfaces([("clip", mag)], cpos, cnormal, mesh.uv, clip.bone_index(CLIP_BONE))
    write(args.out, "models/ammo/thompsonclip/opm_thompsonclip.skd",
          skd.write(model_like(clip, "opm_thompsonclip.skd", clip_surfs)))

    anims = {"idle": skc_name["idle"], "secondaryfire": skc_name["idle"],
             "reload": skc_name["idle"], "fire": skc_name["fire"]}
    # the empty gun's: each a copy of its loaded one (frame commands and all)
    empties = {"idle_empty": ("idle", skc_name["idle_empty"]),
               "secondaryfire_empty": ("secondaryfire", skc_name["idle_empty"]),
               "reload_empty": ("reload", skc_name["reload_empty"]),
               "fire_empty": ("fire", skc_name["fire_empty"])}
    tik = retarget_tik(fs.read_text(GUN_TIK), "opm_thompson.skd", ["ThompsonSMG1", "Clip"], anims)
    write(args.out, GUN_TIK, add_animations(tik, empties).encode("latin-1"))
    write(args.out, CLIP_TIK, retarget_tik(fs.read_text(CLIP_TIK), "opm_thompsonclip.skd",
                                           ["all"]).encode("latin-1"))
    textures(args.src, args.out)
    print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
