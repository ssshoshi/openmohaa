#!/usr/bin/env python3
"""The data for a remaster review page: one entry per model a remaster run changed, with its
before/after sheet as a JPEG.

    tools/blender/remaster/review_data.py ~/remaster-all OUT_DIR

writes OUT_DIR/models.json and OUT_DIR/sheets/<key>.jpg (key: the model's game path with
"/" as "~", the same key the review page stores decisions under).
"""

import glob
import json
import os
import sys

from PIL import Image


def key_of(model):
    return model.replace("/", "~")


def main():
    work, out = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.join(out, "sheets"), exist_ok=True)
    entries, unchanged = [], []
    skd_users = {}
    for rp in sorted(glob.glob(os.path.join(work, "models", "**", "report.json"), recursive=True)):
        model = os.path.relpath(os.path.dirname(rp), work).replace(os.sep, "/") + ".tik"
        with open(rp) as f:
            r = json.load(f)
        surfaces = list(r.get("surfaces", {}).values())
        changed = [s for s in surfaces if s["result"] != "original"]
        if not changed:
            reasons = sorted({s.get("reason", "") for s in surfaces})
            unchanged.append({"model": model, "reason": "; ".join(x for x in reasons if x)})
            continue
        for s in changed:
            skd_users.setdefault(s["skd"], []).append(model)
        sheet = os.path.join(os.path.dirname(rp), "review", "before_after.png")
        key = key_of(model)
        im = Image.open(sheet).convert("RGB")
        im.resize((im.width * 3 // 4, im.height * 3 // 4), Image.LANCZOS).save(
            os.path.join(out, "sheets", key + ".jpg"), quality=82, optimize=True)
        entries.append({
            "key": key,
            "model": model,
            "tris_before": sum(s["original_tris"] for s in surfaces),
            "tris_after": sum(s.get("tris", s["original_tris"]) if s["result"] != "original"
                              else s["original_tris"] for s in surfaces),
            "deviation": max(max(s.get("missing_p95", 0), s.get("extra_p95", 0)) for s in changed),
            "surfaces": [{"name": n.split(" (")[0], "result": s["result"], "reason": s.get("reason", "")}
                         for n, s in r["surfaces"].items()],
            "skds": sorted({s["skd"] for s in changed}),
        })
    for e in entries:
        e["shares_with"] = sorted({m for skd in e["skds"] for m in skd_users[skd]} - {e["model"]})
    with open(os.path.join(out, "models.json"), "w") as f:
        json.dump({"models": entries, "unchanged": unchanged}, f, separators=(",", ":"))
    print("%d changed, %d unchanged" % (len(entries), len(unchanged)))


if __name__ == "__main__":
    main()
