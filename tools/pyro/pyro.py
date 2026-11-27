#!/usr/bin/env python3
"""Simulated explosions (Blender Mantaflow) as flipbook sprites for the game.

    tools/pyro/pyro.py sim truck_hit                  # bake + render (Windows Blender, GPU)
    tools/pyro/pyro.py sim truck_hit --preview        # every 4th frame, low res, to tune
    tools/pyro/pyro.py pack truck_hit --pk3 OUT.pk3   # textures + shader into a test pak
    tools/pyro/pyro.py sheet truck_hit OUT.png        # contact sheet of the packed frames

A preset (presets/NAME.json) is the simulation: domain size in metres, the fuel blobs and
their burst, Mantaflow's fire settings and the render (px, samples). pyro_blender.py runs
it inside Blender and renders three sequences (see there): smoke lit from six sides, two
renders of three channels, and the fire's own light.

The work folder holds the cache and renders: $PYRO_WORK, default the Windows temp folder
(Blender is a Windows program under WSL, and the cache is large).

pack writes, per preset, textures/opm/pyro/NAME/fNN.tga (64 frames) and a shader
opm_pyro_NAME, which is a sprite (opm_pyro_NAME.spr) that plays once, from where it was
spawned. The smoke is lit here, from above and in front, until the renderer lights it
itself from the six-way frames.
"""

import argparse
import glob
import io
import json
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
FRAMES = 64


def blender():
    if os.environ.get("BLENDER"):
        return os.environ["BLENDER"]
    found = sorted(glob.glob("/mnt/c/Program Files/Blender Foundation/Blender */blender.exe"))
    if not found:
        sys.exit("no Blender: set $BLENDER")
    return found[-1]


def winpath(p):
    return subprocess.check_output(["wslpath", "-w", os.path.abspath(p)], text=True).strip()


def work_dir():
    if os.environ.get("PYRO_WORK"):
        return os.environ["PYRO_WORK"]
    tmp = subprocess.check_output(["cmd.exe", "/c", "echo %TEMP%"], text=True,
                                  stderr=subprocess.DEVNULL).strip()
    return os.path.join(subprocess.check_output(["wslpath", tmp], text=True).strip(), "opm-pyro")


def load_preset(name):
    with open(os.path.join(HERE, "presets", name + ".json")) as fh:
        return json.load(fh)


def cmd_sim(args):
    p = load_preset(args.name)
    if args.preview:
        p.update(res=min(p["res"], 96), px=160, samples=16, frame_step=4)
    for kv in args.set or []:
        k, v = kv.split("=", 1)
        p[k] = json.loads(v)
    work = work_dir()
    out = os.path.join(work, args.name + ("_preview" if args.preview else ""))
    if args.grids_only:
        pass
    elif args.render_only:
        for mode in ("pos", "neg", "fire"):
            shutil.rmtree(os.path.join(out, mode), ignore_errors=True)
    else:
        if os.path.isdir(out):
            shutil.rmtree(out)
        os.makedirs(out)
    preset = os.path.join(out, "preset.json")
    with open(preset, "w") as fh:
        json.dump(p, fh, indent=1)
    script = os.path.join(work, "pyro_blender.py")
    shutil.copy(os.path.join(HERE, "pyro_blender.py"), script)
    cmd = [blender(), "-b", "--factory-startup", "--python", winpath(script), "--",
           winpath(preset), winpath(out)] + (["--stage", "render"] if args.render_only else ["--stage", "grids"] if args.grids_only else [])
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            errors="replace", cwd=work)
    for line in proc.stdout:
        if line.startswith("PYRO") or "Error" in line:
            print(line.rstrip(), flush=True)
    sys.exit(proc.wait())


# ---- pack ----------------------------------------------------------------------------

def frames_of(src, mode):
    return sorted(glob.glob(os.path.join(src, mode, "f*.png")))


