#!/usr/bin/env python3
"""Remastering the game's models in batches: new detail from TRELLIS (or meshes you supply)
on the original rigs, UVs and textures, with smoothing where that fails.

    tools/blender/mohaa_remaster.py run models/weapons/kar98.tik 'models/human/allied_*.tik' \\
        --work ~/remaster --pk3 ~/remaster/zzz-remaster.pk3

For every model:
  prepare   Blender imports it with the game's textures (yours, when your texture pk3 is in
            main/), lifts its arms and renders it from around: WORK/<model>/views/
  generate  TRELLIS turns the renders into candidate meshes: WORK/<model>/candidates/
            (runs only when --trellis-python or $TRELLIS_PYTHON is set; a candidate already
            there is kept). Meshes of your own go there too, or in --detail-dir under the
            model's game path (models/weapons/kar98.obj, or a folder of that name).
  build     Blender fits the best candidate to the model, cuts it into the original surfaces
            with their UVs, weights and morphs, checks each surface's shape and its
            stretching over the model's animations, and smooths the surfaces that fail.
            The changed .skd files go to WORK/out/ at their game paths; the game's .tik and
            animations are untouched. WORK/<model>/review/before_after.png and report.json
            say what happened.
  pack      WORK/out/ into a pk3 (--pk3), for main/ (after your texture pk3, by name).

Steps can be run on their own (prepare, generate, build, pack) or all at once (run); a
step whose output is there is skipped unless --redo names it.

TRELLIS: github.com/microsoft/TRELLIS, installed with its own environment (Linux or WSL,
CUDA). --trellis-python is that environment's python (e.g. ~/miniconda3/envs/trellis/bin/python)
and --trellis-home its checkout (default: $TRELLIS_HOME); the generator runs with the
checkout as its working folder so its package imports.
"""

import argparse
import fnmatch
import json
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "io_scene_mohaa"))

import mohaa_blender  # noqa: E402
from mohaa import vfs  # noqa: E402

MESH_EXTS = (".obj", ".glb", ".gltf", ".ply", ".fbx", ".stl")


def expand_models(args):
    """Game paths (globs allowed, against the paks) and files on disk."""
    fs = None
    out = []
    for m in args.models:
        if os.path.exists(m):
            out.append(m)
            continue
        if any(c in m for c in "*?["):
            if fs is None:
                fs = vfs.GameFS(mohaa_blender.game_folders(args))
            top = m.split("/")[0] if "/" in m else ""
            found = [p for p in fs.listdir(top, ".tik") if fnmatch.fnmatch(p.lower(), m.lower())]
            if not found:
                print("nothing matches", m)
            out += found
        else:
            out.append(m)
    return list(dict.fromkeys(out))


def work_dir(args, model):
    rel = os.path.splitext(model if not os.path.isabs(model) else os.path.basename(model))[0]
    return os.path.join(args.work, *rel.replace("\\", "/").split("/"))


def step_prepare(args, model, wd):
    if os.path.exists(os.path.join(wd, "source.blend")) and "prepare" not in args.redo:
        return
    os.makedirs(wd, exist_ok=True)
    src = mohaa_blender.materialize(model, args, wd)  # a game path goes to WD/game/
    extra = ["--spread", str(args.spread), "--check-anims", args.check_anims, "--size", str(args.size)]
    mohaa_blender.run_blender(args, "remaster-prepare", src, wd, extra)


def candidates(args, model, wd):
    found = []
    cdir = os.path.join(wd, "candidates")
    if os.path.isdir(cdir):
        found += [os.path.join(cdir, n) for n in sorted(os.listdir(cdir)) if n.lower().endswith(MESH_EXTS)]
    if args.detail_dir and not os.path.isabs(model):
        stem = os.path.join(args.detail_dir, *os.path.splitext(model)[0].split("/"))
        for ext in MESH_EXTS:
            if os.path.exists(stem + ext):
                found.append(stem + ext)
        if os.path.isdir(stem):
            found += [os.path.join(stem, n) for n in sorted(os.listdir(stem)) if n.lower().endswith(MESH_EXTS)]
    return found


