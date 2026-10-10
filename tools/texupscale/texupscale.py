#!/usr/bin/env python3
"""Upscale MOH:AA's textures 4x with Real-ESRGAN into a DDS add-on pk3.

Every world, model and sky texture under 1024 pixels is run through
Real-ESRGAN's 4x network and written as a mipmapped DXT1 (opaque) or DXT5
(alpha) DDS of the same name. With r_ext_compressed_textures 1, the GL2
renderer loads a .dds before the .tga or .jpg of that name (R_LoadImage in
code/renderergl2/tr_image.c), from whichever pk3 holds it, so the add-on
needs no shader changes and the game's files are never touched.

Textures are read the way the engine resolves them, so installed texture
packs (AA HD, HRRTM) are what gets upscaled, and textures they already ship
at 1024 or more are left alone.
"""

import argparse
import fnmatch
import hashlib
import importlib.util
import io
import json
import math
import os
import random
import shutil
import sys
import time
import zipfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dds  # noqa: E402

# Pak resolution and shader parsing are matgen's.
_spec = importlib.util.spec_from_file_location(
    "matgen", os.path.join(HERE, "..", "matgen", "matgen.py"))
matgen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(matgen)

VERSION = 1

ROOTS = ("textures/", "env/", "models/")

# UI, tool and particle textures: drawn at a fixed size on screen, never seen,
# or soft gradients that gain nothing.
SKIP_DIRS = (
    "textures/common/", "textures/effects/", "textures/gametext/",
    "textures/hud/", "textures/light/", "textures/lights/",
    "textures/mohmenu/", "textures/objectives/", "textures/special/",
    "textures/sprites/", "textures/test/", "textures/mohtest/",
    "textures/weather/", "textures/tempsign/",
)

# matgen's outputs, and the material maps texture packs ship
MATERIAL_SUFFIXES = ("_n", "_nh", "_s")

# Sky box faces meet at their edges and model skins are cut into islands;
# neither tiles, so both are padded by mirroring rather than wrapping.
NO_WRAP_DIRS = ("env/", "textures/models/", "models/", "textures/sky/")

PAD = 16  # source pixels of context around each image for the network


def pow2(n):
    """Nearest power of two, as the textures are drawn mipmapped."""
    return 1 << max(0, round(math.log2(max(1, n))))


class Shaders:
    """What the shader scripts say about each image."""

    def __init__(self, vfs):
        self.clamp, self.alpha_test = set(), set()
        for path in sorted(p for p in vfs.files if p.startswith("scripts/")
                           and p.endswith(".shader")):
            text = vfs.read(vfs.files[path]).decode("latin-1")
            for _name, _words, stages in matgen.parse_shaders(text):
                for st in stages:
                    low = [[t.lower() for t in l] for l in st]
                    test = any(l[0] == "alphafunc" for l in low)
                    for l, raw in zip(low, st):
                        if l[0] in ("map", "clampmap", "clampmapx",
                                    "clampmapy", "animmap") and len(l) > 1:
                            names = raw[2:] if l[0] == "animmap" else raw[1:2]
                            for n in names:
                                base, _ = matgen.image_base(n)
                                if l[0].startswith("clampmap"):
                                    self.clamp.add(base)
                                if test:
                                    self.alpha_test.add(base)


class Source:
    def __init__(self, entry, ext, size):
        self.entry, self.ext, self.size = entry, ext, size


def image_size(vfs, entry, ext):
    data = _read_head(vfs, entry)
    if ext == "dds":
        return (int.from_bytes(data[16:20], "little"),
                int.from_bytes(data[12:16], "little"))
    return Image.open(io.BytesIO(data)).size


_open_paks = {}


def _pak(game_dir, pak):
    z = _open_paks.get(pak)
    if z is None:
        # opening a pk3 reads its whole directory, which is slow over /mnt
        z = _open_paks[pak] = zipfile.ZipFile(os.path.join(game_dir, pak))
    return z


def _read_head(vfs, entry):
    with _pak(vfs.game_dir, entry[0]).open(entry[1]) as f:
        return f.read(1 << 16)


def read_file(vfs, entry):
    return _pak(vfs.game_dir, entry[0]).read(entry[1])


