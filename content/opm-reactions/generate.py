#!/usr/bin/env python3
"""The animations opm-reactions adds, worked out from the game's own.

A man shot in the back flinches forward. The game has no animation for it:
every stand pain animation it has, "hit_lower_back" among them, throws the head
back (measured: head from 20 units ahead of the pelvis to 8 to 13 behind). So
each weapon's back hit is turned round here: the pelvis and the back, neck and
head move by the same rotation they did, but the other way, about where they
started. The arms hang off the chest as they did, so the weapon stays in the
hands, and the feet (IK targets) stay where they stood.

    generate.py --game "/mnt/d/Medal of Honor" --out DIR

A man knocked down gets up again by an animation motion captured for it: two
takes of the CMU motion capture database (subject 140, "Getting Up From
Ground"), downloaded once (no restrictions on their use) and retargeted
(tools/mocap/retarget.py). anim/opm_getups.scr says, for each, where its pelvis
starts and which way its head points, relative to where he ends up standing,
so anim/opm_knockdown.scr can start it where his ragdoll lies.

Run by tools/content/pack.py; writes models/human/animation/opm/*.skc and
anim/opm_getups.scr.
"""
import argparse
import io
import shutil
import math
import os
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "mocap"))

# weapon group -> the animation its back (or, lacking one, lower torso) hit plays
SOURCES = {
    "rifle":    "weapon_rifle/minor_pain/rifle_stand_hit_lower_back.skc",
    "mp40":     "weapon_mp40/minor_pain/mp40_stand_hit_lowerback.skc",
    "mp44":     "weapon_mp44/minor_pain/mp44_stand_hit_lowerback.skc",
    "pistol":   "weapon_pistol/minor_pain/pistol_stand_hit_lowerback.skc",
}
# (A man with a grenade or nothing in his hands already doubles forward when
# hit in the lower torso; opm_reactions.tik gives him that one as it is.)

# The get-ups: takes of the CMU database (subjects 139 and 140), each also
# mirrored where that gives the other side. Which way up each starts is worked
# out from it; each is cut to just before he starts to rise and just after he
# stands, and played faster than captured (a man under fire does not take his
# time).
CMU = "https://raw.githubusercontent.com/Shriinivas/cmubvh/main/Sequence-136-140/%s/Data/%s.zip"
GETUP_TAKES = [
    ("140_01", False), ("140_02", False), ("139_16", False), ("139_17", False),  # face down
    ("140_08", False), ("140_09", False),                                        # on the back
    ("140_03", False), ("140_03", True),                                         # on a side
    ("140_04", False), ("140_04", True),
    ("139_18", False), ("139_18", True),
]
GETUP_SPEED = 1.4
# How long before his head starts to come up the cut begins (the push up with
# the arms comes first), and after he is up it ends, seconds.
GETUP_LEAD = 1.0
GETUP_TAIL = 0.4

CACHE = os.path.expanduser("~/.cache/openmohaa-mocap")

# How long a man takes to straighten up again where the turned-round animation
# leaves him bent, seconds.
RECOVER = 0.4

# How much of the motion is kept, turned round: all of it doubled the man over.
SHARE = 0.8

# The bones turned round, and the root moved the other way.
TURNED = ["Bip01 Pelvis", "Bip01 Spine", "Bip01 Spine1", "Bip01 Spine2", "Bip01 Neck", "Bip01 Head"]


def slerp(a, b, t, np):
    if np.dot(a, b) < 0:
        b = -b
    d = min(1.0, float(np.dot(a, b)))
    th = np.arccos(d)
    q = a + (b - a) * t if th < 1e-5 else (np.sin((1 - t) * th) * a + np.sin(t * th) * b) / np.sin(th)
    return q / np.linalg.norm(q)


def scaled(q, share, np):
    """The rotation q (a channel quaternion), by share of the way from none."""
    w = max(-1.0, min(1.0, q[3]))
    angle = 2.0 * np.arccos(w)
    s = np.sqrt(max(1e-12, 1.0 - w * w))
    axis = q[:3] / s
    a = angle * share / 2.0
    return np.array([*(axis * np.sin(a)), np.cos(a)])


