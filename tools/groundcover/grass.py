#!/usr/bin/env python3
"""Draw the ground cover's textures (data/opm-groundcover).

    tools/groundcover/grass.py              # writes the textures into data/
    tools/groundcover/grass.py --preview    # and previews on grey beside them

groundcover_cover.tga, 1024x512 RGBA: eight square cells of 256, each one
tuft quad (code/renderergl2/tr_groundcover.c, GC_KIND_*), in this order,
left to right, top row first:

    0 grass        1 grass (another clump)   2 weeds (plantain, dock)   3 grass in flower
    4 dead grass   5 dry scrub               6 reeds                    7 dry grass

groundcover_grass.tga, 512x256: the first two cells alone, the texture of
renderers from before the kinds (groundcover/grass).

Each is drawn from its foot at the bottom of the cell up, with a margin to
the cell's sides and top so mip levels do not bleed into the next. The
colours are albedo: the renderer multiplies them by the ground's light.
Pixels the alpha test drops take the drawing's colour, so filtering does not
draw a dark rim. Seeded: the same files every run.
"""

import argparse
import math
import os
import random

import numpy as np
from PIL import Image

CELL = 256
SS = 3  # supersampling
DATA = os.path.join(os.path.dirname(__file__), "..", "..", "data", "opm-groundcover", "textures", "opm")

GRASS = [  # blade colours, albedo
    (92, 112, 52), (104, 122, 58), (82, 104, 46), (118, 128, 64),
    (98, 116, 50), (128, 126, 70), (140, 132, 78),  # the last two dry
]
DEAD = [(168, 150, 104), (150, 132, 92), (182, 166, 120), (132, 116, 84), (158, 146, 112)]
DRY = [(160, 146, 86), (148, 136, 78), (172, 156, 98), (136, 128, 74), (124, 122, 70)]
REED = [(84, 104, 56), (96, 112, 60), (106, 118, 66), (120, 120, 70)]


class Canvas:
    def __init__(self):
        n = CELL * SS
        self.w = self.h = n
        self.rgb = np.zeros((n, n, 3), np.float32)
        self.alpha = np.zeros((n, n), np.float32)

    def dot(self, x, y, r, c):
        """A filled disc."""
        xa, xb = int(math.floor(x - r)), int(math.ceil(x + r))
        ya, yb = int(math.floor(y - r)), int(math.ceil(y + r))
        for yi in range(max(ya, 0), min(yb + 1, self.h)):
            for xi in range(max(xa, 0), min(xb + 1, self.w)):
                if (xi - x) ** 2 + (yi - y) ** 2 <= r * r:
                    self.rgb[yi, xi] = c
                    self.alpha[yi, xi] = 1.0

    def stroke(self, pts, w0, w1, color, rng, dark=0.55):
        """A tapering line along pts (from its foot), darker at the foot."""
        n = len(pts)
        for i in range(n - 1):
            (xa, ya), (xb, yb) = pts[i], pts[i + 1]
            steps = int(max(abs(xb - xa), abs(yb - ya), 1.0)) + 1
            for k in range(steps):
                u = k / steps
                t = (i + u) / (n - 1)
                x, y = xa + (xb - xa) * u, ya + (yb - ya) * u
                half = (w0 + (w1 - w0) * t) * 0.5 + 0.6 * SS
                shade = dark + (1 - dark) * t ** 0.7
                c = np.array(color) * shade * rng.uniform(0.96, 1.04)
                yi = int(y)
                if yi < 0 or yi >= self.h:
                    continue
                for xi in range(max(int(x - half), 0), min(int(x + half) + 1, self.w)):
                    mid = 1.0 - abs(xi - x) / max(half, 1e-3)
                    self.rgb[yi, xi] = c * (0.94 + 0.08 * max(mid, 0))
                    self.alpha[yi, xi] = 1.0


def blade_pts(x0, height, bend, h, droop=0.0):
    """A blade from (x0, the bottom) up to height, its tip bent sideways by bend;
    droop folds the top of it over and down (dead and dry grass)."""
    steps = int(height / SS) + 4
    pts = []
    for i in range(steps + 1):
        t = i / steps
        x = x0 + bend * t * t
        y = h - 1 - t * height
        if droop and t > 0.6:  # past its middle it falls over to the side it leans
            f = (t - 0.6) / 0.4
            y += droop * height * f * f
            x += math.copysign(droop * height * 0.6 * f, bend if bend else 1.0)
        pts.append((x, y))
    return pts


