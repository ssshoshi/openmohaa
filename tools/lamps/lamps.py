#!/usr/bin/env python3
"""Where the bulbs of the game's lamps are (data/opm-lamps/lights/lamps.txt).

    tools/lamps/lamps.py suggest                    # every lamp model, its bulbs found from its mesh
    tools/lamps/lamps.py suggest models/lights/hang5lamp.tik
    tools/lamps/lamps.py sheet out.png              # each model in lamps.txt, three views, its bulbs marked
    tools/lamps/lamps.py check                      # lamps.txt's models exist, and their bulbs are inside them

A lamp model is one surface with one texture: nothing in it says where its
bulb is. The renderer puts each lamp's light at the bulbs lamps.txt gives
(code/renderergl2/tr_rtlight.c), so that is where the light comes from.

`suggest` finds them from the mesh: the brightest parts of its texture (the
glass, the shade lit from inside, the bulb), each piece of mesh one bulb, at
the middle of its box. It is right for most of them and a start for the rest
(a lantern's glass is dark, a winter lamp post's snow is bright); `sheet`
draws what lamps.txt says, to check it by eye.

Positions are in the model's frame as the game draws it: posed by its first
animation frame and scaled by its TIKI's scale, before the map's own scale.

MOHAA_GAME (default /mnt/d/Medal of Honor) is the folder with main/.
"""

import argparse
import io
import math
import os
import re
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import paks  # noqa: E402

TABLE = os.path.join(os.path.dirname(__file__), "..", "..", "data", "opm-lamps", "lights", "lamps.txt")

# the models the renderer takes for lamps (R_RtIsFixture), less the glows
LAMP_MODELS = r"^models/(lights|static)/.*(light|lamp|lantern|bulb|chand|sconce|candle).*\.tik$"

_idx = None


def _files():
    """{lowercase path: pak that wins}"""
    global _idx
    if _idx is None:
        _idx = {}
        for pak, files in paks.index().items():  # in load order: a later pak wins
            for f in files:
                _idx[re.sub("/+", "/", f.lower())] = (pak, f)
    return _idx


def read(path):
    hit = _files().get(re.sub("/+", "/", path.lower()))
    return paks.read(hit[0], hit[1]) if hit else None


# --------------------------------------------------------------------------- TIKI and shaders

def parse_tiki(path):
    text = re.sub(r"//[^\n]*", "", read(path).decode("latin-1"))
    tiki = {"scale": 1.0, "skel": [], "surfaces": {}, "anim": None}
    folder = ""
    for line in text.splitlines():
        w = line.split()
        if not w:
            continue
        key = w[0].lower()
        if key == "scale" and len(w) > 1:
            tiki["scale"] = float(w[1])
        elif key == "path" and len(w) > 1:
            folder = w[1]
        elif key == "skelmodel" and len(w) > 1:
            tiki["skel"].append(folder + "/" + w[1])
        elif key == "surface" and len(w) >= 4 and w[2].lower() == "shader":
            tiki["surfaces"][w[1].lower()] = w[3]
        elif tiki["anim"] is None and len(w) >= 2 and w[1].lower().endswith(".skc"):
            tiki["anim"] = folder + "/" + w[1]
    return tiki


_shaders = None


def shader_image(name):
    """The first image a shader maps, or the name itself (a shader that is only an image)."""
    global _shaders
    if _shaders is None:
        _shaders = {}
        for path in sorted(p for p in _files() if p.startswith("scripts/") and p.endswith(".shader")):
            text = re.sub(r"//[^\n]*", "", read(path).decode("latin-1"))
            depth, current, body = 0, None, []
            for tok in re.findall(r"\{|\}|[^\s{}]+", text):
                if tok == "{":
                    depth += 1
                    if depth == 1:
                        body = []
                elif tok == "}":
                    depth -= 1
                    if depth == 0 and current:
                        _shaders[current.lower()] = body
                        current = None
                elif depth == 0:
                    current = tok
                else:
                    body.append(tok)
    body = _shaders.get(name.lower())
    if body is None:
        return name
    for i, tok in enumerate(body[:-1]):
        if tok.lower() in ("map", "clampmap") and not body[i + 1].startswith("$"):
            return body[i + 1]
        if tok.lower() == "animmap" and i + 2 < len(body):
            return body[i + 2]
    return None


