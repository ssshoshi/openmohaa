"""Minimal MOHAA BSP reader: solid test (brushes + LOD terrain) and line of sight."""
import struct, re, math
class BSP:
    def __init__(self, d):
        self.d = d
        ident, ver = struct.unpack_from("<4si", d, 0); self.ver = ver
        self.lumps = [struct.unpack_from("<ii", d, 12 + 8 * i) for i in range(28)]
        so, sl = self.L(0); sh = [struct.unpack_from("<64sii", d, so + k * 140) for k in range(sl // 140)]
        self.shaders = [(s.split(b"\0")[0].decode("latin1"), sf, cf) for s, sf, cf in sh]
        self.opaque = [bool(cf & 1) and not any(k in n.lower() for k in ("clip", "trigger", "hint", "origin", "glass", "window", "fence", "grate"))
                       for n, sf, cf in self.shaders]
        self.planes = self.arr(1, "<4f")
        self.nodes = self.arr(9, "<9i")
        leaf = "<16i" if ver > 17 else "<14i"
        self.leafs = self.arr(8, leaf)
        self.leafbrushes = [x[0] for x in self.arr(6, "<i")]
        self.sides = self.arr(11, "<3i")
        self.brushes = self.arr(12, "<3i")
        self.ents = []
        eo, el = self.L(14)
        for b in re.findall(r"\{([^{}]*)\}", d[eo:eo+el].decode("latin1")):
            self.ents.append(dict(re.findall(r'"([^"]+)"\s+"([^"]*)"', b)))
        self.terr = {}
        to, tl = self.L(22)
        for i in range(tl // 388):
            o = to + i * 388
            x, y, base = struct.unpack_from("<bbh", d, o + 36)
            hm = d[o + 4 + 32 + 2 + 2 + 2 + 2 + 8 + 252: o + 4 + 32 + 2 + 2 + 2 + 2 + 8 + 252 + 81]
            self.terr[(x, y)] = (base, hm)
    def L(self, i):
        if self.ver <= 18 and i > 12: i += 1
        return self.lumps[i]
    def arr(self, i, fmt):
        o, l = self.L(i); n = struct.calcsize(fmt)
        return [struct.unpack_from(fmt, self.d, o + k * n) for k in range(l // n)]
    def ground(self, x, y):
        # LOD terrain (cm_terrain.c): a patch's corner is x*64, y*64; 9x9
        # heights 64 apart, base + 2 * byte
        best = None
        px, py = math.floor(x / 64), math.floor(y / 64)
        for (tx, ty), (base, hm) in self._near(px, py):
            i = (x - tx * 64) / 64; j = (y - ty * 64) / 64
            if 0 <= i <= 8 and 0 <= j <= 8:
                ii, jj = min(int(i), 7), min(int(j), 7); fi, fj = i - ii, j - jj
                h = lambda a, b: base + 2 * hm[b * 9 + a]
                z = (h(ii, jj) * (1-fi) * (1-fj) + h(ii+1, jj) * fi * (1-fj) + h(ii, jj+1) * (1-fi) * fj + h(ii+1, jj+1) * fi * fj)
                best = z if best is None else max(best, z)
        return best
    def _near(self, px, py):
        # a patch is 8 cells of 64 a side from its x, y (in 64s)
        for kx in range(px - 8, px + 1):
            for ky in range(py - 8, py + 1):
                t = self.terr.get((kx, ky))
                if t: yield (kx, ky), t
    def leaf(self, p):
        n = 0
        while n >= 0:
            pn, c0, c1 = self.nodes[n][:3]
            a, b, c, dist = self.planes[pn]
            n = c0 if a*p[0] + b*p[1] + c*p[2] - dist >= 0 else c1
        return -1 - n
    def solid(self, p):
        lf = self.leafs[self.leaf(p)]
        if lf[0] == -1: return True
        fb, nb = lf[10], lf[11]
        for k in range(fb, fb + nb):
            br = self.brushes[self.leafbrushes[k]]
            if not self.opaque[br[2]]: continue
            inside = True
            for s in range(br[0], br[0] + br[1]):
                a, b, c, dist = self.planes[self.sides[s][0]]
                if a*p[0] + b*p[1] + c*p[2] - dist > 0: inside = False; break
            if inside: return True
        if self.terr:
            g = self.ground(p[0], p[1])
            if g is not None and p[2] < g: return True
        return False
    def depth(self, p, yaw, pitch=0, far=3000, step=24):
        cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
        cp, sp = math.cos(math.radians(pitch)), -math.sin(math.radians(pitch))
        t = step
        while t < far:
            q = (p[0] + cy*cp*t, p[1] + sy*cp*t, p[2] + sp*t)
            if self.solid(q): return t
            t += step
        return far
