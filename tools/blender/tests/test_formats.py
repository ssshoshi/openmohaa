#!/usr/bin/env python3
"""Round trips every .skd/.skb/.skc in the game's paks through the format library.

    tools/blender/tests/test_formats.py [--game "/mnt/d/Medal of Honor"] [--limit N]

Every file must come back with the same data; most SKD 5 and SKC 13 files also
come back byte for byte (the rest carry bytes the engine never reads: data past
the hit boxes, junk after the NUL of a channel name).
SKB and SKC 14 are written back as SKD 5 and SKC 13, as the exporter does.
"""

import argparse
import os
import sys
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "io_scene_mohaa"))

from mohaa import skc, skd  # noqa: E402


def same_model(a, b):
    assert [x.name for x in a.bones] == [x.name for x in b.bones]
    assert [x.parent for x in a.bones] == [x.parent for x in b.bones]
    assert len(a.surfaces) == len(b.surfaces)
    for sa, sb in zip(a.surfaces, b.surfaces):
        assert sa.name == sb.name and sa.triangles == sb.triangles
        assert len(sa.verts) == len(sb.verts)
        for va, vb in zip(sa.verts, sb.verts):
            assert va.weights == vb.weights and va.morphs == vb.morphs and va.uv == vb.uv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor"))
    ap.add_argument("--limit", type=int, default=0, help="files per kind per pak (0 = all)")
    args = ap.parse_args()

    counts = {}
    failures = 0
    for mod in ("main", "mainta", "maintt"):
        folder = os.path.join(args.game, mod)
        if not os.path.isdir(folder):
            continue
        for pak in sorted(os.listdir(folder), key=str.lower):
            if not pak.lower().endswith(".pk3"):
                continue
            try:
                z = zipfile.ZipFile(os.path.join(folder, pak))
            except zipfile.BadZipFile:
                continue
            seen = {}
            for name in z.namelist():
                low = name.lower()
                ext = low[-4:]
                if ext not in (".skd", ".skb", ".skc") or low.startswith("newanim/"):
                    continue
                if args.limit and seen.get(ext, 0) >= args.limit:
                    continue
                data = z.read(name)
                version = int.from_bytes(data[4:8], "little")
                key = "%s v%d" % (ext, version)
                try:
                    if ext == ".skc":
                        if version not in (13, 14):
                            continue  # the engine refuses these too
                        a = skc.read(data)
                        out = skc.write(a)
                        b = skc.read(out)
                        assert b.channels == a.channels and skc.write(b) == out  # NaNs compare unequal
                        if version == 13 and out == data:
                            key += " (same bytes)"
                    else:
                        m = skd.read(data)
                        out = skd.write(m, 6 if version == 6 else 5)
                        b = skd.read(out)
                        same_model(m, b)
                        assert b.boxes == m.boxes and b.morph_names == m.morph_names
                        if version == 5 and out == data:
                            key += " (same bytes)"
                    counts[key] = counts.get(key, 0) + 1
                    seen[ext] = seen.get(ext, 0) + 1
                except Exception as e:  # noqa: BLE001
                    failures += 1
                    print("FAIL %s/%s:%s: %s" % (mod, pak, name, e))
    for k in sorted(counts):
        print("%-10s %d ok" % (k, counts[k]))
    print("failures:", failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
