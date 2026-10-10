#!/usr/bin/env python3
"""A pk3 of the remastered models a review chose "new" for.

    tools/blender/remaster/review_pack.py decisions.json ~/remaster-all OUT.pk3

decisions.json is the review page's decision store as a list of {id, data} rows (an
export of its "decisions" collection); WORK is the remaster run. A .skd shared by several
models goes in only when none of them chose "original". Models marked "again" are listed
with their notes and left out.
"""

import json
import os
import sys
import zipfile


def main():
    dec_path, work, pk3 = sys.argv[1:4]
    with open(dec_path) as f:
        rows = json.load(f)
    decisions = {r["id"]: r["data"] for r in rows}
    with open(os.path.join(work, "review", "models.json")) as f:  # review_data.py WORK WORK/review
        models = json.load(f)["models"]
    by_skd = {}
    for m in models:
        for skd in m["skds"]:
            by_skd.setdefault(skd, []).append(m)
    chosen, vetoed, again = set(), set(), []
    for m in models:
        d = decisions.get(m["key"], {})
        c = d.get("choice")
        if c == "again":
            again.append((m["model"], d.get("tags", []), d.get("note", "")))
        for skd in m["skds"]:
            if c == "new":
                chosen.add(skd)
            elif c == "original":
                vetoed.add(skd)
    files = sorted(chosen - vetoed)
    for skd in sorted(chosen & vetoed):
        print("left out (another model using it chose original): %s" % skd)
    tmp = pk3 + ".part"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        for skd in files:
            z.write(os.path.join(work, "out", *skd.split("/")), skd)
    os.replace(tmp, pk3)
    undecided = sum(1 for m in models if m["key"] not in decisions)
    print("wrote %s: %d meshes (%d models undecided, %d for another pass)" % (pk3, len(files), undecided, len(again)))
    for model, tags, note in again:
        print("again: %s: %s" % (model, "; ".join(tags + ([note] if note else []))))


if __name__ == "__main__":
    main()
