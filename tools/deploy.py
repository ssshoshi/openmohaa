#!/usr/bin/env python3
"""Build main and put it into the game installs, in one go.

    tools/deploy.py                 # main into ragdoll, physics-test and live, and the paks
    tools/deploy.py --rtlight       # also openmohaa-rtlight (main + the rtlight worktree's work)
    tools/deploy.py --dry-run       # say what would change
    tools/deploy.py --only paks     # or --only binaries

Main is built in ~/projects/openmohaa-main (the report pipeline's worktree,
kept clean on main), fast-forwarded first. The paks are the data/ folders
(cmake), the content/ paks (tools/content/pack.py) and the m3l2 add-on
(tools/m3l2-assault/build.py); they go into the shared main/ folder, with any
copy they replace kept in pk3-backups/ (never in main/: a backup there would be
loaded as a pak). Each install's replaced binaries are kept beside them as
*.bak-prev. Nothing is copied into an install whose game is running.

The rtlight install is main plus the realtime lighting work: its branch
(feat/realtime-lighting) merged onto main, plus whatever is uncommitted in its
worktree, built in ~/projects/openmohaa-rtlight-deploy. Commit that work to
its branch and this stops depending on the worktree.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time

GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")
GAME_MAIN = os.path.join(GAME, "main")
PAK_BACKUPS = os.path.join(GAME, "pk3-backups")
SRC = os.path.expanduser("~/projects/openmohaa-main")
RTLIGHT = os.path.expanduser("~/projects/openmohaa-rtlight")
RTLIGHT_BUILD = os.path.expanduser("~/projects/openmohaa-rtlight-deploy")
REMOTE = "fork"
INSTALLS = ["openmohaa-ragdoll", "openmohaa-physics-test", "openmohaa-live"]
BINARIES = ["openmohaa.exe", "cgame.dll", "game.dll", "renderer_opengl1.dll", "renderer_opengl2.dll", "omohaaded.exe"]
JOBS = "4"  # WSL has 7 GB; a wider build has crashed it


def run(cmd, cwd=None, quiet=True):
    r = subprocess.run(cmd, cwd=cwd, capture_output=quiet, text=True)
    if r.returncode:
        if quiet:
            sys.stderr.write((r.stdout or "")[-3000:] + (r.stderr or "")[-3000:])
        sys.exit(f"failed: {' '.join(cmd)}")
    return (r.stdout or "").strip()


def md5(path):
    return hashlib.md5(open(path, "rb").read()).hexdigest() if os.path.isfile(path) else None


def running_installs():
    out = subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                          "Get-Process openmohaa,omohaaded -ErrorAction SilentlyContinue | % { $_.Path }"],
                         capture_output=True, text=True).stdout
    return {line.strip().split("\\")[-2] for line in out.splitlines() if "\\" in line}


def build(tree):
    b = os.path.join(tree, "build", "win64")
    if not os.path.isfile(os.path.join(b, "CMakeCache.txt")):
        run(["cmake", "-S", tree, "-B", b, "-DCMAKE_BUILD_TYPE=Release",
             f"-DCMAKE_TOOLCHAIN_FILE={tree}/cmake/toolchains/mingw-w64-x86_64.cmake"])
    # A fresh configure leaves GL2 off (the option's default).
    run(["cmake", "-S", tree, "-B", b, "-DBUILD_RENDERER_GL2=ON"])
    print(f"building {tree} ...")
    run(["cmake", "--build", b, f"-j{JOBS}"])
    return os.path.join(b, "Release")


def update_main(dry):
    run(["git", "fetch", REMOTE, "-q"], cwd=SRC)
    if run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=SRC):
        sys.exit(f"{SRC} has uncommitted changes; it should be a clean main")
    if not dry:
        run(["git", "checkout", "-q", "main"], cwd=SRC)
        run(["git", "merge", "--ff-only", "-q", f"{REMOTE}/main"], cwd=SRC)
    return run(["git", "log", "-1", "--format=%h %s", f"{REMOTE}/main"], cwd=SRC)


def rtlight_tree(dry):
    """A worktree at main with the rtlight branch merged and the rtlight
    worktree's uncommitted work applied on top."""
    if dry:
        return RTLIGHT_BUILD
    if not os.path.isdir(RTLIGHT_BUILD):
        run(["git", "worktree", "add", "-q", "--detach", RTLIGHT_BUILD, f"{REMOTE}/main"], cwd=SRC)
    run(["git", "reset", "-q", "--hard", f"{REMOTE}/main"], cwd=RTLIGHT_BUILD)
    run(["git", "clean", "-qfd", "-e", "build"], cwd=RTLIGHT_BUILD)
    branch = run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=RTLIGHT)
    if branch != "HEAD":
        run(["git", "merge", "-q", "--no-edit", branch], cwd=RTLIGHT_BUILD)
    diff = subprocess.run(["git", "diff", "HEAD", "--binary"], cwd=RTLIGHT, capture_output=True).stdout
    if diff:
        r = subprocess.run(["git", "apply", "--3way"], cwd=RTLIGHT_BUILD, input=diff, capture_output=True)
        if r.returncode:
            sys.exit("the rtlight worktree's uncommitted work does not apply on main:\n" + r.stderr.decode()[-2000:])
    for f in run(["git", "ls-files", "--others", "--exclude-standard"], cwd=RTLIGHT).splitlines():
        os.makedirs(os.path.dirname(os.path.join(RTLIGHT_BUILD, f)) or ".", exist_ok=True)
        shutil.copy2(os.path.join(RTLIGHT, f), os.path.join(RTLIGHT_BUILD, f))
    return RTLIGHT_BUILD