def load_image(path):
    if not path:
        return None
    base = re.sub(r"\.(tga|jpg)$", "", path, flags=re.I)
    for ext in (".tga", ".jpg"):
        data = read(base + ext)
        if data:
            return np.asarray(Image.open(io.BytesIO(data)).convert("RGBA")).astype(np.float32) / 255
    return None


# --------------------------------------------------------------------------- skeletal meshes

def _cstr(raw):
    return raw.split(b"\0", 1)[0].decode("latin-1")


def read_skd(data):
    """[(name, parent, type)], [(surface name, [(weights, uv)], [(a, b, c)])] of an SKMD 5 or 6."""
    ident, version = struct.unpack_from("<4si", data, 0)
    if ident != b"SKMD" or version not in (5, 6):
        raise ValueError("not an skd 5/6")
    num_surfaces, num_bones, ofs_bones, ofs_surfaces = struct.unpack_from("<4i", data, 72)
    bones, pos = [], ofs_bones
    for _ in range(num_bones):
        name, parent = _cstr(data[pos:pos + 32]), _cstr(data[pos + 32:pos + 64])
        btype, ofs_base, _ofs_chan, _ofs_refs, ofs_end = struct.unpack_from("<5i", data, pos + 64)
        offset = struct.unpack_from("<3f", data, pos + ofs_base) if btype == 0 else (0.0, 0.0, 0.0)
        bones.append((name, None if parent.lower() == "worldbone" else parent, btype, offset))
        pos += ofs_end
    surfaces, pos = [], ofs_surfaces
    for _ in range(num_surfaces):
        name = _cstr(data[pos + 4:pos + 68])
        num_tris, num_verts, _static, ofs_tris, ofs_verts, _c, ofs_end = struct.unpack_from("<7i", data, pos + 68)
        flat = struct.unpack_from("<%di" % (num_tris * 3), data, pos + ofs_tris)
        tris = [flat[i:i + 3] for i in range(0, len(flat), 3)]
        verts, p = [], pos + ofs_verts
        for _ in range(num_verts):
            _n0, _n1, _n2, u, v, num_weights, num_morphs = struct.unpack_from("<5f2i", data, p)
            p += 28 + 16 * num_morphs
            weights = []
            for _ in range(num_weights):
                bone, weight, ox, oy, oz = struct.unpack_from("<if3f", data, p)
                weights.append((bone, weight, (ox, oy, oz)))
                p += 20
            verts.append((weights, (u, v)))
        surfaces.append((name, verts, tris))
        pos += ofs_end
    return bones, surfaces


def read_skc_frame0(data):
    """{channel name (lower): value} of an SKAN 13's first frame."""
    ident, version = struct.unpack_from("<4si", data, 0)
    if ident != b"SKAN" or version != 13:
        raise ValueError("not an skc 13")
    num_channels, ofs_names, num_frames = struct.unpack_from("<3i", data, 36)
    names = [_cstr(data[ofs_names + 32 * i:ofs_names + 32 * i + 32]).lower() for i in range(num_channels)]
    first = 48 + 48 * num_frames
    return {n: struct.unpack_from("<4f", data, first + 16 * i) for i, n in enumerate(names)}


def _quat_rows(q):
    """QuatToMat (q_math.c): an engine bone matrix's rows; p' = p * M."""
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                     [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                     [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])


def pose(bones, channels):
    """{bone name (lower): (rows, origin)}: rotation and position bones only (all a lamp has)."""
    out = {}

    def world(i):
        name, parent, btype, offset = bones[i]
        if name.lower() in out:
            return out[name.lower()]
        q = channels.get(name.lower() + " rot", (0.0, 0.0, 0.0, 1.0))
        origin = np.array(offset if btype == 0 else channels.get(name.lower() + " pos", (0, 0, 0, 0))[:3])
        rows = _quat_rows(q)
        if parent:
            j = next((k for k, b in enumerate(bones) if b[0].lower() == parent.lower()), None)
            if j is not None:
                prows, porigin = world(j)
                rows, origin = rows @ prows, origin @ prows + porigin
        out[name.lower()] = (rows, origin)
        return out[name.lower()]

    for i in range(len(bones)):
        world(i)
    return out


