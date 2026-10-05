#!/usr/bin/env python3
"""MOHAA <-> Blender conversion from the command line.

    tools/blender/mohaa_blender.py import models/weapons/kar98.tik out/kar98.blend
    tools/blender/mohaa_blender.py import models/human/allied_airborne_soldier.tik out/airborne.glb --anims FILTER --filter 'idle*,run*'
    tools/blender/mohaa_blender.py export out/kar98.blend mod/models/weapons/kar98_custom.tik --pk3 mod/zzz-kar98.pk3
    tools/blender/mohaa_blender.py preview models/weapons/kar98.tik /tmp/kar98.png
    tools/blender/mohaa_blender.py info models/weapons/kar98/kar98.skd
    tools/blender/mohaa_blender.py extract models/weapons/kar98.tik out/extracted

An input given as a game path (models/...) is taken from the game's paks; a path on
disk is used as is. import/export/preview run Blender in the background: $BLENDER, then
blender on PATH, then (under WSL) the newest Blender in C:\\Program Files\\Blender Foundation.
The game folder is --game or $MOHAA_GAME (the folder holding main/; default
/mnt/d/Medal of Honor); --ta / --tt add mainta / maintt.
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "io_scene_mohaa"))  # the format library, without the add-on's bpy parts

from mohaa import shader, skc, skd, tiki, vfs  # noqa: E402

DEFAULT_GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")


def game_folders(args):
    out = [os.path.join(args.game, "main")]
    if args.ta:
        out.append(os.path.join(args.game, "mainta"))
    if args.tt:
        out.append(os.path.join(args.game, "maintt"))
    return [f for f in out if os.path.isdir(f)]


def find_blender():
    if os.environ.get("BLENDER"):
        return os.environ["BLENDER"]
    exe = shutil.which("blender")
    if exe:
        return exe
    found = sorted(glob.glob("/mnt/c/Program Files/Blender Foundation/Blender */blender.exe"),
                   key=lambda p: [int(x) if x.isdigit() else x for x in p.replace(".", " ").split()])
    if found:
        return found[-1]
    raise SystemExit("no Blender found: set $BLENDER")


def is_windows_exe(exe):
    return exe.lower().endswith(".exe") and os.path.exists("/proc/sys/fs/binfmt_misc/WSLInterop")


def to_host(path, windows):
    if not windows:
        return path
    return subprocess.check_output(["wslpath", "-w", os.path.abspath(path)], text=True).strip()


def materialize(path, args, tmp):
    """A game path from the paks written to a temporary game tree (so the add-on can read
    it and its neighbours); a disk path as is."""
    if os.path.exists(path):
        return path
    fs = vfs.GameFS(game_folders(args))
    if not fs.exists(path):
        raise SystemExit("%s is neither a file nor a game path" % path)
    out = os.path.join(tmp, "game", *fs.real_name(path).split("/"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(fs.read(path))
    return out


def run_blender(args, command, inp, out, extra):
    exe = find_blender()
    win = is_windows_exe(exe)
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as tmp:
        if command in ("import", "preview"):
            inp = materialize(inp, args, tmp)
        folders = ";".join(to_host(f, win) for f in game_folders(args))
        cmd = [exe, "-b", "--factory-startup", "--python", to_host(os.path.join(HERE, "blender_run.py"), win), "--",
               command, to_host(inp, win), to_host(out, win), "--folders", folders] + extra
        if args.verbose:
            print(" ".join(cmd))
        proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    result = None
    for line in proc.stdout.splitlines():
        if line.startswith("MOHAA_RESULT "):
            result = json.loads(line[len("MOHAA_RESULT "):])
    if proc.returncode != 0 or result is None:
        sys.stdout.write(proc.stdout[-6000:])
        sys.stderr.write(proc.stderr[-6000:])
        raise SystemExit("Blender failed (exit %d)" % proc.returncode)
    if args.verbose:
        sys.stdout.write(proc.stdout)
    for w in result["warnings"]:
        print("warning:", w)
    for f in result["written"]:
        print("wrote", f)
    return result


def cmd_info(args):
    fs = vfs.GameFS(game_folders(args))
    path = args.input
    data = open(path, "rb").read() if os.path.exists(path) else fs.read(path)
    if data is None:
        raise SystemExit("cannot find %s" % path)
    low = path.lower()
    if low.endswith(".tik"):
        reader = (lambda p: open(path, encoding="latin-1").read() if p == path else fs.read_text(p))
        tk = tiki.parse(reader if os.path.exists(path) else fs.read_text, path)
        print("scale", tk.scale)
        for m in tk.skelmodels:
            print("skelmodel", m, "" if fs.exists(m) else "(missing)")
        for s in tk.surfaces.values():
            print("surface %-20s shader %s" % (s.name, " ".join(s.shaders)))
        print("%d animations" % len(tk.anims))
        for a in tk.anims[:args.limit]:
            print("  %-28s %s%s" % (a.alias, a.path, "" if fs.exists(a.path) else " (missing)"))
    elif low.endswith((".skd", ".skb")):
        m = skd.read(data)
        print("%s version %d, %d bones, %d surfaces, %d morph targets" % (m.name, m.version, len(m.bones),
                                                                          len(m.surfaces), len(m.morph_names)))
        for b in m.bones:
            print("  bone %-28s parent %-24s %s" % (b.name, b.parent or "-", skd.BONE_TYPE_NAMES[b.type]))
        for s in m.surfaces:
            print("  surface %-20s %5d verts %5d tris" % (s.name, len(s.verts), len(s.triangles)))
    elif low.endswith(".skc"):
        a = skc.read(data)
        print("version %d, %d frames at %.4fs, %d channels, delta %s" % (a.version, a.num_frames, a.frame_time,
                                                                        len(a.channels), a.total_delta))
        for c in a.channels:
            print("  " + c)
    elif low.endswith(".shader") or "." not in os.path.basename(path):
        info = shader.ShaderLibrary(fs).resolve(path)
        print(vars(info))


def cmd_extract(args):
    """A model and everything it uses, out of the paks into a folder tree."""
    fs = vfs.GameFS(game_folders(args))
    tk = tiki.parse(fs.read_text, args.input)
    lib = shader.ShaderLibrary(fs)
    want = [args.input] + list(tk.skelmodels) + [a.path for a in tk.anims]
    for s in tk.surfaces.values():
        for sh in s.shaders:
            info = lib.resolve(sh)
            want += [p for p in (info.image, info.normal_map) if p]
    n = 0
    for p in dict.fromkeys(want):
        data = fs.read(p)
        if data is None:
            print("missing", p)
            continue
        out = os.path.join(args.output, *fs.real_name(p).split("/"))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(data)
        n += 1
    print("extracted %d files to %s" % (n, args.output))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("import", "export", "preview", "info", "extract"))
    ap.add_argument("input")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--ta", action="store_true", help="add mainta (Spearhead)")
    ap.add_argument("--tt", action="store_true", help="add maintt (Breakthrough)")
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("--limit", type=int, default=50, help="info: animations listed")
    args, extra = ap.parse_known_args()
    if args.command == "info":
        return cmd_info(args)
    if not args.output:
        ap.error("%s needs an output" % args.command)
    if args.command == "extract":
        return cmd_extract(args)
    run_blender(args, args.command, args.input, args.output, extra)


if __name__ == "__main__":
    main()
