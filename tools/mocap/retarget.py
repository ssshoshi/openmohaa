#!/usr/bin/env python3
"""Retarget a BVH clip onto the MOHAA Bip01 skeleton and write a .skc.

    tools/mocap/retarget.py clip.bvh out.skc --template models/human/animation/idle/standidle01.skc
        [--start 2.0] [--end 12.0] [--fps 30] [--loop 0.5]

The BVH must start with a T-pose facing +Z, Y up (the cgspeed conversion of
the CMU database does). Each mapped bone gets the world-space rotation its
mocap bone made since that T-pose, applied on top of the same bone in a Bip01
T-pose: a frame of the template animation with every bone swung to point the
way the mocap bone does in the T-pose. Swinging rather than rebuilding keeps
the template's twist, so palms, feet and the head start out the way the game's
own animations hold them.

The legs are IK in the game, driven only by the feet: each foot is put where
the mocap ankle is relative to its hip, scaled by leg length, and turned the
way the mocap foot turns. The knee then bends in the plane the engine picks.

Channels nothing maps (fingers, tags, the eyes and camera bones) keep the
template's first frame. The clip is turned to face +X at its first frame and
moved over the origin; root motion stays in the pose (delta 0), which is what
an idle wants.
"""
import argparse
import math
import os
import sys
import zipfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mohskel as M  # noqa: E402
from bvh import Bvh  # noqa: E402

GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")
SKELETON = "models/human/allied_army_soldier/usarmy.skd"

# mocap Y up, facing +Z, left +X  ->  MOHAA Z up, facing +X, left +Y
C = np.array([[0, 0, 1], [1, 0, 0], [0, 1, 0]], dtype=float)

# Bip01 bone <- CMU (cgspeed) joint, and for the T-pose swing the pair of
# joints whose direction the bone should take: (bip from, bip to) ~ (mocap from, mocap to).
# A "+X" Bip01 end means the bone's own local X axis (no child bone to aim at).
BONES = [
    ("Bip01 Pelvis", "Hips", None),
    ("Bip01 Spine", "LowerBack", (("Bip01 Spine", "Bip01 Spine1"), ("Hips", "Spine"))),
    ("Bip01 Spine1", "Spine", (("Bip01 Spine1", "Bip01 Spine2"), ("Spine", "Spine1"))),
    ("Bip01 Spine2", "Spine1", (("Bip01 Spine2", "Bip01 Neck"), ("Spine1", "Neck1"))),
    ("Bip01 Neck", "Neck1", (("Bip01 Neck", "Bip01 Head"), ("Neck1", "Head"))),
    ("Bip01 Head", "Head", (("Bip01 Head", "+X"), ("Head", "Head.end"))),
]
for _s, _m in (("L", "Left"), ("R", "Right")):
    BONES += [
        ("Bip01 %s Clavicle" % _s, _m + "Shoulder",
         (("Bip01 %s Clavicle" % _s, "Bip01 %s UpperArm" % _s), (_m + "Shoulder", _m + "Arm"))),
        ("Bip01 %s UpperArm" % _s, _m + "Arm",
         (("Bip01 %s UpperArm" % _s, "Bip01 %s Forearm" % _s), (_m + "Arm", _m + "ForeArm"))),
        ("Bip01 %s Forearm" % _s, _m + "ForeArm",
         (("Bip01 %s Forearm" % _s, "Bip01 %s Hand" % _s), (_m + "ForeArm", _m + "Hand"))),
        ("Bip01 %s Hand" % _s, _m + "Hand",
         (("Bip01 %s Hand" % _s, "+X"), (_m + "Hand", _m + "HandIndex1"))),
        ("Bip01 %s Foot" % _s, _m + "Foot",
         (("Bip01 %s Foot" % _s, "Bip01 %s Toe0" % _s), (_m + "Foot", _m + "ToeBase"))),
        ("Bip01 %s Toe0" % _s, _m + "ToeBase", None),
    ]
LEGS = (("L", "Left"), ("R", "Right"))


def swing(a, b):
    """The shortest rotation taking direction a to direction b."""
    a, b = a / np.linalg.norm(a), b / np.linalg.norm(b)
    v, c = np.cross(a, b), float(np.dot(a, b))
    if c < -0.9999:
        p = np.cross(a, [1, 0, 0] if abs(a[0]) < 0.9 else [0, 1, 0])
        p /= np.linalg.norm(p)
        return 2 * np.outer(p, p) - np.eye(3)
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k / (1 + c)