def compose(pos, neg, fire, p):
    """One game frame: smoke lit by a sun above, in front and a little to the right, with
    the six-way average as sky light, its colour the preset's smoke tint; plus the fire."""
    import numpy as np
    a = np.asarray(pos, dtype=np.float32) / 255.0
    b = np.asarray(neg, dtype=np.float32) / 255.0
    f = np.asarray(fire, dtype=np.float32) / 255.0
    lin = lambda c: np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    srgb = lambda c: np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(np.maximum(c, 0), 1 / 2.4) - 0.055)
    right, top, back = lin(a[..., 0]), lin(a[..., 1]), lin(a[..., 2])
    left, bottom, front = lin(b[..., 0]), lin(b[..., 1]), lin(b[..., 2])
    w = p.get("pack_light", {"top": 0.55, "front": 0.35, "right": 0.25, "ambient": 0.6})
    amb = (right + left + top + bottom + front + back) / 6.0
    light = w["top"] * top + w["front"] * front + w["right"] * right + w["ambient"] * amb
    tint = np.array(p.get("smoke_tint", [0.42, 0.40, 0.38]), dtype=np.float32)
    rgb = light[..., None] * tint * 2.0 + lin(f[..., :3])
    alpha = np.maximum(a[..., 3], f[..., 3]) ** p.get("alpha_gamma", 1.0)
    # undo the premultiplication by the cover: the game blends with the alpha itself
    rgb = rgb / np.maximum(alpha[..., None], 1e-3)
    out = np.concatenate([srgb(np.clip(rgb, 0, 1)), alpha[..., None]], axis=-1)
    return (out * 255 + 0.5).astype(np.uint8)


VOL_MAGIC = b"OPMV"
VOL_VERSION = 1


def make_vol(src, p):
    """The game's volume (renderergl2 tr_pyro.c) from grids.bin:

        char  magic[4] "OPMV"; int version, nx, ny, nz, frames
        float size[3]    the box in game units (at entity scale 1)
        float offset[3]  its lowest corner from the entity's origin
        float extinction per unit where a cell holds 255
        float flame      the fire's brightness
        float times[frames]  seconds from the start
        then each frame, nz * ny * nx cells (x fastest) of two bytes:
        sqrt(density / max) and flame
    """
    import struct
    import numpy as np
    with open(os.path.join(src, "grids.json")) as fh:
        meta = json.load(fh)
    n = meta["size"]
    times = meta["frames"]
    raw = np.fromfile(os.path.join(src, "grids.bin"), dtype=np.float16).astype(np.float32)
    raw = raw.reshape(len(times), 2, n, n, n)
    dens, flame = raw[:, 0], raw[:, 1]
    dmax = float(np.percentile(dens[dens > 1e-3], 99.9))
    upm = p.get("units_per_metre", 52)
    size = [p["domain"][0] * upm, p["domain"][0] * upm, p["domain"][1] * upm]
    # Blender's density_scale is per metre at density 1
    ext = p.get("density_scale", 6.0) / upm * dmax * p.get("vol_density", 1.0)
    d8 = (np.sqrt(np.clip(dens / dmax, 0, 1)) * 255 + 0.5).astype(np.uint8)
    f8 = (np.clip(flame, 0, 1) * 255 + 0.5).astype(np.uint8)
    cells = np.stack([d8, f8], axis=-1)  # frames, z, y, x, 2
    # where the cloud is at the end, for the smoke that takes over from it (effect_tiki)
    last = dens[-1] > dmax * 0.08
    zs, xs = np.nonzero(last.any(axis=1))
    if len(zs):
        m = lambda v: v / n * p["domain"][0]
        p["cloud"] = [m(zs.min()), m(zs.max() + 1), m(xs.max() + 1 - xs.min()) * 0.5, 0.0]
        print("cloud at the end: %.1f-%.1f m up, %.1f m across" % (p["cloud"][0], p["cloud"][1], 2 * p["cloud"][2]))
    head = VOL_MAGIC + struct.pack("<5i", VOL_VERSION, n, n, n, len(times))
    head += struct.pack("<3f", *size) + struct.pack("<3f", -size[0] / 2, -size[1] / 2, 0.0)
    head += struct.pack("<2f", ext, p.get("vol_fire", 3.0))
    head += struct.pack("<%df" % len(times), *times)
    print("volume: %d frames of %d^3, %.1f s, %.0f units, extinction %.3f/unit, %.1f MB" %
          (len(times), n, times[-1], size[0], ext, cells.nbytes / 1e6))
    return head + cells.tobytes(), times[-1]


