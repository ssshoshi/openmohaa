#!/usr/bin/env python3
"""Contact sheet of a .skc posed the way the game poses it, beside its source BVH.

    tools/mocap/preview.py out.skc sheet.png [--bvh clip.bvh --start 1 --fps 30] [--frames 8]

Each column is one frame: the game skeleton from the front and from the side,
and under it the mocap the frame came from (same view, its own scale). A
.skc given as a game path (models/...) is read from the paks.
"""
import argparse
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mohskel as M  # noqa: E402
from bvh import Bvh  # noqa: E402
from retarget import C, SKELETON, facing, load_game_file  # noqa: E402

SEGMENTS = [
    ("Bip01 Pelvis", "Bip01 Spine"), ("Bip01 Spine", "Bip01 Spine1"), ("Bip01 Spine1", "Bip01 Spine2"),
    ("Bip01 Spine2", "Bip01 Neck"), ("Bip01 Neck", "Bip01 Head"),
]
for s in "LR":
    SEGMENTS += [
        ("Bip01 Spine2", "Bip01 %s Clavicle" % s), ("Bip01 %s Clavicle" % s, "Bip01 %s UpperArm" % s),
        ("Bip01 %s UpperArm" % s, "Bip01 %s Forearm" % s), ("Bip01 %s Forearm" % s, "Bip01 %s Hand" % s),
        ("Bip01 Pelvis", "Bip01 %s Thigh" % s), ("Bip01 %s Thigh" % s, "Bip01 %s Calf" % s),
        ("Bip01 %s Calf" % s, "Bip01 %s Foot" % s), ("Bip01 %s Foot" % s, "Bip01 %s Toe0" % s),
    ]


def draw(ax, segs, axes, color_l="tab:blue", color_r="tab:red"):
    for a, b, side in segs:
        c = color_l if side == "L" else color_r if side == "R" else "k"
        ax.plot([a[axes[0]], b[axes[0]]], [a[axes[1]], b[axes[1]]], color=c, lw=2)
    ax.set_aspect("equal")
    ax.axis("off")


def side_of(name):
    return "L" if " L " in name or name.startswith("Left") else "R" if " R " in name or name.startswith("Right") else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("skc")
    ap.add_argument("out")
    ap.add_argument("--bvh")
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--frames", type=int, default=8)
    ap.add_argument("--mirror", action="store_true")
    ap.add_argument("--loop", type=float, default=0.0, help="as given to retarget.py: the clip frames are offset by it")
    a = ap.parse_args()

    bones = M.read_skd(load_game_file(SKELETON))
    data = load_game_file(a.skc) if a.skc.startswith("models/") else open(a.skc, "rb").read()
    anim = M.read_skc(data)
    n = len(anim["values"])
    picks = np.linspace(0, n - 1, a.frames).round().astype(int)

    clip = Bvh(open(a.bvh).read()) if a.bvh else None
    if clip:
        clip.mirror = a.mirror
    if clip:
        step = max(1, round(1.0 / (a.fps * clip.frame_time)))
        first = max(1, round(a.start / clip.frame_time))
        turn = facing(clip, first) @ C
    rows = 2 + (2 if clip else 0)
    fig, axs = plt.subplots(rows, a.frames, figsize=(2.2 * a.frames, 3.2 * rows))
    for col, i in enumerate(picks):
        pose = M.fk(bones, M.frame_channels(anim, i))
        segs = [(pose[x][1], pose[y][1], side_of(y)) for x, y in SEGMENTS]
        draw(axs[0, col], segs, (1, 2))   # front: Y across (left on the right), Z up
        axs[0, col].invert_xaxis()
        draw(axs[1, col], segs, (0, 2))   # side: X (forward) across
        axs[0, col].set_title("%d (%.1fs)" % (i, i * anim["frame_time"]), fontsize=8)
        if clip:
            f = first + (i + int(round(a.loop * a.fps))) * step
            if f < clip.num_frames:
                mp = clip.pose(f)
                msegs = []
                for j in clip.joints:
                    if j["parent"] >= 0:
                        p = clip.joints[j["parent"]]["name"]
                        msegs.append((turn @ mp[p][1], turn @ mp[j["name"]][1], side_of(j["name"])))
                draw(axs[2, col], msegs, (1, 2))
                axs[2, col].invert_xaxis()
                draw(axs[3, col], msegs, (0, 2))
    fig.tight_layout()
    fig.savefig(a.out, dpi=70)
    print(a.out)


if __name__ == "__main__":
    main()