def yaw(deg):
    a = math.radians(deg)
    return np.array([[math.cos(a), -math.sin(a), 0], [math.sin(a), math.cos(a), 0], [0, 0, 1]])


def load_game_file(path):
    """The copy of a game file the game loads: the last pak in name order wins."""
    main = os.path.join(GAME, "main")
    found = None
    for pak in sorted(os.listdir(main), key=str.lower):
        if not pak.lower().endswith(".pk3"):
            continue
        z = zipfile.ZipFile(os.path.join(main, pak))
        for n in z.namelist():
            if n.lower() == path.lower():
                found = z.read(n)
    if found is None:
        raise SystemExit("not in any pak: " + path)
    return found


class Retarget:
    def __init__(self, clip, bones, template):
        self.clip, self.bones, self.tmpl = clip, bones, template
        self.tmpl_ch = M.frame_channels(template, 0)
        stock = M.fk(bones, self.tmpl_ch)
        tpose = clip.pose(0)

        def mpos(pose, name):
            if name.endswith(".end"):
                return C @ clip.tip(pose, name[:-4])
            return C @ pose[name][1]

        # Bip01 T-pose world rotations: the template's, swung onto the mocap directions.
        self.ref, self.mref = {}, {}
        for bip, moc, aim in BONES:
            g = stock[bip][0]
            if aim:
                (bf, bt), (mf, mt) = aim
                d_bip = g[:, 0] if bt == "+X" else stock[bt][1] - stock[bf][1]
                d_moc = mpos(tpose, mt) - mpos(tpose, mf)
                g = swing(d_bip, d_moc) @ g
            self.ref[bip] = g
            self.mref[bip] = C @ tpose[moc][0]

        # Uniform scale from leg length: thigh base to foot in the game, hip to ankle in the clip.
        chains = M.ik_chains(bones)
        elbow, wrist = chains["Bip01 L Thigh"]
        bip_leg = elbow["length"] + wrist["length"]
        moc_leg = (np.linalg.norm(tpose["LeftLeg"][1] - tpose["LeftUpLeg"][1])
                   + np.linalg.norm(tpose["LeftFoot"][1] - tpose["LeftLeg"][1]))
        self.scale = bip_leg / moc_leg

        self.pelvis_local_rot = M.channel_to_rot(self.tmpl_ch["Bip01 Pelvis rot"])
        self.pelvis_local_pos = np.array(self.tmpl_ch["Bip01 Pelvis pos"][:3])
        self.thigh_offset = {s: next(b for b in bones if b["name"] == "Bip01 %s Thigh" % s)["offset"]
                             for s, _ in LEGS}
        self.foot_ground = stock["Bip01 L Foot"][1][2]
        self.ankle_ground = (C @ tpose["LeftFoot"][1])[2]

    def frame(self, f, turn, shift):
        """Channels for clip frame f, turned by `turn` and moved by `shift` (game units)."""
        pose = self.clip.pose(f)
        world = {}
        for bip, moc, _ in BONES:
            delta = turn @ (C @ pose[moc][0]) @ self.mref[bip].T
            world[bip] = delta @ self.ref[bip]

        s = self.scale
        hips = turn @ (C @ pose["Hips"][1]) * s + shift
        hips[2] = (C @ pose["Hips"][1])[2] * s - self.ankle_ground * s + self.foot_ground

        ch = dict(self.tmpl_ch)
        g_pel = world["Bip01 Pelvis"]
        g_bip = g_pel @ self.pelvis_local_rot.T
        p_bip = hips - g_bip @ self.pelvis_local_pos
        ch["Bip01 rot"] = M.rot_to_channel(g_bip)
        ch["Bip01 pos"] = np.array([*p_bip, 0.0])

        parents = {b["name"]: b["parent"] for b in self.bones}
        for bip, _, _ in BONES:
            if bip == "Bip01 Pelvis" or bip.endswith("Foot"):
                continue
            parent = parents[bip]
            gp = g_pel if parent == "Bip01 Pelvis" else world[parent]
            ch[bip + " rot"] = M.rot_to_channel(gp.T @ world[bip])

        for side, moc in LEGS:
            hip_game = hips + g_pel @ self.thigh_offset[side]
            leg = turn @ (C @ (pose[moc + "Foot"][1] - pose[moc + "UpLeg"][1])) * s
            foot = world["Bip01 %s Foot" % side]
            # The wrist (foot) is in model space: no parent.
            ch["Bip01 %s Foot rot" % side] = M.rot_to_channel(foot)
            ch["Bip01 %s Foot pos" % side] = np.array([*(hip_game + leg), 0.0])
            toe = world["Bip01 %s Toe0" % side]
            ch["Bip01 %s Toe0 rot" % side] = M.rot_to_channel(foot.T @ toe)
        return ch


