#!/usr/bin/env python3
"""The Opel truck's glass, a surface a pane, so each can be shot out.

models/vehicles/opeltruck/opel.skd has all its glass in one surface,
opelwindows: the windscreen and both door windows. This writes the model with
that surface cut into its panes, and the panes' boxes, for the game
(fgame g_glass.cpp) to tell which a bullet went through:

    opm_opelglass_front   the windscreen (both halves)
    opm_opelglass_left    the left door's window   (+y)
    opm_opelglass_right   the right door's window  (-y)

    tools/vehicles/opel_glass.py --out DIR
        DIR/models/vehicles/opeltruck/opel.skd   (from the player's own paks)
        DIR/opm/glass.txt                         (pane boxes in the model's units, and normals)
        DIR/opm/collide/opel.txt                  (the truck's triangles, for cgame cg_meshclip.c:
                                                   the glass that falls out lands on the truck)

Every other surface is written back as read. A model naming no shader for a
pane draws it with the shader of the pane's name (intact glass,
opm_lamps.shader), so the other trucks on opel.skd keep their glass.
content/opm-lamp-states runs it (opel.generate). MOHAA_GAME is the folder with main/.
"""

import argparse
import glob
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "blender", "io_scene_mohaa"))
from mohaa import pose, skc, skd  # noqa: E402

GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")
SKD = "models/vehicles/opeltruck/opel.skd"
SKC = "models/vehicles/opeltruck/opel.skc"
SOURCE = "opelwindows"
# the models on opel.skd, which the shape is for
TIKIS = (
    "models/vehicles/opeltruck.tik",
    "models/vehicles/opeltruckgray.tik",
    "models/vehicles/opeltruckgreen.tik",
    "models/animate/vehicle_german_opeltruckgreen.tik",
)
# not in the shape: the glass (it is what falls out) and the turning wheels
# (opelhubstill/opeltreadstill are the same wheels standing)
NOT_SOLID = ("opelwindows", "opelhub", "opeltread")
PAD = 4.0  # a pane's box this much thicker each way (model units: cm)


def game_file(path):
    found = None
    for pak in sorted(glob.glob(os.path.join(GAME, "main", "*.pk3"))):
        if os.path.basename(pak).lower().startswith("zzzzzzzzz-opm-lamp-states"):
            continue
        with zipfile.ZipFile(pak) as z:
            for n in z.namelist():
                if n.lower() == path:
                    found = (pak, n)
    if not found:
        raise SystemExit("%s: not in the game's paks" % path)
    with zipfile.ZipFile(found[0]) as z:
        return z.read(found[1])


def components(surface):
    """The surface's vertexes in connected pieces."""
    parent = list(range(len(surface.verts)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for t in surface.triangles:
        for i in t[1:]:
            parent[find(i)] = find(t[0])
    groups = {}
    for i in range(len(surface.verts)):
        groups.setdefault(find(i), []).append(i)
    return list(groups.values())


def pane_normal(pts):
    """The way a pane faces, out of the cab: the plane through its points (the
    least spread of them), turned away from the truck's middle line."""
    import numpy as np

    p = np.asarray(pts, dtype=np.float64)
    mid = p.mean(axis=0)
    _, _, vt = np.linalg.svd(p - mid)
    n = vt[2]
    out = mid.copy()
    out[2] = 0.0  # away from the line down the truck's middle, at its height
    out[0] = mid[0] if abs(mid[1]) < abs(mid[0]) * 0.25 else 0.0
    if np.dot(n, out) < 0:
        n = -n
    return n / np.linalg.norm(n)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    model = skd.read(game_file(SKD))
    anim = skc.read(game_file(SKC))
    world = pose.Skeleton(model.bones).evaluate(pose.frame_channels(anim, 0))
    names = [b.name for b in model.bones]

    src = next((s for s in model.surfaces if s.name.lower() == SOURCE), None)
    if not src:
        raise SystemExit("%s has no %s surface" % (SKD, SOURCE))
    posed = [pose.skin_vertex(v, names, world) for v in src.verts]

    panes = {"opm_opelglass_front": [], "opm_opelglass_left": [], "opm_opelglass_right": []}
    xs = [p[0] for p in posed]
    front_x = max(xs) - 0.25 * (max(xs) - min(xs))
    for group in components(src):
        cx = sum(posed[i][0] for i in group) / len(group)
        cy = sum(posed[i][1] for i in group) / len(group)
        key = "opm_opelglass_front" if cx > front_x else ("opm_opelglass_left" if cy > 0 else "opm_opelglass_right")
        panes[key] += group

    out_surfaces, lines = [], ["// pane surface, its box in the model's units, the way it faces out (tools/vehicles/opel_glass.py)"]
    for s in model.surfaces:
        if s is not src:
            out_surfaces.append(s)
            continue
        for name, idx in panes.items():
            if not idx:
                continue
            keep = sorted(idx)
            remap = {old: new for new, old in enumerate(keep)}
            pane = skd.Surface(name)
            pane.verts = [src.verts[i] for i in keep]
            pane.triangles = [tuple(remap[i] for i in t) for t in src.triangles if t[0] in remap]
            # its own LOD data is gone with the cut: never reduced (skd.write)
            pane.collapse = None
            pane.collapse_index = None
            out_surfaces.append(pane)
            pts = [posed[i] for i in keep]
            lo = [min(p[k] for p in pts) - PAD for k in range(3)]
            hi = [max(p[k] for p in pts) + PAD for k in range(3)]
            normal = pane_normal(pts)
            lines.append("%s %.1f %.1f %.1f %.1f %.1f %.1f %.3f %.3f %.3f" % (name, *lo, *hi, *normal))
    model.surfaces = out_surfaces

    errors = skd.validate(model)
    if errors:
        raise SystemExit("; ".join(errors))
    path = os.path.join(args.out, SKD)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(skd.write(model, model.version if model.version in (5, 6) else 5))
    shape = ["// the shape of opel.skd in its units, for cgame cg_meshclip.c (tools/vehicles/opel_glass.py)"]
    shape += ["tiki %s" % t for t in TIKIS]
    count = 0
    for s in out_surfaces:
        if s.name.lower() in NOT_SOLID or s.name.startswith("opm_opelglass"):
            continue
        sp = [pose.skin_vertex(v, names, world) for v in s.verts]
        for t in s.triangles:
            shape.append("tri " + " ".join("%.1f %.1f %.1f" % tuple(sp[i]) for i in t))
            count += 1
    col = os.path.join(args.out, "opm", "collide", "opel.txt")
    os.makedirs(os.path.dirname(col), exist_ok=True)
    with open(col, "w", newline="\r\n") as f:
        f.write("\n".join(shape) + "\n")
    print("shape: %d triangles" % count)
    txt = os.path.join(args.out, "opm", "glass.txt")
    os.makedirs(os.path.dirname(txt), exist_ok=True)
    with open(txt, "w", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines[1:]))


if __name__ == "__main__":
    main()