def clump(seed, palette=GRASS, blades=80, droop=0.0, tall=0.97):
    """A clump of blades from a common base, the tall ones in the middle."""
    rng = random.Random(seed)
    c = Canvas()
    w, h = c.w, c.h
    out = []
    for _ in range(blades):
        x0 = w * 0.5 + rng.gauss(0, w * 0.16)
        x0 = min(max(x0, w * 0.08), w * 0.92)
        centre = 1.0 - abs(x0 - w * 0.5) / (w * 0.5)
        height = h * rng.uniform(0.45, tall) * (0.65 + 0.35 * centre)
        lean = (x0 - w * 0.5) / (w * 0.5)
        bend = (lean * rng.uniform(0.3, 0.9) + rng.uniform(-0.3, 0.3)) * height * 0.45
        # the tip stays on the quad
        bend = min(max(bend, w * 0.04 - x0), w * 0.96 - x0)
        base_w = rng.uniform(2.5, 5.5) * SS
        color = rng.choice(palette)
        d = droop * rng.uniform(0.2, 1.0) if droop and rng.random() < 0.55 else 0.0
        out.append((height, x0, base_w, bend, color, d))
    # the short ones in front
    for height, x0, base_w, bend, color, d in sorted(out, key=lambda b: -b[0]):
        pts = blade_pts(x0, height, bend, h, droop=d)
        pts = [(min(max(x, w * 0.03), w * 0.97), max(y, h * 0.02)) for x, y in pts]
        c.stroke(pts, base_w, 0.0, color, rng)
    return c


def weeds(seed):
    """Broadleaf weeds as stand in a field's grass: plantain and dock leaves
    from one crown, a few seed stalks, and some grass round them."""
    rng = random.Random(seed)
    c = clump(seed + 100, blades=26, tall=0.6)
    w, h = c.w, c.h
    for _ in range(4):  # seed stalks, behind the leaves
        x0 = w * 0.5 + rng.gauss(0, w * 0.06)
        height = h * rng.uniform(0.6, 0.9)
        pts = blade_pts(x0, height, rng.uniform(-0.12, 0.12) * height, h)
        c.stroke(pts, 1.6 * SS, 1.0 * SS, (96, 104, 60), rng, dark=0.7)
        n = len(pts)
        for i in range(int(n * 0.72), n):  # the seed head along its top
            px, py = pts[i]
            c.dot(px + rng.uniform(-1, 1) * SS, py, 1.8 * SS, np.array((104, 98, 62)) * rng.uniform(0.9, 1.1))
    leaves = []
    for _ in range(16):
        x0 = w * 0.5 + rng.gauss(0, w * 0.07)
        ang = max(-1.3, min(1.3, rng.gauss(0, 0.75)))  # from upright
        length = h * rng.uniform(0.28, 0.55)
        wide = length * rng.uniform(0.22, 0.32)
        leaves.append((length, x0, ang, wide, rng.choice([(70, 104, 44), (80, 112, 50), (64, 96, 40), (88, 116, 54)])))
    for length, x0, ang, wide, color in sorted(leaves, key=lambda l: -l[0]):
        dx, dy = math.sin(ang), -math.cos(ang)
        steps = int(length) + 2
        for i in range(steps):
            t = i / (steps - 1)
            # an ellipse's width along it, its tip weighed down a little
            half = wide * 0.5 * math.sin(math.pi * min(1.0, 0.08 + 0.92 * t)) ** 0.8
            x = x0 + dx * length * t
            y = h - 1 + dy * length * t + length * 0.25 * abs(dx) * t * t
            shade = 0.6 + 0.4 * t
            for j in np.arange(-half, half + 0.5, 0.5):
                xi = int(x - j * dy)
                yi = int(y + j * dx)
                if 0 <= xi < w and 0 <= yi < h and yi > h * 0.02:
                    vein = 1.12 if abs(j) < 0.7 * SS else 1.0
                    c.rgb[yi, xi] = np.array(color) * shade * vein
                    c.alpha[yi, xi] = 1.0
    return c


def flowers(seed):
    """Grass in flower: a thinner clump with white and yellow flower heads
    (daisies, buttercups) on stalks above it."""
    rng = random.Random(seed)
    c = clump(seed + 200, blades=50, tall=0.8)
    w, h = c.w, c.h
    for _ in range(16):
        x0 = w * 0.5 + rng.gauss(0, w * 0.17)
        x0 = min(max(x0, w * 0.12), w * 0.88)
        height = h * rng.uniform(0.4, 0.85)
        pts = blade_pts(x0, height, rng.uniform(-0.1, 0.1) * height, h)
        c.stroke(pts, 1.4 * SS, 1.0 * SS, (88, 112, 52), rng, dark=0.7)
        tx, ty = pts[-1]
        if rng.random() < 0.55:  # a daisy: white petals round a yellow eye
            for k in range(10):
                a = k / 10 * 2 * math.pi
                c.dot(tx + math.cos(a) * 5.0 * SS, ty + math.sin(a) * 2.6 * SS, 2.6 * SS, (236, 234, 224))
            c.dot(tx, ty, 2.6 * SS, (222, 180, 48))
        else:  # a buttercup
            c.dot(tx, ty, 4.6 * SS, (232, 200, 40))
            c.dot(tx, ty - 1.2 * SS, 2.4 * SS, (246, 222, 90))
    return c