def mesh(model):
    """The model's triangles as the game draws them: [(p0, p1, p2, uv0, uv1, uv2, image)]."""
    tiki = parse_tiki(model)
    channels = {}
    if tiki["anim"] and read(tiki["anim"]):
        channels = read_skc_frame0(read(tiki["anim"]))
    tris = []
    for skel in tiki["skel"]:
        bones, surfaces = read_skd(read(skel))
        posed = pose(bones, channels)
        for name, verts, faces in surfaces:
            shader = tiki["surfaces"].get(name.lower()) or tiki["surfaces"].get("all") or name
            image = load_image(shader_image(shader))
            points = []
            for weights, _uv in verts:
                p = np.zeros(3)
                for bone, weight, offset in weights:
                    rows, origin = posed[bones[bone][0].lower()]
                    p += weight * (np.array(offset) @ rows + origin)
                points.append(p * tiki["scale"])
            for a, b, c in faces:
                tris.append((points[a], points[b], points[c], verts[a][1], verts[b][1], verts[c][1], image))
    return tris


# --------------------------------------------------------------------------- finding the bulbs

def tri_colour(t):
    image = t[6]
    if image is None:
        return np.array([1.0, 0.0, 1.0, 1.0])
    h, w = image.shape[:2]
    samples = []
    for weights in ((1 / 3, 1 / 3, 1 / 3), (0.6, 0.2, 0.2), (0.2, 0.6, 0.2), (0.2, 0.2, 0.6)):
        u = sum(k * uv[0] for k, uv in zip(weights, t[3:6]))
        v = sum(k * uv[1] for k, uv in zip(weights, t[3:6]))
        samples.append(image[int((v % 1) * h) % h, int((u % 1) * w) % w])
    return np.mean(samples, axis=0)


def find_bulbs(tris):
    """[(centre, size)]: the bright pieces of the mesh, the faint ones left out."""
    colours = [tri_colour(t) for t in tris]
    luma = np.array([0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2] if c[3] >= 0.5 else 0.0 for c in colours])
    area = np.array([0.5 * np.linalg.norm(np.cross(t[1] - t[0], t[2] - t[0])) for t in tris])
    if not (area > 0).any():
        return []
    threshold = max(0.55, 0.8 * np.percentile(luma[area > 0], 95))
    bright = [i for i in range(len(tris)) if luma[i] >= threshold and area[i] > 0]

    # pieces of mesh: bright triangles that share a corner
    parent = {i: i for i in bright}

    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    corner = {}
    for i in bright:
        for p in tris[i][:3]:
            key = tuple(np.round(p, 1))
            if key in corner:
                parent[root(i)] = root(corner[key])
            else:
                corner[key] = i
    pieces = {}
    for i in bright:
        pieces.setdefault(root(i), []).append(i)
    boxes = []
    for members in pieces.values():
        pts = np.array([p for i in members for p in tris[i][:3]])
        boxes.append([pts.min(0), pts.max(0), sum(area[i] for i in members)])

    # a piece with its middle in another is part of it (the bulb in a shade);
    # shades that only touch, a chandelier's, stay apart
    def inside(p, box):
        return np.all(p >= box[0] - 0.5) and np.all(p <= box[1] + 0.5)

    merged = True
    while merged:
        merged = False
        for i in range(len(boxes)):
            for j in range(i + 1, len(boxes)):
                a, b = boxes[i], boxes[j]
                if inside((a[0] + a[1]) / 2, b) or inside((b[0] + b[1]) / 2, a):
                    boxes[i] = [np.minimum(a[0], b[0]), np.maximum(a[1], b[1]), a[2] + b[2]]
                    del boxes[j]
                    merged = True
                    break
            if merged:
                break
    total = sum(b[2] for b in boxes) or 1.0
    return [((b[0] + b[1]) / 2, b[1] - b[0]) for b in boxes if b[2] > 0.04 * total]


# --------------------------------------------------------------------------- lamps.txt

def read_table(path=TABLE):
    """{model: {key: [values...] or [[values], ...] for bulb}}"""
    text = re.sub(r"//[^\n]*", "", open(path).read())
    table, current = {}, None
    for line in text.splitlines():
        w = line.split()
        if not w:
            continue
        if w[0] == "{" or w[0] == "}":
            continue
        if current is None or (len(w) == 1 and w[0].lower().endswith(".tik")):
            current = table.setdefault(w[0].lower(), {"bulb": []})
            continue
        if w[0] == "bulb":
            current["bulb"].append([float(x) for x in w[1:4]])
        else:
            current[w[0]] = w[1:]
    return table