def facing(clip, f):
    """The turn about Z putting the clip's frame f facing +X."""
    pose = clip.pose(f)
    left = C @ (pose["LeftUpLeg"][1] - pose["RightUpLeg"][1])
    heading = math.degrees(math.atan2(left[1], left[0])) - 90.0  # left is +Y when facing +X
    return yaw(-heading)


def bounds_of(bones, ch):
    pts = np.array([t[1] for t in M.fk(bones, ch).values()])
    lo, hi = pts.min(0) - 12.0, pts.max(0) + 12.0
    lo[2] = min(lo[2], 0.0)
    radius = float(np.linalg.norm(np.maximum(abs(lo), abs(hi))))
    return (tuple(lo), tuple(hi)), radius


def slerp(a, b, t):
    a, b = np.asarray(a, float), np.asarray(b, float)
    if np.dot(a, b) < 0:
        b = -b
    d = min(1.0, float(np.dot(a, b)))
    th = math.acos(d)
    if th < 1e-5:
        q = a + (b - a) * t
    else:
        q = (math.sin((1 - t) * th) * a + math.sin(t * th) * b) / math.sin(th)
    return q / np.linalg.norm(q)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bvh")
    ap.add_argument("out")
    ap.add_argument("--template", required=True, help="game path of a stock .skc to take unmapped channels from")
    ap.add_argument("--start", type=float, default=0.0, help="seconds into the clip (the T-pose frame is skipped)")
    ap.add_argument("--end", type=float, default=None)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--loop", type=float, default=0.0, help="seconds at the end blended into the start")
    a = ap.parse_args()

    clip = Bvh(open(a.bvh).read())
    bones = M.read_skd(load_game_file(SKELETON))
    tmpl = M.read_skc(load_game_file(a.template))
    rt = Retarget(clip, bones, tmpl)

    step = max(1, round(1.0 / (a.fps * clip.frame_time)))
    first = max(1, round(a.start / clip.frame_time))
    last = clip.num_frames - 1 if a.end is None else min(clip.num_frames - 1, round(a.end / clip.frame_time))
    frames = list(range(first, last + 1, step))
    turn = facing(clip, first)
    hp = turn @ (C @ clip.pose(first)["Hips"][1]) * rt.scale
    shift = np.array([-hp[0], -hp[1], 0.0])  # over the origin

    names = tmpl["names"]
    chans = [rt.frame(f, turn, shift) for f in frames]

    n_blend = int(round(a.loop * a.fps))
    if n_blend:
        # The last n frames ease into the first n, so the end matches the start.
        head = chans[:n_blend]
        chans = chans[n_blend:]
        for i in range(n_blend):
            t = (i + 1) / (n_blend + 1)
            src, dst = chans[len(chans) - n_blend + i], head[i]
            for n in names:
                if n.endswith(" rot"):
                    src[n] = slerp(src[n], dst[n], t)
                elif n.endswith(" pos"):
                    src[n] = np.asarray(src[n]) * (1 - t) + np.asarray(dst[n]) * t

    values = np.array([[np.asarray(c[n], float) for n in names] for c in chans])
    meta = []
    for c in chans:
        b, r = bounds_of(bones, c)
        meta.append(dict(bounds=b, radius=r, delta=(0.0, 0.0, 0.0), angle_delta=0.0))
    anim = dict(flags=tmpl["flags"], frame_time=clip.frame_time * step, total_delta=np.zeros(3),
                total_angle=0.0, names=names, values=values, meta=meta)
    open(a.out, "wb").write(M.write_skc(anim))
    print("%s: %d frames at %.1f fps, scale %.2f" % (a.out, len(chans), 1.0 / anim["frame_time"], rt.scale))


if __name__ == "__main__":
    main()