def install_binaries(release, install, dry):
    dest = os.path.join(GAME, install)
    changed = []
    for f in BINARIES:
        src, dst = os.path.join(release, f), os.path.join(dest, f)
        if not os.path.isfile(src) or (f == "omohaaded.exe" and not os.path.isfile(dst)):
            continue
        if md5(src) == md5(dst):
            continue
        changed.append(f)
        if not dry:
            if os.path.isfile(dst):
                shutil.copy2(dst, dst + ".bak-prev")
            shutil.copy2(src, dst)
            if md5(src) != md5(dst):
                sys.exit(f"copy did not take: {dst}")
    return changed


def build_paks(dry):
    out = os.path.join(SRC, "build", "deploy-paks")
    if dry:
        return out
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    data = os.path.join(SRC, "build", "win64", "data")
    for f in os.listdir(data) if os.path.isdir(data) else []:
        if f.endswith(".pk3"):
            shutil.copy2(os.path.join(data, f), out)
    run([sys.executable, "tools/content/pack.py", "--game", GAME, "--out", out], cwd=SRC)
    run([sys.executable, "tools/m3l2-assault/build.py", "--out",
         os.path.join(out, "zzzzzzzzz-opm-m3l2-assault.pk3")], cwd=SRC)
    return out


def install_paks(folder, dry):
    stamp = time.strftime("%Y-%m-%d_%H%M")
    changed = []
    for f in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
        src, dst = os.path.join(folder, f), os.path.join(GAME_MAIN, f)
        if md5(src) == md5(dst):
            continue
        changed.append(f)
        if not dry:
            if os.path.isfile(dst):
                os.makedirs(os.path.join(PAK_BACKUPS, stamp), exist_ok=True)
                shutil.copy2(dst, os.path.join(PAK_BACKUPS, stamp, f))
            shutil.copy2(src, dst)
    return changed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rtlight", action="store_true", help="also build and install openmohaa-rtlight")
    ap.add_argument("--only", choices=["binaries", "paks"])
    ap.add_argument("--installs", nargs="+", default=INSTALLS)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    dry = args.dry_run

    targets = list(args.installs) + (["openmohaa-rtlight"] if args.rtlight else [])
    busy = running_installs() & set(targets)
    if busy and args.only != "paks":
        sys.exit(f"the game is running from {', '.join(sorted(busy))}; close it first")

    print("main:", update_main(dry))
    # The paks under data/ come out of the same build as the binaries.
    release = os.path.join(SRC, "build", "win64", "Release") if dry else build(SRC)
    if dry:
        print(f"(binaries compared with the last build in {release}; a real run rebuilds first)")

    if args.only != "paks":
        for inst in args.installs:
            changed = install_binaries(release, inst, dry)
            print(f"{inst:24} {', '.join(changed) if changed else 'up to date'}")
            if inst == "openmohaa-live" and changed and not dry:
                # Records the interfaces orch.py swap checks against.
                run([sys.executable, "tools/orchestrator/orch.py", "setup", "--build", release], cwd=SRC)
        if args.rtlight and dry:
            print(f"{'openmohaa-rtlight':24} would rebuild from main + {RTLIGHT}'s work")
        elif args.rtlight:
            tree = rtlight_tree(dry)
            rt_release = os.path.join(tree, "build", "win64", "Release") if dry else build(tree)
            changed = install_binaries(rt_release, "openmohaa-rtlight", dry)
            print(f"{'openmohaa-rtlight':24} {', '.join(changed) if changed else 'up to date'}")

    if args.only != "binaries":
        folder = build_paks(dry)
        changed = install_paks(folder, dry)
        print(f"{'paks in main/':24} {', '.join(changed) if changed else 'up to date'}")

    if dry:
        print("(dry run: nothing was changed)")


if __name__ == "__main__":
    main()