def cmd_pack(args):
    from PIL import Image
    p = load_preset(args.name)
    src = os.path.join(work_dir(), args.name + ("_preview" if args.preview else ""))
    if not args.flipbook:
        return pack_volume(args, p, src)
    pos, neg, fire = (frames_of(src, m) for m in ("pos", "neg", "fire"))
    if not (len(pos) == len(neg) == len(fire)) or not pos:
        sys.exit("renders incomplete in %s" % src)
    # the first frames show the fuel's source shapes before the burst tears them up
    skip = 0 if args.preview else p.get("start_frame", 1) - 1
    pos, neg, fire = pos[skip:], neg[skip:], fire[skip:]
    n = len(pos)
    pick = [round(i * (n - 1) / (FRAMES - 1)) for i in range(FRAMES)] if n >= FRAMES else list(range(n))
    files = {}
    names = []
    fade = p.get("fade_frames", 16)
    for i, k in enumerate(pick):
        im = compose(Image.open(pos[k]).convert("RGBA"), Image.open(neg[k]).convert("RGBA"),
                     Image.open(fire[k]).convert("RGBA"), p)
        # the last frames fade out while the game's own smoke fades in where the cloud is
        left = len(pick) - 1 - i
        if left < fade:
            im[..., 3] = (im[..., 3] * (left / fade)).astype(im.dtype)
        path = "textures/opm/pyro/%s/f%02d.tga" % (args.name, i)
        buf = io.BytesIO()
        Image.fromarray(im, "RGBA").save(buf, "TGA")
        files[path] = buf.getvalue()
        names.append(path)
    secs = n * (4 if args.preview else 1) / p.get("fps", 24)
    import numpy as np
    k = pick[max(0, len(pick) - 1 - fade)]
    cover = np.asarray(Image.open(pos[k]).convert("RGBA"))[..., 3] > 96
    ys, xs = np.nonzero(cover)
    side = cover.shape[0]
    if len(xs):
        # metres: across from the middle, and up from the ground (the frame's bottom)
        m = lambda v: v / side * p["domain"][0]
        p["cloud"] = [m(side - ys.max()), m(side - ys.min()), m(xs.max() - xs.min()) * 0.5,
                      m((xs.max() + xs.min()) * 0.5) - p["domain"][0] * 0.5]
        print("cloud as it fades: %.1f-%.1f m up, %.1f m across" % (p["cloud"][0], p["cloud"][1], 2 * p["cloud"][2]))
    rate = len(names) / secs
    size_units = p["domain"][0] * p.get("units_per_metre", 52)
    sprite_scale = size_units / Image.open(pos[0]).size[0]
    shader = ("// OPM: generated by tools/pyro/pyro.py from presets/%s.json; edit that.\n"
              "opm_pyro_%s\n{\n\tcull none\n\tspriteGen parallel\n\tspriteScale %.4f\n"
              "\t{\n\t\tanimMapOnce %.3f %s\n\t\tblendFunc blend\n\t\trgbGen vertex\n"
              "\t\talphaGen vertex\n\t}\n}\n") % (args.name, args.name, sprite_scale, rate,
                                                  " ".join(names))
    files["scripts/opm_pyro_%s.shader" % args.name] = shader.encode()
    if p.get("replaces"):
        files[p["replaces"]["model"]] = effect_tiki(args.name, p, secs).encode()
    print("%d frames at %.1f/s, %.1f s, sprite %.0f units (scale %.3f)" %
          (len(names), rate, secs, size_units, sprite_scale))
    if args.pk3:
        with zipfile.ZipFile(args.pk3, "w", zipfile.ZIP_DEFLATED) as z:
            for path, data in sorted(files.items()):
                z.writestr(path, data)
            for extra in args.add or []:
                root, rel = extra.split("=", 1) if "=" in extra else (extra, None)
                for f in glob.glob(os.path.join(root, "**", "*"), recursive=True):
                    if os.path.isfile(f):
                        z.write(f, os.path.relpath(f, root))
        print("wrote", args.pk3)
    if args.sheet:
        sheet([Image.open(io.BytesIO(files[n])) for n in names], args.sheet)


