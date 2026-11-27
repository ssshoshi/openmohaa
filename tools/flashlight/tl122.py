#!/usr/bin/env python3
"""The TL-122 flashlight the player wears on the shirt (content/opm-flashlight).

    tools/flashlight/tl122.py --out DIR            the pak's files
    tools/flashlight/tl122.py --out DIR --sheet    and DIR/sheet.png: the icon, large

Reads the player's own model in place (models/Items/TL-122C Flashlight/*.glb in the
game folder, with its textures/ folder; never committed) and writes:

  models/items/opm_tl122.skd/.skc/.tik   the flashlight, one bone; its lens has a
                                          second skin, lit (the renderer picks it while
                                          the light is on: lamps.txt glowsurface), and a
                                          glow disc in front of it (tl122_beam: the
                                          lamps.txt lightsurface, which cgame hides
                                          while the light is off)
  textures/opm/items/tl122*.jpg/png      its colour, normal and specular maps
  scripts/opm_flashlight.shader
  textures/hud/opm_flashlight.tga        the HUD icon: the model rendered, on clear
  sound/opm/flashlight_on.wav, _off.wav  the switch's click (made here)

The model's frame: x out of the lens, y to its left, z up; the origin at the back of
its body, where the clip holds it to the shirt, at the height of the lens.
"""

import argparse
import glob
import math
import os
import struct
import sys
import wave

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "weapons"))
from remodel import read_glb  # noqa: E402
from thompson import skc, skd  # noqa: E402

GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")
SOURCE = os.path.join("models", "Items", "TL-122C Flashlight")
TEX_DIR = "textures/opm/items"
SCALE = 1.0           # the source is in inches, as the game's units about are
BONE = "tl122"
ICON = 128


