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

Run by tools/content/pack.py; writes models/human/animation/opm/*.skc.
"""
import argparse
import os
import sys

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


if __name__ == "__main__":
    main()