def pack_volume(args, p, src):
    vol, secs = make_vol(src, p)
    path = "models/fx/opm_pyro_%s.vol" % args.name
    files = {path: vol}
    if p.get("replaces"):
        files[p["replaces"]["model"]] = effect_tiki(args.name, p, secs, volume=path).encode()
    if args.pk3:
        with zipfile.ZipFile(args.pk3, "w", zipfile.ZIP_DEFLATED) as z:
            for name, data in sorted(files.items()):
                z.writestr(name, data)
        print("wrote", args.pk3, "%.1f MB" % (os.path.getsize(args.pk3) / 1e6))


def effect_tiki(name, p, secs, volume=None):
    """The game effect model the preset replaces: our flipbook, the light and the smoke
    that stays, under the stock model's name and with its server side (an Animate whose
    start animation the map plays)."""
    r = p["replaces"]
    upm = p.get("units_per_metre", 52)
    mid = p["domain"][1] * 0.5 * upm * r.get("scale", 1.0)  # the frame's middle above the ground
    sc = r.get("scale", 1.0)
    upm *= sc
    # where the flipbook's cloud is when it fades (measured by cmd_pack), in units
    lo, hi, half, dx = p.get("cloud", [3.0, 7.0, 2.0, 0.0])
    # the head of the cloud, not its stem and skirt
    lo = lo + (hi - lo) * 0.4
    cz, ch = (lo + hi) * 0.5 * upm, (hi - lo) * 0.5 * upm
    cw = half * upm * 0.8
    # the volume fades over its last quarter (tr_pyro.c), the flipbook over fade_frames
    hand = secs * 0.75 if volume else secs - p.get("fade_frames", 16) / FRAMES * secs
    t = lambda x: ("%.2f" % x).rstrip("0").rstrip(".")
    return "\n".join([
        "TIKI",
        "// OPM: generated by tools/pyro/pyro.py from presets/%s.json; edit that." % name,
        "// %s" % p.get("comment", ""),
        "setup", "{", "\tscale 1.0", "\tpath models/fx/dummy", "\tskelmodel dummy3.skd", "}", "",
        "init", "{", "\tserver", "\t{", "\t\tclassname Animate", "\t\tnotsolid",
        "\t\trendereffects +dontdraw", "\t\tangles 270 0 0", "\t}",
        "\tclient", "\t{", "\t\tcache %s" % (volume or "opm_pyro_%s.spr" % name), "\t\tcache vsssource.spr",
        *["\t\tcache %s" % m for m in sorted({m for g in r.get("debris", []) for m in g["models"]})], "",
        "\t\toriginemitter wreck", "\t\t(", "\t\t\tstartoff", "\t\t\tmodel vsssource.spr",
        "\t\t\tsmokevolume", "\t\t\tcolor 0.22 0.21 0.2", "\t\t\talpha 0.5",
        "\t\t\tspawnrate 2.5", "\t\t\tlife 9", "\t\t\tscalemin 1.2", "\t\t\tscalemax 1.8",
        "\t\t\tscalerate 0.35", "\t\t\toffset crandom 24 crandom 24 random 24",
        "\t\t\trandvel crandom 10 crandom 10 range 40 30", "\t\t\tfadein 0.6", "\t\t\tfade",
        "\t\t)", "\t}", "}", "",
        "animations", "{", "\tidle dummy3.skc", "", "\tstart dummy3.skc", "\t{",
        "\t\tserver", "\t\t{", "\t\t\tlast commanddelay 1 anim idle", "\t\t}",
        "\t\tclient", "\t\t{",
        "\t\t\tenter opm_blast %s" % t(r.get("blast", 1.5)),
        "\t\t\tenter blockdlight %s %s" % (t(r.get("light", 600)), t(min(secs * 0.4, 1.5))),
        "\t\t\t(", "\t\t\t\tcolor 1.0 0.75 0.4", "\t\t\t\tcolorvelocity -0.7 -0.8 -0.6",
        "\t\t\t\tscalerate -0.6", "\t\t\t\toffset 0 0 %s" % t(upm * 1.5), "\t\t\t)",
        "\t\t\tenter originspawn", "\t\t\t(", "\t\t\t\tmodel %s" % (volume or "opm_pyro_%s.spr" % name),
        "\t\t\t\tcount 1", "\t\t\t\tscale %s" % t(sc), "\t\t\t\tlife %s" % t(secs + 0.1),
        "\t\t\t\toffset 0 0 %s" % t(0 if volume else mid), "\t\t\t)",
        # the smoke that stays: it fades in as the flipbook's cloud fades out
        "\t\t\tenter commanddelay %s originspawn" % t(hand), "\t\t\t(",
        "\t\t\t\tmodel vsssource.spr", "\t\t\t\tsmokevolume", "\t\t\t\tcount %d" % max(6, int(cw * ch / 1800)),
        "\t\t\t\tcolor 0.2 0.19 0.18", "\t\t\t\talpha 0.5", "\t\t\t\tscalemin %s" % t(cw / 40),
        "\t\t\t\tscalemax %s" % t(cw / 28), "\t\t\t\tscalerate 0.12", "\t\t\t\tlife 16",
        "\t\t\t\toffset crandom %s crandom %s range %s %s" % (t(cw), t(cw), t(cz - ch), t(2 * ch)),
        "\t\t\t\trandvel crandom 8 crandom 8 range 10 14", "\t\t\t\tfadein %s" % t(secs - hand),
        "\t\t\t\tfade", "\t\t\t)",
        *debris_lines(r.get("debris", []), t),
        "\t\t\tenter commanddelay %s emitteron wreck" % t(hand * 0.5),
        "\t\t\tenter commanddelay 25 emitteroff wreck",
        "\t\t}", "\t}", "}", ""]).replace("\n", "\r\n")


