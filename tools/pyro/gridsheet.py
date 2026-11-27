#!/usr/bin/env python3
"""A side view of a sim's grids over time (grids.bin): the smoke's thickness through
the box (dark), the flame on top (orange), every Nth stored frame. For judging a sim's
shape without the game: gridsheet.py WORKDIR OUT.png [every]"""
import json, os, sys
import numpy as np
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
every = int(sys.argv[3]) if len(sys.argv) > 3 else 6
meta = json.load(open(os.path.join(src, "grids.json")))
n, times = meta["size"], meta["frames"]
raw = np.fromfile(os.path.join(src, "grids.bin"), dtype=np.float16).astype(np.float32).reshape(len(times), 2, n, n, n)
dmax = np.percentile(raw[:, 0][raw[:, 0] > 1e-3], 99.5)
tiles = []
for i in range(0, len(times), every):
    d, f = raw[i]
    thick = 1 - np.exp(-d.sum(axis=1) / dmax * 0.6)   # z, x: through y
    fl = f.max(axis=1)
    img = np.ones((n, n, 3)) * np.array([0.75, 0.8, 0.88])
    img = img * (1 - thick[..., None]) + np.array([0.08, 0.08, 0.08]) * thick[..., None]
    img = img * (1 - fl[..., None]) + np.array([1.0, 0.5, 0.1]) * fl[..., None]
    img = (np.clip(img[::-1], 0, 1) * 255).astype(np.uint8)
    t = Image.fromarray(img).resize((160, 160), Image.BILINEAR)
    tiles.append(t)
S = Image.new("RGB", (160 * min(len(tiles), 8), 160 * ((len(tiles) + 7) // 8)), (0, 0, 0))
for k, t in enumerate(tiles):
    S.paste(t, ((k % 8) * 160, (k // 8) * 160))
S.save(out)
print(out, "frames", [round(times[i], 1) for i in range(0, len(times), every)])