def plan(vfs, args, sizes):
    """[(base, Source, target (w, h), alpha_test, wrap)], skip counts."""
    shaders = Shaders(vfs)
    exts = {}
    for path in vfs.files:
        if not path.startswith(ROOTS):
            continue
        base, ext = matgen.image_base(path)
        if ext:
            exts.setdefault(base, set()).add(ext)

    stats = dict.fromkeys(("skipped_dir", "material_map", "small",
                           "already_large", "filtered"), 0)
    jobs = []
    for base in sorted(exts):
        if base.startswith(SKIP_DIRS):
            stats["skipped_dir"] += 1
            continue
        if any(base.endswith(s) and base[:-len(s)] in exts
               for s in MATERIAL_SUFFIXES):
            stats["material_map"] += 1
            continue
        if args.only and not any(fnmatch.fnmatch(base, g) for g in args.only):
            stats["filtered"] += 1
            continue

        # With compressed textures on, a .dds of the name wins over any
        # .jpg/.tga; but a texture pack's bigger .jpg/.tga is the better
        # source, and the DDS written for it then lets it show at all.
        cands = []
        for ext in ("dds", "jpg", "tga", "png"):
            if ext in exts[base]:
                entry = vfs.files[base + "." + ext]
                key = "%s|%s|%d" % (entry[0], entry[1], entry[3])
                if key not in sizes:
                    try:
                        sizes[key] = image_size(vfs, entry, ext)
                    except Exception:
                        sizes[key] = None
                if sizes[key]:
                    cands.append(Source(entry, ext, tuple(sizes[key])))
        if not cands:
            continue
        dds_src = next((c for c in cands if c.ext == "dds"), None)
        other = next((c for c in cands if c.ext != "dds"), None)
        src = dds_src or other
        if dds_src and other and max(other.size) > max(dds_src.size):
            src = other
        hidden = src.ext != "dds" and dds_src is not None

        w, h = src.size
        if max(w, h) < args.min_size:
            stats["small"] += 1
            continue
        if max(w, h) >= args.threshold and not hidden:
            stats["already_large"] += 1
            continue
        factor = 4 if max(w, h) < args.threshold else 1
        tw, th = pow2(w) * factor, pow2(h) * factor
        while max(tw, th) > args.max_size:
            tw, th = max(4, tw // 2), max(4, th // 2)
        wrap = base not in shaders.clamp and not base.startswith(NO_WRAP_DIRS)
        jobs.append((base, src, (tw, th), base in shaders.alpha_test, wrap))
    return jobs, stats


def job_key(src, target, alpha_test, wrap, args, model_id):
    h = hashlib.sha1()
    h.update(json.dumps([VERSION, src.entry[0], src.entry[1], src.entry[3],
                         target, alpha_test, wrap, args.format,
                         model_id]).encode())
    return h.hexdigest()


def load_rgba(data):
    img = Image.open(io.BytesIO(data))
    img.load()
    has_alpha = "A" in img.getbands() or "transparency" in img.info
    rgba = np.asarray(img.convert("RGBA"), dtype=np.float32) / 255.0
    if has_alpha and rgba[..., 3].min() >= 250 / 255.0:
        has_alpha = False
    return rgba, has_alpha


def _resize(a, w, h):
    """Lanczos resize of one float channel."""
    if a.shape[1] == w and a.shape[0] == h:
        return a
    return np.asarray(Image.fromarray(a, "F").resize((w, h), Image.LANCZOS))


def upscale_image(net, rgba, has_alpha, target, wrap, run_net=True):
    """Float RGBA -> float RGBA at target (w, h)."""
    tw, th = target
    if run_net:
        h, w = rgba.shape[:2]
        py, px = min(PAD, h), min(PAD, w)
        mode = "wrap" if wrap else "symmetric"
        padded = np.pad(rgba, ((py, py), (px, px), (0, 0)), mode=mode)
        s = 4
        rgb = net.upscale(padded[..., :3])[py * s:(py + h) * s,
                                           px * s:(px + w) * s]
        if has_alpha:
            a = net.upscale(np.repeat(padded[..., 3:], 3, axis=2))
            a = a[py * s:(py + h) * s, px * s:(px + w) * s].mean(axis=2)
        else:
            a = np.ones(rgb.shape[:2], np.float32)
        rgba = np.concatenate([rgb, a[..., None]], axis=2)
    chans = [np.clip(_resize(np.ascontiguousarray(rgba[..., c]), tw, th),
                     0, 1) for c in range(4)]
    out = np.stack(chans, axis=2)
    if not has_alpha:
        out[..., 3] = 1.0
    return out


def process(net, vfs, job, args):
    base, src, target, alpha_test, wrap = job
    rgba, has_alpha = load_rgba(read_file(vfs, src.entry))
    big = upscale_image(net, rgba, has_alpha, target, wrap,
                        run_net=max(src.size) < args.threshold)
    fmt = args.format if args.format != "dxt" else (
        "dxt5" if has_alpha else "dxt1")
    levels = dds.mip_chain(big, alpha_test=alpha_test and has_alpha)
    return dds.write_dds(levels, fmt), fmt, big


def write_preview(net, vfs, jobs, args):
    """Source (nearest) beside the upscale, a crop of each, on one sheet."""
    rng = random.Random(1)
    pick = rng.sample(jobs, min(args.preview, len(jobs)))
    cell = 256
    sheet = Image.new("RGB", (cell * 2 + 8, (cell + 8) * len(pick)), "black")
    for i, job in enumerate(pick):
        _data, _fmt, big = process(net, vfs, job, args)
        src_rgba, _ = load_rgba(read_file(vfs, job[1].entry))
        tw, th = job[2]
        sw, sh = src_rgba.shape[1], src_rgba.shape[0]
        # the same region of both: a quarter of the texture's width
        cw = max(1, sw // 4)
        cx, cy = (sw - cw) // 2, max(0, (sh - cw) // 2)
        a = Image.fromarray(np.round(src_rgba[cy:cy + cw, cx:cx + cw, :3]
                                     * 255).astype(np.uint8))
        bx, by, bw = cx * tw // sw, cy * th // sh, cw * tw // sw
        b = Image.fromarray(np.round(big[by:by + bw, bx:bx + bw, :3]
                                     * 255).astype(np.uint8))
        sheet.paste(a.resize((cell, cell), Image.NEAREST), (0, i * (cell + 8)))
        sheet.paste(b.resize((cell, cell), Image.LANCZOS),
                    (cell + 8, i * (cell + 8)))
        print("  %s %dx%d -> %dx%d" % (job[0], sw, sh, tw, th))
    sheet.save(args.preview_out)
    print("wrote %s" % args.preview_out)


def selftest():
    """DDS round trips through Pillow's decoder; no GPU or game data."""
    ok = True
    # smooth colour, and leaves of alpha with a soft edge, like foliage
    y, x = np.mgrid[0:64, 0:32].astype(np.float32)
    img = np.stack([x / 31, y / 63, 0.5 + 0.5 * np.sin(x / 3) * np.cos(y / 5),
                    np.clip((np.sin(x / 2.5) * np.sin(y / 4) - 0.2) * 4, 0, 1)],
                   axis=2).astype(np.float32)
    for fmt in dds.FORMATS:
        levels = dds.mip_chain(img, alpha_test=True)
        data = dds.write_dds(levels, fmt)
        back = Image.open(io.BytesIO(data))
        back = np.asarray(back.convert("RGBA"), dtype=np.float32) / 255.0
        err = np.abs(back[..., :3] - levels[0][..., :3] / 255.0).mean()
        sizes = [l.shape[:2] for l in levels]
        good = back.shape == (64, 32, 4) and err < 0.03 and \
            sizes[-1][0] <= 4 and sizes[-1][1] <= 4
        print("%-5s %d levels, mean error %.3f: %s" % (
            fmt, len(levels), err, "ok" if good else "FAIL"))
        ok &= good
    # levels of 64 texels or more; a 4x4 level can only come close
    for test in (False, True):
        cov = [float(np.mean(l[..., 3] >= 128)) for l in dds.mip_chain(
            img, alpha_test=test) if l.shape[0] * l.shape[1] >= 64]
        print("alpha coverage by level, %s: %s" % (
            "kept" if test else "plain box filter",
            " ".join("%.2f" % c for c in cov)))
    good = max(cov) - min(cov) < 0.05
    print("coverage kept: %s" % ("ok" if good else "FAIL"))
    return 0 if ok and good else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=os.environ.get(
        "TEXUPSCALE_GAME", "/mnt/d/Medal of Honor/main"),
        help="game directory holding the pk3s (default: %(default)s)")
    ap.add_argument("--out", default="build/texupscale",
                    help="work directory (default: %(default)s)")
    ap.add_argument("--pk3", default="zzzzzzzzz-opm-upscale.pk3",
                    help="output pk3 name (default: %(default)s)")
    ap.add_argument("--exclude", action="append",
                    default=["zzzzzzzzzz-opm-materials*.pk3"],
                    help="pk3s not to read, glob (repeatable; matgen's "
                         "output is excluded by default)")
    ap.add_argument("--only", action="append",
                    help="only texture bases matching this glob, e.g. "
                         "'textures/models/human/*' (repeatable)")
    ap.add_argument("--threshold", type=int, default=1024,
                    help="textures this size or larger are left as they are "
                         "(default: %(default)s)")
    ap.add_argument("--min-size", type=int, default=64,
                    help="textures smaller than this are left as they are "
                         "(default: %(default)s)")
    ap.add_argument("--max-size", type=int, default=2048,
                    help="largest output side (default: %(default)s)")
    ap.add_argument("--format", choices=("dxt", "bc7"), default="dxt",
                    help="dxt: DXT1 for opaque and DXT5 for alpha textures, "
                         "as the game's own; bc7: better colour at twice "
                         "DXT1's memory (default: %(default)s)")
    ap.add_argument("--model", help="a 4x RRDBNet checkpoint instead of "
                                    "RealESRGAN_x4plus")
    ap.add_argument("--tile", type=int, default=256,
                    help="network tile size in source pixels; lower it if "
                         "the GPU runs out of memory (default: %(default)s)")
    ap.add_argument("--dry-run", action="store_true",
                    help="list what would be upscaled and stop")
    ap.add_argument("--preview", type=int, default=0, metavar="N",
                    help="write a before/after sheet of N random textures "
                         "and stop")
    ap.add_argument("--preview-out", default="texupscale-preview.png")
    ap.add_argument("--limit", type=int, default=0,
                    help="process at most N textures this run")
    ap.add_argument("--install", action="store_true",
                    help="copy the finished pk3s into the game directory")
    ap.add_argument("--selftest", action="store_true",
                    help="check the DDS writer against Pillow's reader")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.pk3.lower().endswith(".pk3"):
        ap.error("--pk3 must end in .pk3")

    def excluded(p):
        return matgen.is_pk3_part(p, args.pk3) or any(
            fnmatch.fnmatch(p.lower(), g.lower()) for g in args.exclude)

    vfs = matgen.Vfs(args.game, exclude=excluded)
    print("%d pk3s, %d files" % (len(vfs.paks), len(vfs.files)))

    os.makedirs(args.out, exist_ok=True)
    sizes_path = os.path.join(args.out, "sizes.json")
    try:
        with open(sizes_path) as f:
            sizes = json.load(f)
    except (OSError, ValueError):
        sizes = {}
    jobs, stats = plan(vfs, args, sizes)
    with open(sizes_path, "w") as f:
        json.dump(sizes, f)

    by_pak = {}
    for job in jobs:
        by_pak[job[1].entry[0]] = by_pak.get(job[1].entry[0], 0) + 1
    print("%d textures to upscale; skipped: %s" % (len(jobs), ", ".join(
        "%s %d" % kv for kv in stats.items() if kv[1])))
    print("from: " + ", ".join("%s %d" % kv for kv in sorted(
        by_pak.items(), key=lambda kv: -kv[1])))

    if args.dry_run:
        for base, src, target, alpha_test, wrap in jobs:
            print("  %-64s %4dx%-4d -> %4dx%-4d %s%s <- %s %s" % (
                base, src.size[0], src.size[1], target[0], target[1],
                "wrap " if wrap else "     ", "atest" if alpha_test else "",
                src.entry[0], src.ext))
        return 0

    import esrgan
    net = esrgan.Upscaler(args.model, tile=args.tile)
    print("Real-ESRGAN x4 on %s" % net.name())
    model_id = os.path.basename(args.model) if args.model else \
        esrgan.WEIGHTS_SHA256[:12]

    if args.preview:
        write_preview(net, vfs, jobs, args)
        return 0

    out_dir = os.path.join(args.out, "out")
    manifest_path = os.path.join(args.out, "manifest.json")
    try:
        with open(manifest_path) as f:
            manifest = json.load(f)
    except (OSError, ValueError):
        manifest = {}

    def save_manifest():
        tmp = manifest_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(manifest, f, indent=0, sort_keys=True)
        os.replace(tmp, manifest_path)

    # outputs no longer wanted
    wanted = {job[0] for job in jobs}
    if not args.only:
        for base in [b for b in manifest if b not in wanted]:
            try:
                os.remove(os.path.join(out_dir, manifest[base]["file"]))
            except OSError:
                pass
            del manifest[base]

    todo = []
    for job in jobs:
        key = job_key(job[1], job[2], job[3], job[4], args, model_id)
        prev = manifest.get(job[0])
        if prev and prev["key"] == key and os.path.exists(
                os.path.join(out_dir, prev["file"])):
            continue
        todo.append((job, key))
    print("%d up to date, %d to upscale" % (len(jobs) - len(todo), len(todo)))
    if args.limit:
        todo = todo[:args.limit]

    errors = []
    start = time.time()
    for i, (job, key) in enumerate(todo, 1):
        base = job[0]
        try:
            data, fmt, _ = process(net, vfs, job, args)
        except Exception as e:  # keep going; report at the end
            errors.append("%s: %s" % (job[1].entry[1], e))
            continue
        name = base + ".dds"
        path = os.path.join(out_dir, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
        manifest[base] = {"key": key, "file": name, "format": fmt,
                          "size": list(job[2]), "source": job[1].entry[0]}
        if i % 25 == 0 or i == len(todo):
            save_manifest()
            left = (time.time() - start) / i * (len(todo) - i)
            print("  %d/%d, %d min left" % (i, len(todo), left / 60),
                  flush=True)
    save_manifest()

    if args.limit and len(todo) == args.limit:
        print("stopped at --limit; run again to continue")
    else:
        pack(args, manifest, out_dir)
    for e in errors:
        print("error: " + e, file=sys.stderr)
    return 1 if errors else 0


def pack(args, manifest, out_dir):
    """Split into pk3s the engine can read (no ZIP64, under 2 GB)."""
    parts, part_bytes = [[]], 0
    for base in sorted(manifest):
        name = manifest[base]["file"]
        size = os.path.getsize(os.path.join(out_dir, name))
        if parts[-1] and part_bytes + size > matgen.PK3_MAX_BYTES:
            parts.append([])
            part_bytes = 0
        parts[-1].append(name)
        part_bytes += size

    names = [matgen.pk3_part_name(args.pk3, i) for i in range(len(parts))]
    for name in os.listdir(args.out):
        if matgen.is_pk3_part(name, args.pk3) and name not in names:
            os.remove(os.path.join(args.out, name))
    for name, files in zip(names, parts):
        pk3_path = os.path.join(args.out, name)
        with zipfile.ZipFile(pk3_path + ".tmp", "w", zipfile.ZIP_STORED,
                             allowZip64=False) as z:
            for n in files:
                z.write(os.path.join(out_dir, n), n)
        os.replace(pk3_path + ".tmp", pk3_path)
        print("wrote %s: %d textures, %.0f MB" % (
            pk3_path, len(files), os.path.getsize(pk3_path) / 1048576.0))

    if args.install:
        for name in os.listdir(args.game):
            if matgen.is_pk3_part(name, args.pk3) and name not in names:
                os.remove(os.path.join(args.game, name))
                print("removed stale %s" % name)
        for name in names:
            dest = os.path.join(args.game, name)
            shutil.copyfile(os.path.join(args.out, name), dest)
            print("installed %s" % dest)


if __name__ == "__main__":
    sys.exit(main())
