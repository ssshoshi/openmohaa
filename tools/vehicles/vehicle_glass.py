#!/usr/bin/env python3
"""The glass of every vehicle in the game, a surface a pane, so it can be shot out.

A vehicle's .skd has all its glass in one surface (the Opel's opelwindows, the
cars' carwindow, the Stuka's stukawindow...). For every model in the player's
paks that has one, this writes:

    DIR/<the .skd>                  the model, its glass cut into panes: each
                                    piece of it (a connected part of the
                                    surface) a surface of its own, opm_glass<n>;
                                    the Opel's grouped as before into
                                    opm_opelglass_front/left/right
    DIR/<each .tik on it>           its pane surfaces' skins: whole (the glass's
                                    own shader), cracked, crazed (opm_glass_cracked,
                                    opm_glass_heavy, opm_vehicles.shader). A
                                    static model's (models/static/, placed in a
                                    map's BSP and drawn by the renderer alone)
                                    draws no glass of its own (opm_glass_none):
                                    fgame g_glass.cpp puts a copy of it there
                                    showing only its glass, which can be hit
    DIR/opm/glass/<model>.txt       the panes: their triangles in the model's
                                    units, the way each faces out, and the
                                    .tik files they are for (fgame g_glass.cpp)
    DIR/opm/collide/<model>.txt     the model's shape, all but its glass, for
                                    the glass that falls to land on
                                    (cgame cg_meshclip.c)

    tools/vehicles/vehicle_glass.py --out DIR

content/opm-lamp-states runs it (vehicle_glass.generate). models/vehicles/
opeltruck.tik is that pak's patch (its slit glow), which names the Opel's
panes itself. Every other surface is written back as read. MOHAA_GAME is the
folder with main/.
"""

import argparse
import glob
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "blender", "io_scene_mohaa"))
from mohaa import pose, skc, skd  # noqa: E402

GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")
# a vehicle's glass surface, by name
GLASS = {"opelwindows", "carwindow", "dtruckwindow", "fockwindow", "window", "gmcwindow",
         "windows", "p47window", "stukawindow"}
# only models under these: "window" is a common name
ROOTS = ("models/vehicles/", "models/static/vehicle", "models/static/opeltruck",
         "models/animate/vehicle")
# glass drawn broken already (a burnt car's, a wrecked truck's, a downed
# plane's): its shader's name says so. Those .tik files get the panes too (the
# model is the same) drawn with their own shader, and nothing breaks them
BROKEN = re.compile(r"(_d\d?|_d_d|windowb|windowr)$", re.I)
# .tik files the pak patches itself (they name their panes there)
PATCHED = {"models/vehicles/opeltruck.tik"}
# the Opel's panes as the patch names them, and what draws each whole
OPEL_DIR = "models/vehicles/opeltruck/"
# the turning wheels are not the truck's shape (their standing copies are),
# nor are fake beams of light (the Opel's opellights reaches far out ahead)
NOT_SOLID = {"opelhub", "opeltread"}


def solid(surface, cutouts=()):
    """In the shape glass lands on: not a fake beam, not a turning wheel, and
    not drawn cut out (an alpha tested shader, *_cull: the Opel's opel2 is
    mostly holes, and shards lay on them in the air round the cab)."""
    name = surface.name.lower()
    return name not in NOT_SOLID and "light" not in name and name not in cutouts


def cutout_surfaces(tiks):
    """The surfaces some .tik on the model draws with a cut-out shader."""
    out = set()
    for _, text, _, _, _ in tiks:
        for m in re.finditer(r"^\s*surface\s+(\S+)\s+shader\s+(\S+)", text, re.M | re.I):
            if "cull" in m.group(2).lower():
                out.add(m.group(1).lower())
    return out


class Paks:
    """The player's paks, the last of them that has a file the one read."""

    def __init__(self):
        self.where = {}
        for pak in sorted(glob.glob(os.path.join(GAME, "main", "*.pk3"))):
            if os.path.basename(pak).lower().startswith("zzzzzzzzz-opm"):
                continue
            try:
                z = zipfile.ZipFile(pak)
            except zipfile.BadZipFile:
                continue
            for n in z.namelist():
                self.where[n.lower()] = (z, n)

    def has(self, path):
        return path.lower() in self.where

    def read(self, path):
        z, n = self.where[path.lower()]
        return z.read(n)

    def names(self):
        return self.where.keys()


def parse_tik(text):
    """Its path, skelmodels, the surfaces' shaders, and the first animation's file."""
    path = re.search(r"^\s*path\s+(\S+)", text, re.M | re.I)
    path = path.group(1).replace("\\", "/").rstrip("/") + "/" if path else ""
    skels = re.findall(r"^\s*skelmodel\s+(\S+)", text, re.M | re.I)
    anim = None
    a = re.search(r"^\s*animations\s*\{(.*)", text, re.M | re.I | re.S)
    if a:
        m = re.search(r"^\s*\w[\w-]*\s+(\S+\.skc)", a.group(1), re.M | re.I)
        if m:
            anim = m.group(1)
    return path, skels, anim


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
    return sorted(groups.values(), key=lambda g: min(g))