def write(out, path, data):
    full = os.path.join(out, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as f:
        f.write(data)


def to_game(p):
    """glTF (lens towards -x, y up) to the model's frame (x out of the lens, z up)."""
    return np.stack([-p[:, 0], p[:, 2], p[:, 1]], axis=1)


def load(src):
    found = sorted(glob.glob(os.path.join(src, "*.glb")))
    if not found:
        raise SystemExit("no source model (*.glb) in %s" % src)
    pieces = read_glb(found[0])
    body = [p for p in pieces if p.material == "0"]
    lens = [p for p in pieces if p.material == "1"]
    if not body or not lens:
        raise SystemExit("%s: expected the body (material 0) and the lens (material 1)" % found[0])
    return body[0], lens[0]


def place(body, lens):
    """Both pieces in the model's frame, the origin at the back of the body at the lens's height."""
    bp, lp = to_game(body.pos) * SCALE, to_game(lens.pos) * SCALE
    origin = np.array([bp[:, 0].min(), 0.5 * (bp[:, 1].min() + bp[:, 1].max()), lp[:, 2].mean()])
    return bp - origin, to_game(body.normal), lp - origin, to_game(lens.normal), origin


def surface(name, pos, normal, uv, tris):
    s = skd.Surface(name)
    for p, n, t in zip(pos, normal, uv):
        s.verts.append(skd.Vertex(tuple(map(float, n)), (float(t[0]), float(t[1])), [(0, 1.0, tuple(map(float, p)))]))
    # glTF winds counter-clockwise, skd front faces clockwise
    s.triangles = [(int(a), int(c), int(b)) for a, b, c in tris]
    return s


def split(pos, normal, uv, tris):
    """Pieces under the engine's 1000 vertices and 2000 triangles a surface, in triangle order."""
    out, start = [], 0
    while start < len(tris):
        used, end = set(), start
        while end < len(tris) and end - start < skd.MAX_TRIS:
            new = set(int(i) for i in tris[end]) - used
            if len(used) + len(new) > skd.MAX_VERTS:
                break
            used |= new
            end += 1
        keep = np.array(sorted(used))
        remap = np.full(len(pos), -1)
        remap[keep] = np.arange(len(keep))
        out.append((pos[keep], normal[keep], uv[keep], remap[tris[start:end]]))
        start = end
    return out


def model(body, lens):
    bp, bn, lp, ln, _ = place(body, lens)
    # the lens faces +x: its own normals are whichever way the source had them
    ln = np.tile([1.0, 0.0, 0.0], (len(lp), 1))
    # the glow just in front of the glass
    gp = lp + [0.04, 0.0, 0.0]
    m = skd.Model("opm_tl122.skd")
    m.bones = [skd.Bone(BONE)]
    m.surfaces = [surface("body_%d" % k, *part) for k, part in enumerate(split(bp, bn, body.uv, body.tris))]
    m.surfaces += [surface("tl122_lens", lp, ln, lens.uv, lens.tris),
                  surface("tl122_beam", gp, ln, lens.uv, lens.tris)]
    for s in m.surfaces:
        s.collapse_index = [32767] * len(s.verts)
    errors = skd.validate(m)
    if errors:
        raise SystemExit("; ".join(errors))
    lo, hi = np.minimum(bp.min(0), gp.min(0)), np.maximum(bp.max(0), gp.max(0))
    a = skc.Animation()
    a.channels = [BONE + " pos", BONE + " rot"]
    a.values = [[(0.0, 0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)]]
    a.frames = [skc.Frame((tuple(map(float, lo)), tuple(map(float, hi))),
                          float(max(np.linalg.norm(lo), np.linalg.norm(hi))))]
    return skd.write(m), skc.write(a), lp


TIK = """TIKI
// OPM (content/opm-flashlight, made by tools/flashlight/tl122.py): the TL-122
// flashlight worn on the shirt (cgame's CG_Flashlight). The light is the
// renderer's (lights/lamps.txt, kind headlight): on while tl122_beam is drawn.
setup
{
	scale 1
	path models/items
	skelmodel opm_tl122.skd
	surface body* shader opm_tl122
	surface tl122_lens shader opm_tl122_lens
	surface tl122_lens shader opm_tl122_lens_lit
	surface tl122_beam shader opm_tl122_beam
	lod_scale 100
}

animations
{
	idle opm_tl122.skc
}
"""

SHADERS = """// the TL-122 flashlight (tools/flashlight/tl122.py)
opm_tl122
{
	qer_editorimage %(t)s/tl122.tga
	{
		map %(t)s/tl122.tga
		rgbGen lightingSpherical
	}
}
// its lens, the light off: clear glass over the reflector
opm_tl122_lens
{
	qer_editorimage %(t)s/tl122_lens.tga
	cull none
	{
		map %(t)s/tl122_lens.tga
		blendFunc blend
		rgbGen lightingSpherical
		alphaGen const 0.6
	}
}
// on: the reflector lit (the renderer's second skin, lamps.txt glowsurface)
opm_tl122_lens_lit
{
	qer_editorimage %(t)s/tl122_glow.tga
	cull none
	q3map_opmglow
	{
		map %(t)s/tl122_glow.tga
		blendFunc add
		rgbGen identity
	}
}
// the glow in front of the lens while it is on; under realtime lighting 2 the
// renderer hides it and lights the lens instead
opm_tl122_beam
{
	qer_editorimage %(t)s/tl122_glow.tga
	cull none
	q3map_opmglow
	{
		map %(t)s/tl122_glow.tga
		blendFunc add
		rgbGen identity
	}
}
"""


def sized(img, size, mode):
    img = img.convert(mode)
    return img.resize((size, size), Image.LANCZOS) if img.size != (size, size) else img


def textures(out, body, lens):
    base = body.maps["base"]
    write_img(out, "tl122.jpg", sized(base, 1024, "RGB"))
    write_img(out, "tl122_n.jpg", sized(body.maps["normal"], 1024, "RGB"))
    b = np.asarray(sized(base, 512, "RGB")).astype(np.float32) / 255
    rough = np.asarray(sized(body.maps["rough"], 512, "L")).astype(np.float32) / 255
    metal = np.asarray(sized(body.maps["metal"], 512, "L")).astype(np.float32) / 255
    spec = np.empty((512, 512, 4), np.float32)
    share = 0.8 * metal[..., None]
    spec[..., :3] = 0.04 * (1 - share) + b * share
    spec[..., 3] = np.minimum(1 - rough, 0.6)
    write_img(out, "tl122_s.png", Image.fromarray(np.clip(spec * 255 + 0.5, 0, 255).astype(np.uint8), "RGBA"))
    write_img(out, "tl122_lens.jpg", sized(lens.maps["base"], 256, "RGB"))
    # the lit reflector: a warm hot middle fading to the rim, over the lens's own UVs
    g = np.asarray(sized(lens.maps["base"], 256, "RGB")).astype(np.float32) / 255
    y, x = np.mgrid[0:256, 0:256]
    r = np.hypot(x - 127.5, y - 127.5) / 128
    hot = np.clip(1.15 - r, 0, 1) ** 1.4
    glow = np.clip((0.35 * g + 0.65) * hot[..., None] * [1.0, 0.86, 0.62], 0, 1)
    write_img(out, "tl122_glow.jpg", Image.fromarray((glow * 255 + 0.5).astype(np.uint8)))


def write_img(out, name, img):
    full = os.path.join(out, TEX_DIR, name)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    if name.endswith(".jpg"):
        img.save(full, quality=92, subsampling=0)
    else:
        img.save(full, optimize=True)


# ----------------------------------------------------------------------------- icon

def render(body, lens, size, ss=4):
    """The flashlight drawn in three-quarter view, textured and lit, on a clear background
    (RGBA): the body, then the lens's glass over the reflector."""
    yaw, pitch = math.radians(-35), math.radians(12)
    ry = np.array([[math.cos(yaw), -math.sin(yaw), 0], [math.sin(yaw), math.cos(yaw), 0], [0, 0, 1]])
    rx = np.array([[1, 0, 0], [0, math.cos(pitch), -math.sin(pitch)], [0, math.sin(pitch), math.cos(pitch)]])
    rot = rx @ ry
    big = size * ss
    # the frame from the body: view down -x, screen right = -y, up = z, nearer = larger x
    pb = to_game(body.pos) @ rot.T
    span = max(np.ptp(pb[:, 1]), np.ptp(pb[:, 2])) * 1.08
    cx, cy = 0.5 * (-pb[:, 1].max() - pb[:, 1].min()), 0.5 * (pb[:, 2].max() + pb[:, 2].min())
    light = np.array([0.55, -0.45, 0.7])
    light /= np.linalg.norm(light)
    color = np.zeros((big, big, 3), np.float32)
    zbuf = np.full((big, big), -1e9, np.float32)
    alpha = np.zeros((big, big), np.float32)

    def draw(piece, glass):
        p = to_game(piece.pos) @ rot.T
        n = to_game(piece.normal) @ rot.T
        X = (-p[:, 1] - cx) / span * big + big / 2
        Y = big / 2 - (p[:, 2] - cy) / span * big
        depth = p[:, 0] + (0.02 if glass else 0)
        tex = np.asarray(piece.maps["base"].convert("RGB").resize((1024, 1024), Image.LANCZOS)).astype(np.float32) / 255
        uv = piece.uv
        for a, b, c in piece.tris:
            xs, ys = X[[a, b, c]], Y[[a, b, c]]
            x0, x1 = int(max(0, math.floor(xs.min()))), int(min(big - 1, math.ceil(xs.max())))
            y0, y1 = int(max(0, math.floor(ys.min()))), int(min(big - 1, math.ceil(ys.max())))
            if x1 < x0 or y1 < y0:
                continue
            den = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if abs(den) < 1e-12:
                continue
            gy, gx = np.mgrid[y0:y1 + 1, x0:x1 + 1] + 0.5
            w0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / den
            w1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / den
            w2 = 1 - w0 - w1
            z = w0 * depth[a] + w1 * depth[b] + w2 * depth[c]
            zb = zbuf[y0:y1 + 1, x0:x1 + 1]
            win = (w0 >= 0) & (w1 >= 0) & (w2 >= 0) & (z > zb)
            if not win.any():
                continue
            u = w0 * uv[a, 0] + w1 * uv[b, 0] + w2 * uv[c, 0]
            v = w0 * uv[a, 1] + w1 * uv[b, 1] + w2 * uv[c, 1]
            t = tex[np.clip((v % 1) * 1023, 0, 1023).astype(int), np.clip((u % 1) * 1023, 0, 1023).astype(int)]
            dst = color[y0:y1 + 1, x0:x1 + 1]
            if glass:
                # a pale reflector behind smudged glass, brighter at its middle
                rr = np.hypot(u - 0.5, v - 0.5) * 2
                refl = (0.55 * np.clip(1 - rr, 0, 1) ** 0.7)[..., None] * [0.85, 0.85, 0.78]
                out = dst * 0.35 + refl + t * 0.45
                dst[win] = np.clip(out, 0, 1)[win]
                alpha[y0:y1 + 1, x0:x1 + 1][win] = 1
                continue
            nn = w0[..., None] * n[a] + w1[..., None] * n[b] + w2[..., None] * n[c]
            nn /= np.maximum(np.linalg.norm(nn, axis=-1, keepdims=True), 1e-9)
            nn = np.where(nn[..., :1] < 0, -nn, nn)  # two-sided
            lam = np.clip(nn @ light, 0, 1)
            rim = np.clip(1 - nn[..., 0], 0, 1) ** 3 * 0.25
            shade = 0.38 + 0.85 * lam[..., None] + rim[..., None]
            zb[win] = z[win]
            dst[win] = (t * shade)[win]
            alpha[y0:y1 + 1, x0:x1 + 1][win] = 1

    draw(body, False)
    draw(lens, True)
    rgba = np.concatenate([np.clip(color, 0, 1), alpha[..., None]], axis=-1)
    # premultiplied downsample so the edge does not pick up black
    rgba[..., :3] *= rgba[..., 3:]
    small = np.asarray(Image.fromarray((rgba * 255 + 0.5).astype(np.uint8), "RGBA").resize((size, size), Image.BOX))
    small = small.astype(np.float32) / 255
    rgb = small[..., :3] / np.maximum(small[..., 3:], 1e-6)
    return Image.fromarray((np.concatenate([np.clip(rgb, 0, 1), small[..., 3:]], -1) * 255 + 0.5).astype(np.uint8), "RGBA")


# ----------------------------------------------------------------------------- sound

def click(on, rate=22050):
    """The slide switch: a sharp tick and the knock of the slide stopping (on), or the
    reverse, softer (off)."""
    rng = np.random.default_rng(122 if on else 123)
    n = int(rate * 0.09)
    t = np.arange(n) / rate
    out = np.zeros(n)
    for at, amp, freq, decay in ((0.0, 0.9, 3400, 900), (0.018, 0.55, 1250, 380)) if on else \
            ((0.0, 0.45, 1150, 380), (0.014, 0.7, 3000, 900)):
        tt = t - at
        env = np.where(tt >= 0, np.exp(-np.maximum(tt, 0) * decay), 0)
        out += amp * env * (np.sin(2 * np.pi * freq * tt) * 0.6 + rng.standard_normal(n) * 0.4)
    out *= 0.7 / np.abs(out).max()
    data = (out * 32767).astype("<i2").tobytes()
    import io
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(data)
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--game", default=GAME)
    ap.add_argument("--sheet", action="store_true")
    args = ap.parse_args()

    body, lens = load(os.path.join(args.game, SOURCE))
    skd_data, skc_data, lens_pos = model(body, lens)
    write(args.out, "models/items/opm_tl122.skd", skd_data)
    write(args.out, "models/items/opm_tl122.skc", skc_data)
    write(args.out, "models/items/opm_tl122.tik", TIK.replace("\n", "\r\n").encode("latin-1"))
    write(args.out, "scripts/opm_flashlight.shader",
          (SHADERS % {"t": TEX_DIR}).replace("\n", "\r\n").encode("latin-1"))
    textures(args.out, body, lens)
    icon = render(body, lens, ICON)
    full = os.path.join(args.out, "textures", "hud", "opm_flashlight.tga")
    os.makedirs(os.path.dirname(full), exist_ok=True)
    icon.save(full)
    write(args.out, "sound/opm/flashlight_on.wav", click(True))
    write(args.out, "sound/opm/flashlight_off.wav", click(False))
    c = lens_pos.mean(0)
    print("lens centre (model frame): %.2f %.2f %.2f" % tuple(c))
    if args.sheet:
        render(body, lens, 512).save(os.path.join(args.out, "sheet.png"))


if __name__ == "__main__":
    main()