def step_generate(args, model, wd):
    py = args.trellis_python or os.environ.get("TRELLIS_PYTHON")
    if not py:
        if not candidates(args, model, wd):
            print("  no TRELLIS (--trellis-python) and no candidate mesh: the build will smooth only")
        return
    home = args.trellis_home or os.environ.get("TRELLIS_HOME") or None
    cdir = os.path.join(wd, "candidates")
    if "generate" in args.redo and os.path.isdir(cdir):
        for n in os.listdir(cdir):
            if n.startswith("trellis_"):
                os.remove(os.path.join(cdir, n))
    cmd = [py, os.path.join(HERE, "remaster", "trellis_generate.py"), os.path.join(wd, "views"), cdir,
           "--seeds", args.seeds]
    if args.verbose:
        print(" ".join(cmd))
    proc = subprocess.run(cmd, cwd=home)
    if proc.returncode != 0:
        print("  TRELLIS failed (exit %d)" % proc.returncode)


def step_build(args, model, wd):
    if os.path.exists(os.path.join(wd, "report.json")) and "build" not in args.redo:
        return
    cands = candidates(args, model, wd)
    win = mohaa_blender.is_windows_exe(mohaa_blender.find_blender())
    opts = {"factor": args.factor, "max_tris": args.max_tris, "keep": args.keep,
            "fit_tolerance": args.fit_tolerance, "smooth": not args.no_smooth}
    extra = ["--candidates", ";".join(mohaa_blender.to_host(c, win) for c in cands), "--options", json.dumps(opts)]
    out_root = os.path.join(args.work, "out")
    os.makedirs(out_root, exist_ok=True)
    result = mohaa_blender.run_blender(args, "remaster-build", wd, out_root, extra)
    r = result.get("report", {})
    counts = {}
    for e in r.get("surfaces", {}).values():
        counts[e["result"]] = counts.get(e["result"], 0) + 1
    print("  %s" % ", ".join("%d %s" % (n, k) for k, n in sorted(counts.items())))


def step_pack(args):
    out_root = os.path.join(args.work, "out")
    if not args.pk3:
        return
    files = []
    for root, _dirs, names in os.walk(out_root):
        for n in names:
            files.append(os.path.join(root, n))
    if not files:
        print("nothing to pack")
        return
    tmp = args.pk3 + ".part"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(files):
            z.write(f, os.path.relpath(f, out_root).replace("\\", "/"))
    shutil.move(tmp, args.pk3)
    print("wrote %s (%d files)" % (args.pk3, len(files)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", choices=("run", "prepare", "generate", "build", "pack"))
    ap.add_argument("models", nargs="*", help="game paths (globs allowed) or .tik files")
    ap.add_argument("--work", required=True, help="working folder (renders, candidates, reports, out/)")
    ap.add_argument("--pk3", help="pack: the pk3 to write")
    ap.add_argument("--game", default=mohaa_blender.DEFAULT_GAME)
    ap.add_argument("--ta", action="store_true")
    ap.add_argument("--tt", action="store_true")
    ap.add_argument("--redo", default="", help="steps to run again although done: prepare,generate,build")
    ap.add_argument("--spread", type=float, default=30.0, help="degrees the arms are lifted for generation")
    ap.add_argument("--check-anims", default="auto", help="animation aliases for the stretch check (auto: a few)")
    ap.add_argument("--size", type=int, default=1024, help="render size for TRELLIS")
    ap.add_argument("--trellis-python")
    ap.add_argument("--trellis-home")
    ap.add_argument("--seeds", default="1,2,3", help="one TRELLIS candidate per seed")
    ap.add_argument("--detail-dir", help="meshes of your own, under the models' game paths")
    ap.add_argument("--factor", type=float, default=4.0, help="triangles, as a multiple of the original's")
    ap.add_argument("--max-tris", type=int, default=30000, help="triangles for a whole model")
    ap.add_argument("--keep", action="append", default=[], help="surface name pattern left as it is")
    ap.add_argument("--fit-tolerance", type=float, default=0.03, help="of the model's size")
    ap.add_argument("--no-smooth", action="store_true", help="keep failing surfaces as they are")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()
    args.work = os.path.abspath(os.path.expanduser(args.work))
    args.redo = {s.strip() for s in args.redo.split(",") if s.strip()}

    if args.step != "pack":
        models = expand_models(args)
        if not models:
            ap.error("no models")
        for i, model in enumerate(models):
            wd = work_dir(args, model)
            print("[%d/%d] %s" % (i + 1, len(models), model))
            try:
                if args.step in ("run", "prepare"):
                    step_prepare(args, model, wd)
                if args.step in ("run", "generate"):
                    step_generate(args, model, wd)
                if args.step in ("run", "build"):
                    step_build(args, model, wd)
            except SystemExit as e:  # one model failing does not stop the batch
                print("  failed: %s" % e)
    if args.step in ("run", "pack"):
        step_pack(args)


if __name__ == "__main__":
    main()
