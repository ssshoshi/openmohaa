#!/usr/bin/env python3
"""How much fire a sim has over time, from its grids.bin: per stored frame, the cells
with flame above 0.25 and the flame's peak. pyro.py firestats NAME [--preview]."""
import json, os, sys
import numpy as np
src = sys.argv[1]
meta = json.load(open(os.path.join(src, "grids.json")))
n, times = meta["size"], meta["frames"]
raw = np.fromfile(os.path.join(src, "grids.bin"), dtype=np.float16).astype(np.float32).reshape(len(times), 2, n, n, n)
line = []
for t, (d, f) in zip(times, raw):
    line.append("%.2fs:%d/%.2f" % (t, int((f > 0.25).sum() * (64 / n) ** 3), f.max()))
print("  ".join(line))
fire = [t for t, (d, f) in zip(times, raw) if (f > 0.25).sum() * (64 / n) ** 3 > 200]
print("visible fire for %.2f s" % ((fire[-1] - fire[0]) if fire else 0))