def pane_normal(pts, centre):
    """The way a pane faces, out of the vehicle: the plane through its points
    (their least spread), turned away from the model's middle."""
    import numpy as np

    p = np.asarray(pts, dtype=np.float64)
    mid = p.mean(axis=0)
    _, _, vt = np.linalg.svd(p - mid)
    n = vt[2]
    if np.dot(n, mid - np.asarray(centre)) < 0:
        n = -n
    return n / (np.linalg.norm(n) or 1)


def opel_pane(cx, cy, front_x):
    return "opm_opelglass_front" if cx > front_x else ("opm_opelglass_left" if cy > 0 else "opm_opelglass_right")


def split_glass(model, src, posed, opel):
    """{pane surface name: [vertex indexes]}"""
    groups = components(src)
    if opel:
        xs = [p[0] for p in posed]
        front_x = max(xs) - 0.25 * (max(xs) - min(xs))
        panes = {}
        for g in groups:
            cx = sum(posed[i][0] for i in g) / len(g)
            cy = sum(posed[i][1] for i in g) / len(g)
            panes.setdefault(opel_pane(cx, cy, front_x), []).extend(g)
        return panes
    # a pane modelled twice (its inside face a piece of its own) is one pane:
    # pieces in one plane, over one another, go together
    import numpy as np

    def plane(g):
        p = np.asarray([posed[i] for i in g], dtype=np.float64)
        mid = p.mean(axis=0)
        _, _, vt = np.linalg.svd(p - mid)
        return mid, vt[2], p.min(axis=0), p.max(axis=0)

    merged = []
    for g in groups:
        mid, n, lo, hi = plane(g)
        for m in merged:
            if abs(float(np.dot(n, m["n"]))) > 0.95 and abs(float(np.dot(mid - m["mid"], m["n"]))) < 3.0 \
                    and np.all(lo <= m["hi"] + 2.0) and np.all(hi >= m["lo"] - 2.0):
                m["idx"] += g
                break
        else:
            merged.append({"idx": list(g), "mid": mid, "n": n, "lo": lo, "hi": hi})
    return {"opm_glass%d" % k: m["idx"] for k, m in enumerate(merged)}


def fmt(v):
    return "%.2f %.2f %.2f" % tuple(v)


def glass_shader(text, glass):
    m = re.search(r"^\s*surface\s+%s\s+shader\s+(\S+)" % re.escape(glass), text, re.M | re.I)
    return m.group(1) if m else glass


def rewrite_tik(text, glass, panes, static, opel, broken=False):
    """The .tik with its glass surface's lines made its panes'."""
    out, named = [], False
    for line in text.split("\n"):
        m = re.match(r"^(\s*)surface\s+(\S+)\s+(.*?)\s*$", line, re.I)
        if not m or m.group(2).lower() != glass.lower():
            out.append(line)
            continue
        indent, rest = m.group(1), m.group(3)
        sh = re.match(r"shader\s+(\S+)", rest, re.I)
        if sh:
            if named:
                continue  # a second skin of the glass: the panes have theirs
            named = True
            out += skin_lines(indent, sh.group(1), panes, static, opel, broken)
        else:
            # a command on the glass (+nodraw in an animation): on every pane
            out += ["%ssurface %s %s" % (indent, p, rest) for p in panes]
    if not named:
        # no shader named: the glass was drawn with the shader of its name
        for i, line in enumerate(out):
            if re.match(r"^\s*skelmodel\s+", line, re.I):
                indent = re.match(r"^(\s*)", line).group(1)
                out[i + 1:i + 1] = skin_lines(indent, glass, panes, static, opel, broken)
                break
    return "\n".join(out)


