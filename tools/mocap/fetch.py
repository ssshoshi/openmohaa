#!/usr/bin/env python3
"""Fetch CMU motion capture clips (the cgspeed BVH conversion) by take id.

    tools/mocap/fetch.py 77_02 137_41 [--dir ~/.cache/openmohaa-mocap]
    tools/mocap/fetch.py --search "wait|idle"

The clips come from github.com/Shriinivas/cmubvh, which mirrors the cgspeed
release: CMU places no restrictions on the data and the conversion adds none.
They are downloaded, never committed. Prints each clip's local path.
"""
import argparse
import html
import io
import os
import re
import subprocess
import zipfile

REPO = "repos/Shriinivas/cmubvh/contents"
CACHE = os.path.expanduser("~/.cache/openmohaa-mocap")


def gh_raw(path):
    return subprocess.run(["gh", "api", REPO + "/" + path, "-H", "Accept: application/vnd.github.raw"],
                          check=True, capture_output=True).stdout


def gh_names(path):
    out = subprocess.run(["gh", "api", REPO + "/" + path, "--jq", ".[].name"],
                         check=True, capture_output=True, text=True).stdout
    return out.split()


def index(cache):
    """take id -> description, from the spreadsheet the release ships."""
    path = os.path.join(cache, "index.txt")
    if not os.path.exists(path):
        ods = zipfile.ZipFile(io.BytesIO(gh_raw("cmu-mocap-index-spreadsheet.ods")))
        xml = ods.read("content.xml").decode()
        rows = []
        for row in re.findall(r"<table:table-row.*?</table:table-row>", xml, re.S):
            cells = [html.unescape(re.sub("<[^>]+>", "", c))
                     for c in re.findall(r"<table:table-cell[^>]*?>(.*?)</table:table-cell>", row, re.S)]
            if cells and re.match(r"\d+_\d+$", cells[0]):
                rows.append("\t".join(cells[:3]))
        open(path, "w").write("\n".join(rows) + "\n")
    return dict(line.split("\t", 1) for line in open(path).read().splitlines())


def fetch(take, cache):
    out = os.path.join(cache, take + ".bvh")
    if os.path.exists(out):
        return out
    subject = int(take.split("_")[0])
    folder = None
    for name in gh_names(""):
        m = re.match(r"Sequence-(\d+)-(\d+)$", name)
        if m and int(m.group(1)) <= subject <= int(m.group(2)):
            folder = name
    if folder is None:
        raise SystemExit("no folder holds subject %d" % subject)
    data = gh_raw("%s/%02d/Data/%s.zip" % (folder, subject, take))
    z = zipfile.ZipFile(io.BytesIO(data))
    bvh = next(n for n in z.namelist() if n.endswith(".bvh"))
    open(out, "wb").write(z.read(bvh))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("takes", nargs="*")
    ap.add_argument("--search", help="regex over the take descriptions")
    ap.add_argument("--dir", default=CACHE)
    a = ap.parse_args()
    os.makedirs(a.dir, exist_ok=True)
    if a.search:
        for take, desc in index(a.dir).items():
            if re.search(a.search, desc, re.I):
                print(take, "\t", desc.replace("\t", "  |  "))
    for take in a.takes:
        print(fetch(take, a.dir))


if __name__ == "__main__":
    main()