def turned_round(anim, M, np):
    names = anim["names"]
    values = anim["values"].copy()
    first = values[0]
    for c, n in enumerate(names):
        if n.endswith(" rot") and n[:-4] in TURNED:
            r0 = M.channel_to_rot(first[c])
            for i in range(len(values)):
                # the frame's turn from the first, undone instead of done
                delta = M.rot_to_channel(r0.T @ M.channel_to_rot(values[i, c]))
                back = scaled(np.array([-delta[0], -delta[1], -delta[2], delta[3]]), SHARE, np)
                values[i, c] = M.rot_to_channel(r0 @ M.channel_to_rot(back))
        elif n == "Bip01 pos":
            for i in range(len(values)):
                moved = values[i, c, :2] - first[c, :2]
                values[i, c, :2] = first[c, :2] - moved * SHARE
    # Ending away from where it began (the rifle's is four frames that never
    # come back upright), the man is eased back there, or he stays bent over
    # until the next animation takes him.
    steps = int(round(RECOVER / anim["frame_time"]))
    turned = [c for c, n in enumerate(names) if n.endswith(" rot") and n[:-4] in TURNED]
    if steps and not np.allclose(values[-1, turned], values[0, turned], atol=0.02):
        last, extra = values[-1], []
        for k in range(1, steps + 1):
            t = k / steps
            t = t * t * (3 - 2 * t)
            frame = last.copy()
            for c, n in enumerate(names):
                if n.endswith(" rot"):
                    frame[c] = slerp(last[c], values[0, c], t, np)
                else:
                    frame[c] = last[c] + (values[0, c] - last[c]) * t
            extra.append(frame)
        values = np.concatenate([values, np.array(extra)])
    out = dict(anim)
    out["values"] = values
    out["meta"] = anim["meta"] + [anim["meta"][-1]] * (len(values) - len(anim["meta"]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    os.environ["MOHAA_GAME"] = a.game
    import numpy as np
    import mohskel as M
    from retarget import load_game_file

    folder = os.path.join(a.out, "models", "human", "animation", "opm")
    os.makedirs(folder, exist_ok=True)
    for group, src in SOURCES.items():
        anim = M.read_skc(load_game_file("models/human/animation/" + src))
        name = os.path.basename(src)[:-4] + "_fwd.skc"
        with open(os.path.join(folder, name), "wb") as f:
            f.write(M.write_skc(turned_round(anim, M, np)))

    getups(a.out, M, load_game_file, np)


def take(name):
    """A CMU take's BVH, downloaded once."""
    path = os.path.join(CACHE, name + ".bvh")
    if not os.path.exists(path):
        os.makedirs(CACHE, exist_ok=True)
        with urllib.request.urlopen(CMU % (name.split("_")[0], name)) as r:
            z = zipfile.ZipFile(io.BytesIO(r.read()))
        bvh = next(n for n in z.namelist() if n.endswith(".bvh"))
        with open(path, "wb") as f:
            f.write(z.read(bvh))
    return path


def cut(path, np):
    """Where in a take to start and end: before he rises, and once he is up."""
    from bvh import Bvh
    from retarget import C

    clip = Bvh(open(path).read())
    heads = np.array([(C @ clip.pose(f)["Head"][1])[2] for f in range(1, clip.num_frames)])
    top = heads[-int(1.0 / clip.frame_time):].mean()
    rising = int(np.argmax(heads > heads[0] + (top - heads[0]) * 0.15))
    up = int(np.argmax(heads > heads[0] + (top - heads[0]) * 0.95))
    start = max(0.1, (rising + 1) * clip.frame_time - GETUP_LEAD)
    end = min((clip.num_frames - 1) * clip.frame_time, (up + 1) * clip.frame_time + GETUP_TAIL)
    return start, end


def lying(pose, np):
    """Which way up a pose lies, as the client tells it (cg_ragdoll.cpp
    CG_RagdollReportDown): front (face down), back, left or right (that side
    down)."""
    along = pose["Bip01 Head"][1] - pose["Bip01 Pelvis"][1]
    right = pose["Bip01 R UpperArm"][1] - pose["Bip01 L UpperArm"][1]
    fwd = np.cross(along, right)
    fwd /= np.linalg.norm(fwd)
    if fwd[2] > 0.5:
        return "back"
    if fwd[2] < -0.5:
        return "front"
    return "left" if right[2] > 0 else "right"


def getups(out, M, load_game_file, np):
    from retarget import SKELETON

    bones = M.read_skd(load_game_file(SKELETON))
    template = "models/human/animation/idle/rifle_idle_normal01(loop).skc"
    kinds = {k: [] for k in ("front", "back", "left", "right")}
    folder = os.path.join(out, "models", "human", "animation", "opm")
    work = os.path.join(out, "_getups")
    os.makedirs(work, exist_ok=True)

    for name, mirror in GETUP_TAKES:
        path = take(name)
        start, end = cut(path, np)
        target = os.path.join(work, "%s%s.skc" % (name, "m" if mirror else ""))
        args = [sys.executable, os.path.join(HERE, "..", "..", "tools", "mocap", "retarget.py"), path, target,
                "--template", template, "--start", "%.2f" % start, "--end", "%.2f" % end, "--speed", str(GETUP_SPEED),
                "--anchor", "end", "--ground", "lowest"]
        if mirror:
            args.append("--mirror")
        subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
        with open(target, "rb") as f:
            data = f.read()
        pose = M.fk(bones, M.frame_channels(M.read_skc(data), 0))
        kind = lying(pose, np)
        pelvis, head = pose["Bip01 Pelvis"][1], pose["Bip01 Head"][1]
        yaw = math.degrees(math.atan2(head[1] - pelvis[1], head[0] - pelvis[0]))
        kinds[kind].append((data, pelvis[0], pelvis[1], yaw, name + (" mirrored" if mirror else "")))

    # The animations, their aliases, and for each where it starts.
    script = ["// Generated by content/opm-reactions/generate.py. For each way a man can",
              "// lie (front: face down, back, left or right side down): how many get-ups,",
              "// and for each where its pelvis starts (forward and left of where he ends up",
              "// standing) and which way its head points then (degrees from his facing).",
              "// anim/opm_knockdown.scr", "", "count local.kind:"]
    tiki = ["animations", "{", "\t$path models/human/animation", "",
            "\t// Generated by content/opm-reactions/generate.py: getting up again,", "\t// anim/opm_knockdown.scr"]
    data_lines = ["", "data local.kind local.i:"]
    for kind, items in kinds.items():
        script.append('\tif (local.kind == "%s")\n\t\tend %d' % (kind, len(items)))
        for i, (data, sx, sy, yaw, source) in enumerate(items, 1):
            name = "getup_%s%d.skc" % (kind, i)
            with open(os.path.join(folder, name), "wb") as f:
                f.write(data)
            tiki.append("\topm_getup_%s%d\t\topm/%s\t// CMU %s" % (kind, i, name, source))
            data_lines.append('\tif (local.kind == "%s" && local.i == %d)\n\t\tend ( %.1f %.1f %.1f )' % (kind, i, sx, sy, yaw))
    script.append("end 0")
    data_lines.append("end ( 0 0 0 )")
    tiki.append("}")
    shutil.rmtree(work)

    os.makedirs(os.path.join(out, "anim"), exist_ok=True)
    with open(os.path.join(out, "anim", "opm_getups.scr"), "w", newline="\r\n") as f:
        f.write("\n".join(script + data_lines) + "\n")
    with open(os.path.join(out, "models", "human", "animation", "opm_getups.tik"), "w", newline="\r\n") as f:
        f.write("\n".join(tiki) + "\n")
    print("get-ups: " + ", ".join("%s %d" % (k, len(v)) for k, v in kinds.items()))


if __name__ == "__main__":
    main()
