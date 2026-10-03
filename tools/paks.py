#!/usr/bin/env python3
"""What the game's paks hold, from an index made once.

The game loads main/*.pk3 in name order and a later pak's file wins. This
indexes every pak once (redone for a pak only when it changes) so a question
about game data is one quick call instead of a scan of every pak.

    tools/paks.py find 'models/fx/.*explo'     # paths matching a regex, and the pak that wins
    tools/paks.py which models/fx/grenexp_base.tik   # every pak that has it, the winner last
    tools/paks.py cat models/fx/grenexp_base.tik     # the copy the game loads
    tools/paks.py shader gren_boom             # a shader's definition, from the winning shader file

MOHAA_GAME (default /mnt/d/Medal of Honor) is the folder with main/.
"""

import argparse
import json
import os
import re
import sys
import zipfile

GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")
MAIN = os.path.join(GAME, "main")
CACHE = os.path.expanduser("~/.cache/openmohaa-paks.json")


def paks():
    # The engine's order: by name, case-insensitively; a later pak wins.
    names = [n for n in os.listdir(MAIN) if n.lower().endswith(".pk3")]
    return sorted(names, key=str.lower)


def index():
    """{pak: [paths]} for every pak, from the cache where the pak is unchanged."""
    try:
        cache = json.load(open(CACHE))
    except (OSError, ValueError):
        cache = {}
    out, changed = {}, False
    for name in paks():
        st = os.stat(os.path.join(MAIN, name))
        key = f"{st.st_size}:{int(st.st_mtime)}"
        entry = cache.get(name)
        if not entry or entry["key"] != key:
            try:
                files = zipfile.ZipFile(os.path.join(MAIN, name)).namelist()
            except zipfile.BadZipFile:
                files = []
            entry = {"key": key, "files": [f for f in files if not f.endswith("/")]}
            changed = True
        out[name] = entry
    if changed or set(cache) != set(out):
        os.makedirs(os.path.dirname(CACHE), exist_ok=True)
        json.dump(out, open(CACHE, "w"))
    return {name: entry["files"] for name, entry in out.items()}


def holders(idx, path):
    want = path.lower()
    return [name for name, files in idx.items() if any(f.lower() == want for f in files)]


def read(pak, path):
    z = zipfile.ZipFile(os.path.join(MAIN, pak))
    for f in z.namelist():
        if f.lower() == path.lower():
            return z.read(f)
    return None


def cmd_find(args):
    rx = re.compile(args.pattern, re.I)
    winner = {}
    count = {}
    for name, files in index().items():
        for f in files:
            if rx.search(f):
                winner[f.lower()] = (f, name)
                count[f.lower()] = count.get(f.lower(), 0) + 1
    for key in sorted(winner)[: args.limit]:
        f, name = winner[key]
        extra = f"  (+{count[key] - 1} older)" if count[key] > 1 else ""
        print(f"{f}  <- {name}{extra}")
    if len(winner) > args.limit:
        print(f"... {len(winner) - args.limit} more (--limit)")


def cmd_which(args):
    found = holders(index(), args.path)
    if not found:
        sys.exit("in no pak")
    for name in found:
        print(name)


def cmd_cat(args):
    found = holders(index(), args.path)
    if not found:
        sys.exit("in no pak")
    data = read(found[-1], args.path)
    sys.stdout.write(data.decode("latin-1"))


def cmd_shader(args):
    idx = index()
    # The winning copy of each shader file, in load order.
    files = {}
    for name, paths in idx.items():
        for f in paths:
            if f.lower().endswith(".shader"):
                files[f.lower()] = (f, name)
    rx = re.compile(r"^" + re.escape(args.name) + r"\s*\n\s*\{", re.I | re.M)
    hits = []
    for f, name in files.values():
        text = read(name, f).decode("latin-1")
        m = rx.search(text)
        if not m:
            continue
        depth, i = 0, m.end() - 1
        while i < len(text):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
            if depth == 0:
                break
        hits.append((name, f, text[m.start():i]))
    if not hits:
        sys.exit("no such shader")
    # The engine takes the last definition it reads; say which that is.
    for name, f, body in hits[:-1]:
        print(f"// also in {name}: {f}")
    name, f, body = hits[-1]
    print(f"// {name}: {f}\n{body}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="what", required=True)
    p = sub.add_parser("find")
    p.add_argument("pattern")
    p.add_argument("--limit", type=int, default=60)
    p.set_defaults(fn=cmd_find)
    p = sub.add_parser("which")
    p.add_argument("path")
    p.set_defaults(fn=cmd_which)
    p = sub.add_parser("cat")
    p.add_argument("path")
    p.set_defaults(fn=cmd_cat)
    p = sub.add_parser("shader")
    p.add_argument("name")
    p.set_defaults(fn=cmd_shader)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
