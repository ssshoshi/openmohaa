#!/usr/bin/env python3
"""Builds the content paks in content/ into zzzzzzzzz-<name>.pk3 files.

Each content/<name>/ folder becomes one pak. A *.patch file in it is a unified
diff against a game file (retail or mod) that is not ours to publish: the
original is taken from the player's own paks, the one the game would load
(the last pak in load order that has it, opm paks left out), and the diff is
applied to it. Any other file is our own and goes in as it is, at its path
under the folder.

    pack.py                         build every pak into build/content/
    pack.py opm-cabinet-ragdoll     build just that one
    pack.py --install DIR           also copy the paks into DIR (a main folder)
    pack.py --diff NAME PATH FILE   write content/NAME/<PATH>.patch from an
                                    edited copy FILE of the game's PATH

The game's files are read with LF line ends and the diffs are LF; the files go
into the pak with CRLF, as the retail ones are.
"""

import argparse
import difflib
import glob
import io
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CONTENT = os.path.join(ROOT, "content")
GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")
PREFIX = "zzzzzzzzz-"


class Skip(Exception):
    """A pak that cannot be built from the game installed (its base is missing or different)."""


def game_paks(game):
    """The game's paks in load order (later ones win), ours left out."""
    paks = sorted(glob.glob(os.path.join(game, "main", "*.pk3")), key=lambda p: os.path.basename(p).lower())
    return [p for p in paks if not os.path.basename(p).lower().startswith(PREFIX + "opm-")]


def original(game, path):
    """The game's own copy of path, with LF line ends, and the pak it came from."""
    found = None
    for pak in game_paks(game):
        with zipfile.ZipFile(pak) as z:
            names = {n.lower(): n for n in z.namelist()}
            if path.lower() in names:
                found = (z.read(names[path.lower()]), pak)
    if not found:
        raise Skip(f"{path} is not in any pak under {game}/main")
    data, pak = found
    return data.decode("latin1").replace("\r\n", "\n"), pak


def patch_target(patch_file):
    with open(patch_file, encoding="latin1") as f:
        for line in f:
            if line.startswith("+++ "):
                path = line[4:].split("\t")[0].strip()
                return path[2:] if path.startswith("b/") else path
    sys.exit(f"{patch_file}: no +++ line")


def apply(text, patch_file):
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "file")
        with open(src, "w", encoding="latin1", newline="\n") as f:
            f.write(text)
        result = subprocess.run(["patch", "--quiet", "--no-backup-if-mismatch", src, patch_file],
                                capture_output=True, text=True)
        if result.returncode:
            raise Skip(f"{os.path.relpath(patch_file, CONTENT)} does not apply to the game's copy:\n"
                       f"{result.stdout}{result.stderr}")
        with open(src, encoding="latin1", newline="\n") as f:
            return f.read()


def build(name, game, out):
    folder = os.path.join(CONTENT, name)
    files = {}
    for dirpath, _, filenames in os.walk(folder):
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, folder).replace(os.sep, "/")
            if rel == "README.md":
                continue
            if fn.endswith(".patch"):
                path = patch_target(full)
                text, pak = original(game, path)
                files[path] = apply(text, full).replace("\n", "\r\n").encode("latin1")
                print(f"  {path}  (patched from {os.path.basename(pak)})")
            else:
                with open(full, "rb") as f:
                    files[rel] = f.read()
                print(f"  {rel}")

    os.makedirs(out, exist_ok=True)
    target = os.path.join(out, PREFIX + name + ".pk3")
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted(files):
            z.writestr(path, files[path])
    return target


def make_diff(name, path, edited, game):
    text, pak = original(game, path)
    with open(edited, encoding="latin1") as f:
        new = f.read().replace("\r\n", "\n")
    diff = difflib.unified_diff(text.splitlines(True), new.splitlines(True), "a/" + path, "b/" + path)
    target = os.path.join(CONTENT, name, path + ".patch")
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target, "w", encoding="latin1", newline="\n") as f:
        f.writelines(diff)
    print(f"{target}  (against {os.path.basename(pak)})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="content folders to build (default: all)")
    ap.add_argument("--game", default=GAME, help="fs_basepath, the folder holding main/*.pk3")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "content"))
    ap.add_argument("--install", help="also copy the built paks into this main folder")
    ap.add_argument("--diff", nargs=3, metavar=("NAME", "PATH", "FILE"))
    args = ap.parse_args()

    if args.diff:
        make_diff(*args.diff, args.game)
        return

    names = args.names or sorted(d for d in os.listdir(CONTENT) if os.path.isdir(os.path.join(CONTENT, d)))
    for name in names:
        print(name)
        try:
            target = build(name, args.game, args.out)
        except Skip as e:
            print(f"  skipped: {e}")
            continue
        print(f"  -> {target}")
        if args.install:
            shutil.copy2(target, args.install)
            print(f"  installed in {args.install}")


if __name__ == "__main__":
    main()