def scrub(seed):
    """A low dry shrub of the North African hills: twiggy grey-brown branches
    from one foot, with a few small grey-green leaves at their ends."""
    rng = random.Random(seed)
    c = Canvas()
    w, h = c.w, c.h
    twig = (112, 96, 80)

    def branch(x, y, ang, length, width, depth):
        steps = 6
        pts = [(x, y)]
        for _ in range(steps):
            ang += rng.gauss(0, 0.18)
            x = min(max(x + math.sin(ang) * length / steps, w * 0.05), w * 0.95)
            y = max(y - math.cos(ang) * length / steps, h * 0.04)
            pts.append((x, y))
        c.stroke(pts, width, width * 0.5, np.array(twig) * rng.uniform(0.85, 1.1), rng, dark=0.7)
        if depth < 3:
            for _ in range(rng.randint(2, 3)):
                bx, by = pts[rng.randint(2, steps)]
                branch(bx, by, ang + rng.choice((-1, 1)) * rng.uniform(0.35, 0.8), length * rng.uniform(0.45, 0.7),
                       width * 0.6, depth + 1)
        else:
            for _ in range(7):  # leaves at the tip
                lx, ly = pts[rng.randint(3, steps)]
                c.dot(lx + rng.uniform(-5, 5) * SS, ly + rng.uniform(-4, 4) * SS, rng.uniform(2.2, 3.4) * SS,
                      np.array((116, 124, 90)) * rng.uniform(0.85, 1.1))

    for _ in range(9):
        branch(w * 0.5 + rng.gauss(0, w * 0.04), h - 1, rng.gauss(0, 0.55), h * rng.uniform(0.35, 0.55), 4.6 * SS, 0)
    return c


def reeds(seed):
    """Reeds and bulrushes at a water's edge: tall straight blades, a few
    stems with the brown head of a bulrush."""
    rng = random.Random(seed)
    c = Canvas()
    w, h = c.w, c.h
    stems = []
    for _ in range(46):
        x0 = w * 0.5 + rng.gauss(0, w * 0.14)
        x0 = min(max(x0, w * 0.1), w * 0.9)
        height = h * rng.uniform(0.55, 0.97)
        bend = rng.uniform(-0.12, 0.12) * height
        head = rng.random() < 0.18
        droop = 0.12 if (not head and rng.random() < 0.3) else 0.0
        stems.append((height, x0, bend, rng.choice(REED), head, droop))
    for height, x0, bend, color, head, droop in sorted(stems, key=lambda s: -s[0]):
        pts = blade_pts(x0, height, bend, h, droop=droop)
        pts = [(min(max(x, w * 0.03), w * 0.97), max(y, h * 0.02)) for x, y in pts]
        if head:
            c.stroke(pts, 1.8 * SS, 1.2 * SS, color, rng, dark=0.6)
            n = len(pts)
            for i in range(int(n * 0.62), int(n * 0.86)):  # the bulrush's head, below the tip
                px, py = pts[i]
                c.dot(px, py, 2.6 * SS, np.array((94, 64, 42)) * rng.uniform(0.92, 1.05))
        else:
            c.stroke(pts, rng.uniform(3.0, 5.0) * SS, 0.0, color, rng, dark=0.5)
    return c


def finish(c, width):
    """Down from the supersampled size: RGBA, the edges keeping the drawing's colour."""
    h = c.h // SS
    img_a = Image.fromarray((c.alpha * 255).astype(np.uint8)).resize((width, h), Image.LANCZOS)
    prem = np.asarray(Image.fromarray(np.clip(c.rgb * c.alpha[..., None], 0, 255).astype(np.uint8))
                      .resize((width, h), Image.LANCZOS), np.float32)
    a = np.asarray(img_a, np.float32) / 255.0
    colour = prem / np.maximum(a[..., None], 1e-3)
    mean = (prem.reshape(-1, 3).sum(0) / max(a.sum(), 1)).astype(np.float32)
    colour = np.where(a[..., None] > 0.02, colour, mean)
    return np.dstack([np.clip(colour, 0, 255), a * 255]).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--preview", action="store_true")
    args = ap.parse_args()

    cells = [
        clump(1), clump(2), weeds(3), flowers(4),
        clump(5, DEAD, blades=60, droop=0.45, tall=0.8), scrub(6), reeds(7), clump(8, DRY, blades=70, droop=0.2),
    ]
    tiles = [finish(c, CELL) for c in cells]
    atlas = np.concatenate([np.concatenate(tiles[0:4], axis=1), np.concatenate(tiles[4:8], axis=1)], axis=0)
    grass = np.concatenate(tiles[0:2], axis=1)

    os.makedirs(DATA, exist_ok=True)
    for name, arr in (("groundcover_cover.tga", atlas), ("groundcover_grass.tga", grass)):
        path = os.path.normpath(os.path.join(DATA, name))
        im = Image.fromarray(arr, "RGBA")
        im.save(path)
        print(path)
        if args.preview:
            bg = Image.new("RGBA", im.size, (128, 128, 128, 255))
            bg.alpha_composite(im)
            p = os.path.join(os.environ.get("TMPDIR", "/tmp"), os.path.splitext(name)[0] + "_preview.png")
            bg.save(p)
            print(p)


if __name__ == "__main__":
    main()
