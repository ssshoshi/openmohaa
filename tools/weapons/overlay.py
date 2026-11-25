#!/usr/bin/env python3
"""A check of remodel.py's work: our model over the game's, posed by the game's idle
animation (or another frame of another animation), drawn from three sides. The game's in
grey, ours one colour a surface name.

    tools/weapons/overlay.py build/opm-weapons colt45 [--anim reload_colt45.skc --frame 20]
"""

import argparse
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from remodel import GAME, OUR_PAKS, Stock, read_anim, skd, vfs, pose, sample  # noqa: E402
from weapon_specs import WEAPONS  # noqa: E402


def posed(model, held):
    names = [b.name.lower() for b in model.bones]
    out = []
    for s in model.surfaces:
        pts = []
        for v in s.verts:
            p = np.zeros(3)
            for bi, bw, off in v.weights:
                m = held[names[bi]]
                p += bw * (m[:3, :3] @ np.array(off) + m[:3, 3])
            pts.append(p)
        bone = names[v.weights[0][0]] if s.verts else ""
        name = s.name.split("_")[0] if s.name.rsplit("_", 1)[-1].isdigit() else s.name
        out.append(("%s@%s" % (name, bone), np.array(pts), np.array(s.triangles, int)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("built")
    ap.add_argument("key")
    ap.add_argument("--anim")
    ap.add_argument("--frame", type=int, default=0)
    ap.add_argument("--out")
    args = ap.parse_args()
    spec = next(w for w in WEAPONS if w["key"] == args.key)
    fs = vfs.GameFS([os.path.join(GAME, "main")], skip_paks=OUR_PAKS)
    stock = Stock(fs, spec["tik"])
    ours = skd.read(open(os.path.join(args.built, stock.dir, "opm_%s.skd" % args.key), "rb").read())
    anim = read_anim(fs, stock.dir + "/" + args.anim) if args.anim else stock.idle_anim
    held = {k: np.array(v) for k, v in pose.Skeleton(stock.model.bones).evaluate(
        pose.frame_channels(anim, args.frame)).items()}
    rng = np.random.default_rng(0)
    theirs = posed(stock.model, held)
    mine = posed(ours, {k: np.array(v) for k, v in pose.Skeleton(ours.bones).evaluate(
        pose.frame_channels(anim, args.frame)).items()})
    allp = np.concatenate([p for _, p, _ in theirs + mine])
    lo, hi = allp.min(0), allp.max(0)
    a, b, c = np.argsort(hi - lo)[::-1]
    xyz = "XYZ"
    views = [(a, b, "%s across, %s up" % (xyz[a], xyz[b])), (a, c, "%s across, %s up" % (xyz[a], xyz[c])),
             (c, b, "%s across, %s up" % (xyz[c], xyz[b]))]
    fig, axes = plt.subplots(3, 1, figsize=(16, 15))
    names = sorted({n for n, _, _ in mine})
    colours = plt.cm.tab10(np.linspace(0, 1, max(10, len(names))))
    for ax, (h, v, title) in zip(axes, views):
        for _, p, t in theirs:
            if len(t):
                q = sample(p, t, 4000, rng)
                ax.scatter(q[:, h], q[:, v], s=1, c="0.6", alpha=0.6)
        for n, p, t in mine:
            if not len(t):
                continue
            base = n
            q = sample(p, t, 1500, rng)
            ax.scatter(q[:, h], q[:, v], s=0.4, color=colours[names.index(base)], alpha=0.5)
        ax.set_aspect("equal")
        ax.set_title(title)
        ax.grid(alpha=0.3)
    for i, n in enumerate(names):
        axes[0].scatter([], [], color=colours[i], label=n)
    axes[0].legend(loc="upper right", markerscale=6)
    out = args.out or os.path.join(args.built, "overlay_%s.png" % args.key)
    fig.tight_layout()
    fig.savefig(out, dpi=70)
    print(out)


if __name__ == "__main__":
    main()