def skin_lines(indent, whole, panes, static, opel, broken=False):
    if broken:
        return ["%s// its glass a surface a pane (tools/vehicles/vehicle_glass.py), broken already: as it was" % indent] \
            + ["%ssurface %s shader %s" % (indent, p, whole) for p in panes]
    lines = ["%s// its glass a surface a pane (tools/vehicles/vehicle_glass.py): %s, cracked, crazed;"
             % (indent, "hidden on the map's own, its copy's skin1 whole" if static else "whole")]
    lines.append("%s// brought down it is not drawn (fgame g_glass.cpp)" % indent)
    for p in panes:
        own = p if opel and whole.lower() == "opelwindows" else whole
        skins = (["opm_glass_none"] if static else []) + [own, "opm_glass_cracked", "opm_glass_heavy"]
        lines += ["%ssurface %s shader %s" % (indent, p, s) for s in skins]
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    paks = Paks()

    # every vehicle .tik, by the .skd it is on
    users = {}
    for name in sorted(paks.names()):
        if not name.endswith(".tik") or not name.startswith(ROOTS):
            continue
        text = paks.read(name).decode("latin-1")
        path, skels, anim = parse_tik(text)
        for s in skels:
            full = (path + s).lower().replace("//", "/")
            if full.endswith(".skb") and not paks.has(full) and paks.has(full[:-4] + ".skd"):
                full = full[:-4] + ".skd"
            users.setdefault(full, []).append((name, text, path, s, anim))

    done = 0
    for skd_path, tiks in sorted(users.items()):
        if not skd_path.endswith(".skd") or not paks.has(skd_path):
            continue
        model = skd.read(paks.read(skd_path))
        src = next((s for s in model.surfaces if s.name.lower() in GLASS), None)
        if not src:
            continue

        anim = None
        for _, _, path, _, a in tiks:
            if a and paks.has((path + a).lower().replace("//", "/")):
                anim = skc.read(paks.read((path + a).lower().replace("//", "/")))
                break
        world = pose.Skeleton(model.bones).evaluate(pose.frame_channels(anim, 0))
        names = [b.name for b in model.bones]
        posed = [pose.skin_vertex(v, names, world) for v in src.verts]
        opel = skd_path.startswith(OPEL_DIR)
        panes = split_glass(model, src, posed, opel)

        # the model's middle, for which way is out
        everything = []
        solid_tris = []
        cutouts = cutout_surfaces(tiks)
        for s in model.surfaces:
            if s is src or not solid(s, cutouts):
                continue
            sp = [pose.skin_vertex(v, names, world) for v in s.verts]
            everything += sp
            solid_tris += [[sp[i] for i in t] for t in s.triangles]
        lo = [min(p[k] for p in everything) for k in range(3)]
        hi = [max(p[k] for p in everything) for k in range(3)]
        centre = [(lo[k] + hi[k]) / 2 for k in range(3)]

        # the model, its glass in panes
        out_surfaces, glass_lines = [], []
        for s in model.surfaces:
            if s is not src:
                out_surfaces.append(s)
                continue
            for name, idx in panes.items():
                keep = sorted(idx)
                remap = {old: new for new, old in enumerate(keep)}
                pane = skd.Surface(name)
                pane.verts = [src.verts[i] for i in keep]
                pane.triangles = [tuple(remap[i] for i in t) for t in src.triangles if t[0] in remap]
                pane.collapse = None  # its LOD data is gone with the cut: never reduced
                pane.collapse_index = None
                out_surfaces.append(pane)
                n = pane_normal([posed[i] for i in keep], centre)
                glass_lines.append("pane %s %.3f %.3f %.3f" % (name, *n))
                for t in src.triangles:
                    if t[0] in remap:
                        glass_lines.append("tri " + " ".join(fmt(posed[i]) for i in t))
        model.surfaces = out_surfaces
        errors = skd.validate(model)
        if errors:
            print("%s: %s" % (skd_path, "; ".join(errors)), file=sys.stderr)
            continue
        dest = os.path.join(args.out, skd_path)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(skd.write(model, model.version if model.version in (5, 6) else 5))

        # the .tik files on it
        tiki_lines, shape_tikis = [], []
        for tik, text, path, skel, _ in tiks:
            static = tik.startswith("models/static/")
            broken = bool(BROKEN.search(glass_shader(text, src.name)))
            shape_tikis.append("tiki %s" % tik)
            if not broken:
                tiki_lines.append(("static %s" if static else "tiki %s") % tik)
            if tik in PATCHED:
                continue
            new = rewrite_tik(text, src.name, list(panes), static and not broken, opel, broken)
            if skel.lower().endswith(".skb") and skd_path.endswith(".skd"):
                new = re.sub(r"(skelmodel\s+)%s" % re.escape(skel), r"\g<1>%s" % (skel[:-4] + ".skd"), new, flags=re.I)
            dest = os.path.join(args.out, tik)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "w", newline="\r\n", encoding="latin-1") as f:
                f.write(new.replace("\r\n", "\n"))

        base = skd_path[len("models/"):-4].replace("/", "_")
        head = "// %s (tools/vehicles/vehicle_glass.py)" % skd_path
        for sub, body in (("glass", glass_lines), ("collide", ["tri " + " ".join(fmt(p) for p in t) for t in solid_tris])):
            dest = os.path.join(args.out, "opm", sub, base + ".txt")
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if sub == "glass" and not tiki_lines:
                continue  # all of its glass broken already
            lines = [head] + (tiki_lines if sub == "glass" else shape_tikis) + body
            with open(dest, "w", newline="\r\n") as f:
                f.write("\n".join(lines) + "\n")
        done += 1
        print("%s: %d panes, %d .tik (%d broken already), %d triangles of shape"
              % (skd_path, len(panes), len(tiks), len(tiks) - len(tiki_lines), len(solid_tris)))
    print("%d models" % done)


if __name__ == "__main__":
    main()