def debris_lines(groups, t):
    """Pieces thrown out by the blast (the preset's replaces.debris): each group a
    temp model spawn of up to six models picked at random, tumbling, falling and
    bouncing off the world, gone after a while."""
    lines = []
    for g in groups:
        sp, up = g.get("speed", 400), g.get("up", [250, 450])
        lines += [
            "\t\t\tenter originspawn", "\t\t\t(",
            "\t\t\t\tmodel %s" % " ".join(g["models"][:6]),
            "\t\t\t\tcount %d" % g.get("count", 8),
            "\t\t\t\tcollision",
            "\t\t\t\tbouncefactor %s" % t(g.get("bounce", 0.3)),
            "\t\t\t\tscalemin %s" % t(g["scale"][0]),
            "\t\t\t\tscalemax %s" % t(g["scale"][1]),
            "\t\t\t\tlife %s" % t(g.get("life", 5)),
            "\t\t\t\toffset crandom 20 crandom 20 range 12 24",
            "\t\t\t\trandvel crandom %s crandom %s range %s %s" % (t(sp), t(sp), t(up[0]), t(up[1] - up[0])),
            "\t\t\t\taccel 0 0 -800",
            "\t\t\t\tangles random 360 random 360 random 360",
            "\t\t\t\tavelocity crandom %s crandom %s crandom %s" % ((t(g.get("spin", 500)),) * 3),
            "\t\t\t\tfadedelay %s" % t(g.get("life", 5) - 1),
            "\t\t\t\tfade",
            "\t\t\t)",
        ]
    return lines


def sheet(ims, out, cols=16):
    from PIL import Image
    w, h = ims[0].size
    rows = (len(ims) + cols - 1) // cols
    s = Image.new("RGB", (w * cols, h * rows), (70, 80, 95))
    for i, im in enumerate(ims):
        bg = Image.new("RGBA", im.size, (70, 80, 95, 255))
        bg.alpha_composite(im.convert("RGBA"))
        s.paste(bg.convert("RGB"), ((i % cols) * w, (i // cols) * h))
    s.save(out)
    print("sheet", out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sim")
    s.add_argument("name")
    s.add_argument("--preview", action="store_true")
    s.add_argument("--set", action="append", help="KEY=JSON over the preset")
    s.add_argument("--render-only", action="store_true", help="render the baked cache again")
    s.add_argument("--grids-only", action="store_true", help="export the baked cache's grids again")
    s.set_defaults(fn=cmd_sim)
    k = sub.add_parser("pack")
    k.add_argument("name")
    k.add_argument("--preview", action="store_true")
    k.add_argument("--pk3")
    k.add_argument("--add", action="append", help="a folder whose files go into the pk3 too")
    k.add_argument("--sheet")
    k.add_argument("--flipbook", action="store_true", help="the flipbook sprite, not the volume")
    k.set_defaults(fn=cmd_pack)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