VIEWS = [("front", 0, 2, 1), ("side", 1, 2, 0), ("top", 0, 1, 2)]


def draw(model, tris, marks, size=220):
    """Three views of a model, flat shaded by its texture, with marks [(point, colour)]."""
    pts = np.array([p for t in tris for p in t[:3]])
    lo, hi = pts.min(0), pts.max(0)
    span, centre = max(hi - lo) * 1.1 + 1e-3, (lo + hi) / 2
    img = Image.new("RGB", (size * 3, size + 20), (40, 40, 60))
    dr = ImageDraw.Draw(img)
    for vi, (name, a, b, depth) in enumerate(VIEWS):
        def project(p):
            return (vi * size + size / 2 + (p[a] - centre[a]) / span * size,
                    20 + size / 2 - (p[b] - centre[b]) / span * size)
        far_first = sorted(tris, key=lambda t: (t[0][depth] + t[1][depth] + t[2][depth]) * (-1 if name == "front" else 1))
        for t in far_first:
            c = tri_colour(t)
            if c[3] >= 0.5:
                dr.polygon([project(p) for p in t[:3]], fill=tuple(int(x * 255) for x in c[:3]))
        x, y = project(np.zeros(3))
        dr.line([x - 4, y, x + 4, y], fill=(255, 0, 0))
        dr.line([x, y - 4, x, y + 4], fill=(255, 0, 0))
        for p, colour in marks:
            x, y = project(p)
            dr.ellipse([x - 4, y - 4, x + 4, y + 4], outline=colour, width=2)
        dr.text((vi * size + 4, 4), name, fill=(255, 255, 255))
    dr.text((size // 2, 4), model.split("/")[-1], fill=(255, 255, 0))
    return img


def lamp_models():
    return sorted(p for p in _files() if re.match(LAMP_MODELS, p) and "corona" not in p)


def cmd_suggest(args):
    for model in args.models or lamp_models():
        try:
            bulbs = find_bulbs(mesh(model))
        except (ValueError, TypeError, KeyError) as e:
            print("// %s: %s" % (model, e))
            continue
        print(model)
        print("{")
        for centre, size in bulbs:
            print("\tbulb %.1f %.1f %.1f\t// %.0f x %.0f x %.0f" % (*centre, *size))
        print("}")


def cmd_sheet(args):
    table = read_table()
    tiles = []
    for model, entry in table.items():
        tris = mesh(model)
        marks = [(np.array(b), (0, 255, 0)) for b in entry["bulb"]]
        if args.auto:
            marks += [(c, (160, 160, 160)) for c, _s in find_bulbs(tris)]
        tiles.append(draw(model, tris, marks))
    w, h = tiles[0].size
    sheet = Image.new("RGB", (w * 2, (len(tiles) + 1) // 2 * h))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % 2) * w, (i // 2) * h))
    sheet.save(args.out)
    print("%d models -> %s" % (len(tiles), args.out))


def cmd_check(args):
    bad = 0
    for model, entry in read_table().items():
        if not read(model):
            print("%s: no such model" % model)
            bad += 1
            continue
        pts = np.array([p for t in mesh(model) for p in t[:3]])
        lo, hi = pts.min(0) - 2, pts.max(0) + 2
        for b in entry["bulb"]:
            if not (np.all(np.array(b) >= lo) and np.all(np.array(b) <= hi)):
                print("%s: bulb %s is outside the model" % (model, b))
                bad += 1
        if not entry["bulb"]:
            print("%s: no bulbs" % model)
            bad += 1
    print("ok" if not bad else "%d problems" % bad)
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("suggest")
    p.add_argument("models", nargs="*")
    p.set_defaults(func=cmd_suggest)
    p = sub.add_parser("sheet")
    p.add_argument("out")
    p.add_argument("--auto", action="store_true", help="mark what suggest finds too, in grey")
    p.set_defaults(func=cmd_sheet)
    p = sub.add_parser("check")
    p.set_defaults(func=cmd_check)
    args = ap.parse_args()
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
