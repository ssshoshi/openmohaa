"""Posing a skeleton the way skeletor does, in plain Python.

Skeleton.evaluate ports the bone classes of code/skeletor/skeletorbones.cpp, IK legs,
hose and averaging helpers included, in the engine's own row-vector convention: a matrix
is four rows (X, Y, Z axes, then the origin), p' = p * M, child = local * parent, and a
bone's local rotation is QuatToMat(q) of the file's (x, y, z, w) quaternion.

Everything handed out is in the column-vector convention (p' = M p) Blender uses:
4x4 row lists. Converted, a local rotation is the CONJUGATE of the file's quaternion and
child = parent @ local.
"""

import math

from . import skd

IDENTITY_Q = (0.0, 0.0, 0.0, 1.0)


# --------------------------------------------------------------------------- engine maths

def _quat_to_rows(q):
    """QuatToMat (q_math.c): the 3x3 rows of an engine bone matrix."""
    x, y, z, w = q
    x2, y2, z2 = x + x, y + y, z + z
    xx, xy, xz = x * x2, x * y2, x * z2
    yy, yz, zz = y * y2, y * z2, z * z2
    wx, wy, wz = w * x2, w * y2, w * z2
    return [[1.0 - (yy + zz), xy - wz, xz + wy],
            [xy + wz, 1.0 - (xx + zz), yz - wx],
            [xz - wy, yz + wx, 1.0 - (xx + yy)]]


def _rows_to_quat(m):
    """MatToQuat (q_math.c)."""
    trace = m[0][0] + m[1][1] + m[2][2]
    q = [0.0, 0.0, 0.0, 0.0]
    if trace > 0.0:
        s = math.sqrt(trace + 1.0)
        q[3] = s * 0.5
        s = 0.5 / s
        q[0] = (m[2][1] - m[1][2]) * s
        q[1] = (m[0][2] - m[2][0]) * s
        q[2] = (m[1][0] - m[0][1]) * s
    else:
        i = 0
        if m[1][1] > m[0][0]:
            i = 1
        if m[2][2] > m[i][i]:
            i = 2
        j = (1, 2, 0)[i]
        k = (1, 2, 0)[j]
        s = math.sqrt((m[i][i] - (m[j][j] + m[k][k])) + 1.0)
        q[i] = s * 0.5
        s = 0.5 / s
        q[3] = (m[k][j] - m[j][k]) * s
        q[j] = (m[j][i] + m[i][j]) * s
        q[k] = (m[k][i] + m[i][k]) * s
    return tuple(q)


def _slerp(a, b, t):
    """Slerp (SkelQuat.h): really a normalised lerp along the shorter way."""
    d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]
    f = math.copysign(1.0 - t, d)
    o = [b[i] * t + a[i] * f for i in range(4)]
    n = math.sqrt(sum(c * c for c in o)) or 1.0
    return tuple(c / n for c in o)


def _mat(rot, origin):
    return [list(rot[0]), list(rot[1]), list(rot[2]), list(origin)]


_ID = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0], [0.0, 0.0, 0.0]]


def _copy(m):
    return [list(r) for r in m]


def _multiply(m1, m2):
    """SkelMat4::Multiply: m1 * m2 for row-vector matrices."""
    out = [[0.0] * 3 for _ in range(4)]
    for i in range(4):
        for j in range(3):
            out[i][j] = m1[i][0] * m2[0][j] + m1[i][1] * m2[1][j] + m1[i][2] * m2[2][j]
        if i == 3:
            for j in range(3):
                out[3][j] += m2[3][j]
    return out


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def _normalize(v):
    n = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    if n:
        v[0] /= n
        v[1] /= n
        v[2] /= n
    return n


def _to_column(m):
    """An engine row-vector matrix as a 4x4 column-vector matrix."""
    return [[m[0][0], m[1][0], m[2][0], m[3][0]],
            [m[0][1], m[1][1], m[2][1], m[3][1]],
            [m[0][2], m[1][2], m[2][2], m[3][2]],
            [0.0, 0.0, 0.0, 1.0]]


# --------------------------------------------------------------------------- column helpers

def quat_to_matrix(q):
    """The column-convention 3x3 rotation of an engine quaternion: QuatToMat(q) transposed."""
    r = _quat_to_rows(q)
    return [[r[0][0], r[1][0], r[2][0]], [r[0][1], r[1][1], r[2][1]], [r[0][2], r[1][2], r[2][2]]]


def matrix_to_quat(r):
    """The engine quaternion of a column-convention 3x3 rotation (w kept positive)."""
    q = _rows_to_quat([[r[0][0], r[1][0], r[2][0]], [r[0][1], r[1][1], r[2][1]], [r[0][2], r[1][2], r[2][2]]])
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    q = tuple(c / n for c in q)
    return q if q[3] >= 0.0 else tuple(-c for c in q)


def apply(m, p):
    return (m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3],
            m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3],
            m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3])


def rotate(m, v):
    return (m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2])


IDENTITY = _to_column(_ID)


# --------------------------------------------------------------------------- skeleton

def _floats(raw):
    import struct
    return struct.unpack("<%df" % (len(raw) // 4), raw[:len(raw) // 4 * 4])


class Skeleton:
    """Bones merged from one or more models (by name, as the engine merges a TIKI's
    skelmodels), posed like skeletor poses them."""

    def __init__(self, bones):
        self.bones = []
        self.by_name = {}
        for b in bones:
            key = b.name.lower()
            if key not in self.by_name:
                self.by_name[key] = b
                self.bones.append(b)
        self.params = {}
        for b in self.bones:
            self.params[b.name.lower()] = self._params(b)
        # IK: a shoulder learns its wrist and both lengths from the bones that name it
        for b in self.bones:
            p = self.params[b.name.lower()]
            if b.type in (skd.IKELBOW, skd.IKWRIST) and b.refs:
                sp = self.params.get(b.refs[0].lower())
                if sp is not None:
                    if b.type == skd.IKELBOW:
                        sp["upper"] = p["length"]
                    else:
                        sp["lower"] = p["length"]
                        sp["wrist"] = b

    @staticmethod
    def _params(b):
        f = _floats(b.base_data) if b.base_data else ()
        p = {"type": b.type, "offset": list(b.offset)}
        if b.type in (skd.IKELBOW, skd.IKWRIST):
            v = f[:3] if len(f) >= 3 else b.offset
            p["length"] = math.sqrt(sum(c * c for c in v))
        elif b.type == skd.AVROT:
            # skelBone_AvRot blends by boneData->weight, which LoadBoneFromBuffer2 never sets
            # (it reads the first float as "length"); the first float is the 0.5 meant here.
            p["weight"] = f[0] if f else 0.5
        elif b.type == skd.HOSEROT:
            p["bend_ratio"], p["bend_max"], p["spin_ratio"] = (f[:3] if len(f) >= 3 else (1.0, 180.0, 1.0))
            hose_type = 0
            if len(b.base_data) >= 40:
                hose_type = int.from_bytes(b.base_data[36:40], "little", signed=True)
            p["hose_type"] = hose_type
            if hose_type in (1, 2):  # HOSEROTPARENT, HOSEROTBOTH
                p["offset"][0] = -p["offset"][0]
                p["offset"][2] = -p["offset"][2]
        return p

    def evaluate(self, channels):
        """{bone name (lower): 4x4 column-convention model-space matrix} for one frame.
        channels maps a channel name (lower) to its value; a channel the frame lacks
        reads as an identity rotation or a zero position, as in skeletor."""
        self._channels = channels
        self._cache = {}
        self._ik = {}
        out = {}
        for b in self.bones:
            out[b.name.lower()] = _to_column(self._world(b))
        return out

    # ---- per bone type, following skeletorbones.cpp

    def _quat(self, name):
        q = self._channels.get(name.lower())
        return tuple(q[:4]) if q is not None else IDENTITY_Q

    def _pos(self, name):
        v = self._channels.get(name.lower())
        return list(v[:3]) if v is not None else [0.0, 0.0, 0.0]

    def _parent(self, b):
        if not b.parent:
            return None
        return self.by_name.get(b.parent.lower())

    def _parent_world(self, b):
        """A copy: the IK and hose bones change it in place."""
        p = self._parent(b)
        return _copy(self._world(p)) if p is not None else _copy(_ID)

    def _world(self, b):
        key = b.name.lower()
        if key in self._cache:
            return self._cache[key]
        self._cache[key] = _copy(_ID)  # guards against reference loops
        p = self.params[key]
        t = b.type
        if t == skd.ROTATION:
            local = _mat(_quat_to_rows(self._quat(skd.rotation_channel(b.name))), p["offset"])
            m = _multiply(local, self._parent_world(b)) if self._parent(b) else local
        elif t == skd.POSROT:
            local = _mat(_quat_to_rows(self._quat(skd.rotation_channel(b.name))),
                         self._pos(skd.position_channel(b.name)))
            m = _multiply(local, self._parent_world(b)) if self._parent(b) else local
        elif t == skd.IKSHOULDER:
            m = self._ik_shoulder(b, p)
        elif t == skd.IKELBOW:
            m = self._ik_elbow(b)
        elif t == skd.IKWRIST:
            m = self._ik_wrist(b)
        elif t == skd.AVROT:
            m = self._avrot(b, p)
        elif t == skd.HOSEROT:
            m = self._hoserot(b, p)
        else:  # ZERO and anything unknown follow the parent
            m = self._parent_world(b)
        self._cache[key] = m
        return m

    def _ik_shoulder(self, b, p):
        wrist = p.get("wrist")
        if wrist is None:
            return _copy(_ID)
        upper = p.get("upper", 0.0)
        lower = p.get("lower", 0.0)
        base = self._parent_world(b)
        bv = p["offset"]
        for j in range(3):
            base[3][j] += bv[0] * base[0][j] + bv[1] * base[1][j] + bv[2] * base[2][j]
        base[0] = [-c for c in base[0]]  # InvertAxis(0)
        base[2] = [-c for c in base[2]]  # InvertAxis(2)

        chans = wrist.channels or [skd.rotation_channel(wrist.name), skd.position_channel(wrist.name)]
        wrist_angle = self._quat(chans[0])
        target = _mat(_quat_to_rows(wrist_angle), [0.0, 0.0, 0.0])
        wrist_pos = self._pos(chans[1] if len(chans) > 1 else skd.position_channel(wrist.name))
        target[3] = list(wrist_pos)

        m = _copy(base)
        m[0] = [target[3][j] - base[3][j] for j in range(3)]
        dist = _normalize(m[0])
        m[1] = _cross(base[2], m[0])
        c2 = _cross(target[2], m[0])
        m[1] = [m[1][j] + c2[j] for j in range(3)]
        if not _normalize(m[1]):
            m[1][1] = 1.0
        m[2] = _cross(m[0], m[1])

        reach = upper + lower - 0.001
        if dist > reach:
            dist = reach
            wrist_pos = [m[3][j] + m[0][j] * reach for j in range(3)]
        if dist and upper:
            cos_upper = (dist * dist + upper * upper - lower * lower) / (dist * upper + dist * upper)
        else:
            cos_upper = 1.0
        cos_upper = min(cos_upper, 1.0)
        if upper and lower:
            cos_elbow = -((lower * lower + upper * upper - dist * dist) / (upper * lower + upper * lower))
        else:
            cos_elbow = -1.0
        sin_upper = -math.sqrt(max(0.0, 1.0 - cos_upper * cos_upper))
        for j in range(3):
            ca, sa = m[0][j], m[1][j]
            m[0][j] = ca * cos_upper - sa * sin_upper
            m[1][j] = sa * cos_upper + ca * sin_upper
        self._ik[b.name.lower()] = (wrist_angle, wrist_pos, cos_elbow, upper)
        return m

    def _shoulder_of(self, b):
        sb = self.by_name.get(b.refs[0].lower()) if b.refs else None
        if sb is None:
            return None, None
        m = self._world(sb)
        return m, self._ik.get(sb.name.lower())

    def _ik_elbow(self, b):
        sm, ik = self._shoulder_of(b)
        if ik is None:
            return self._parent_world(b)
        _, _, cos_elbow, upper = ik
        m = _copy(sm)
        for j in range(3):
            m[3][j] += m[0][j] * upper
        s = math.sqrt(max(0.0, 1.0 - cos_elbow * cos_elbow))
        for j in range(3):
            ca, sa = m[0][j], m[1][j]
            m[0][j] = ca * cos_elbow - sa * s
            m[1][j] = sa * cos_elbow + ca * s
        return m

    def _ik_wrist(self, b):
        _, ik = self._shoulder_of(b)
        if ik is None:
            return self._parent_world(b)
        wrist_angle, wrist_pos, _, _ = ik
        return _mat(_quat_to_rows(wrist_angle), wrist_pos)

    def _avrot(self, b, p):
        r1 = self.by_name.get(b.refs[0].lower()) if len(b.refs) > 0 else None
        r2 = self.by_name.get(b.refs[1].lower()) if len(b.refs) > 1 else None
        if r1 is None or r2 is None:
            return self._parent_world(b)
        q = _slerp(_rows_to_quat(self._world(r1)[:3]), _rows_to_quat(self._world(r2)[:3]), p["weight"])
        m = _mat(_ID[:3], p["offset"])
        if self._parent(b):
            m = _multiply(m, self._parent_world(b))
        rows = _quat_to_rows(q)
        return [rows[0], rows[1], rows[2], m[3]]

    def _hoserot(self, b, p):
        target_bone = self.by_name.get(b.refs[0].lower()) if b.refs else None
        parent_tm = self._parent_world(b)
        target_tm = _copy(self._world(target_bone)) if target_bone is not None else _copy(_ID)
        if p["hose_type"] == 2:  # both
            target_tm[0] = [-c for c in target_tm[0]]
            target_tm[2] = [-c for c in target_tm[2]]
        if p["hose_type"] in (1, 2):
            parent_tm[0] = [-c for c in parent_tm[0]]
            parent_tm[2] = [-c for c in parent_tm[2]]

        aim, target_aim = parent_tm[0], target_tm[0]
        axis = _cross(target_aim, aim)
        s = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]
        if s == 0.0:
            axis[0] = 1.0
        elif s != 1.0:
            ln = 1.0 / math.sqrt(s)
            axis = [c * ln for c in axis]
        d = aim[0] * target_aim[0] + aim[1] * target_aim[1] + aim[2] * target_aim[2]
        if d < 1.0:
            angle = math.acos(d) if d > -0.999 else 6.2831855
        else:
            angle = 0.0
        v = min(angle * p["bend_ratio"], p["bend_max"])

        # inverse of the parent's rotation (and origin)
        temp = [[parent_tm[0][0], parent_tm[1][0], parent_tm[2][0]],
                [parent_tm[0][1], parent_tm[1][1], parent_tm[2][1]],
                [parent_tm[0][2], parent_tm[1][2], parent_tm[2][2]],
                [-(parent_tm[i][0] * parent_tm[3][0] + parent_tm[i][1] * parent_tm[3][1]
                   + parent_tm[i][2] * parent_tm[3][2]) for i in range(3)]]
        axis = [axis[0] * temp[0][j] + axis[1] * temp[1][j] + axis[2] * temp[2][j] for j in range(3)]
        c = math.cos(v * 0.5)
        ln = math.sqrt(max(0.0, 1.0 - c * c))
        q = (axis[0] * ln, axis[1] * ln, axis[2] * ln, c)
        if p["spin_ratio"] < 1.0:
            tq = _rows_to_quat(_multiply(target_tm, temp)[:3])
            q = _slerp(tq, q, p["spin_ratio"])
        local = _mat(_quat_to_rows(q), p["offset"])
        return _multiply(local, parent_tm)


def frame_channels(anim, frame):
    """The channels of one frame of an skc.Animation, as Skeleton.evaluate wants them."""
    if anim is None or not anim.values:
        return {}
    row = anim.values[min(frame, len(anim.values) - 1)]
    return {name.lower(): value for name, value in zip(anim.channels, row)}


def skin_vertex(vertex, bone_names, world):
    """The position of a vertex for posed bones; bone_names maps the weights' indices."""
    x = y = z = 0.0
    for bi, bw, off in vertex.weights:
        m = world.get(bone_names[bi].lower(), IDENTITY) if 0 <= bi < len(bone_names) else IDENTITY
        p = apply(m, off)
        x += p[0] * bw
        y += p[1] * bw
        z += p[2] * bw
    return (x, y, z)


# --------------------------------------------------------------------------- bind pose

def _kabsch(src, dst):
    """The rigid 4x4 T (numpy) with T @ src ~ dst, its worst residual and its singular values."""
    import numpy as np
    P = np.asarray(src, dtype=np.float64)
    Q = np.asarray(dst, dtype=np.float64)
    pc, qc = P.mean(axis=0), Q.mean(axis=0)
    H = (P - pc).T @ (Q - qc)
    U, S, Vt = np.linalg.svd(H)
    d = 1.0 if np.linalg.det(Vt.T @ U.T) >= 0 else -1.0
    R = Vt.T @ np.diag([1.0, 1.0, d]) @ U.T
    t = qc - R @ pc
    T = np.eye(4)
    T[:3, :3] = R
    T[:3, 3] = t
    res = float(np.abs(P @ R.T + t - Q).max())
    return T, res, S


def bind_pose(skeleton, models, reference, tolerance=0.05):
    """The pose the meshes were skinned in, as {bone (lower): 4x4 column matrix}.

    A MOHAA vertex keeps one offset per bone that moves it. In the pose the artist bound
    the mesh in, all of a vertex's offsets land on the same point; in any other pose
    (an animation's first frame) they drift apart. Blender's armature deform blends one
    rest position, so it only matches the engine from that bind pose. For every pair of
    bones sharing vertices, the rigid fit between their two offset sets is their relative
    bind transform; chaining the fits along the best-shared pairs rebuilds the pose. Bones
    no shared vertex ties down keep their place relative to a neighbour from `reference`
    (the engine pose used otherwise). Returns (pose, worst fit residual)."""
    import numpy as np

    pairs = {}
    for m in models:
        names = [b.name.lower() for b in m.bones]
        for s in m.surfaces:
            for v in s.verts:
                ws = [(names[bi], off) for bi, _, off in v.weights if 0 <= bi < len(names)]
                for i in range(len(ws)):
                    for j in range(i + 1, len(ws)):
                        (a, oa), (b, ob) = ws[i], ws[j]
                        if a == b:
                            continue
                        if a > b:
                            (a, oa), (b, ob) = (b, ob), (a, oa)
                        pairs.setdefault((a, b), []).append((oa, ob))
    # rel[(a, b)]: b's bind matrix in a's space (B_b = B_a @ T)
    rel = {}
    worst = 0.0
    for (a, b), pts in pairs.items():
        if len(pts) < 3:
            continue
        T, res, S = _kabsch([p[1] for p in pts], [p[0] for p in pts])
        if res > tolerance or S[1] < 1e-6 * max(S[0], 1e-12):
            continue  # not rigid (another skelmodel bound elsewhere) or too few distinct points
        worst = max(worst, res)
        rel[(a, b)] = (len(pts), T)
        rel[(b, a)] = (len(pts), np.linalg.inv(T))
    neighbours = {}
    for (a, b), (n, T) in rel.items():
        neighbours.setdefault(a, []).append((n, b, T))

    ref = {k: np.array(v, dtype=np.float64) for k, v in reference.items()}
    out = {}

    def grow(start):
        # Prim's maximum spanning tree from start over the shared-vertex pairs
        import heapq
        heap = [(-n, b, start, T) for n, b, T in neighbours.get(start, ())]
        heapq.heapify(heap)
        while heap:
            _, b, a, T = heapq.heappop(heap)
            if b in out:
                continue
            out[b] = out[a] @ T
            for n, c, T2 in neighbours.get(b, ()):
                if c not in out:
                    heapq.heappush(heap, (-n, c, b, T2))

    depth = {}
    for b in skeleton.bones:
        d, p = 0, b
        while p is not None and p.parent:
            p = skeleton.by_name.get(p.parent.lower())
            d += 1
        depth[b.name.lower()] = d
    constrained = sorted(neighbours, key=lambda k: (-sum(n for n, _, _ in neighbours[k]), depth.get(k, 0)))
    if constrained:
        # anchor the best-tied component at its shallowest bone's reference place
        comp = {constrained[0]}
        stack = [constrained[0]]
        while stack:
            for _, c, _ in neighbours.get(stack.pop(), ()):
                if c not in comp:
                    comp.add(c)
                    stack.append(c)
        anchor = min(comp, key=lambda k: depth.get(k, 0))
        out[anchor] = ref[anchor]
        grow(anchor)

    # everything else: from a placed parent or child, keeping the reference relation
    remaining = [b.name.lower() for b in skeleton.bones if b.name.lower() not in out]
    while remaining:
        progress = False
        for key in list(remaining):
            b = skeleton.by_name[key]
            parent = b.parent.lower() if b.parent else None
            src = None
            if parent in out:
                src = parent
            else:
                for c in skeleton.bones:
                    if c.parent and c.parent.lower() == key and c.name.lower() in out:
                        src = c.name.lower()
                        break
            if src is None and parent is None and not out:
                out[key] = ref[key]
            elif src is not None:
                out[key] = out[src] @ np.linalg.inv(ref[src]) @ ref[key]
            else:
                continue
            remaining.remove(key)
            progress = True
            if key in neighbours:
                grow(key)  # a component no earlier bone reached: hang it here
        if not progress:
            for key in remaining:  # disconnected from everything placed
                out[key] = ref[key]
                if key in neighbours:
                    grow(key)
            remaining = [k for k in remaining if k not in out]
    return {k: [list(r) for r in v] for k, v in out.items()}, worst


class Binding:
    """How a model's meshes sit on an armature whose rest pose is `rest`.

    positions[(model index, surface index)] are the rest positions, weights[...] per vertex
    lists of (deforming bone name, weight). A vertex rests where its heaviest bone puts it.
    Its other bones must put it there too for Blender's deform to match the engine; where
    the rest pose does not (MOHAA meshes carry a few such seams), the vertex is weighted to
    a proxy: a bone with the needed rest matrix that copies the real bone's motion.
    proxies: {proxy name: (real bone name, 4x4 rest matrix)}."""

    def __init__(self):
        self.positions = {}
        self.weights = {}
        self.proxies = {}
        self.worst = 0.0


def bind(skeleton, models, rest, tolerance=0.01):
    import numpy as np

    R = {k: np.array(v, dtype=np.float64) for k, v in rest.items()}
    out = Binding()
    groups = {}  # (model, other bone, primary bone) -> [(surface, vertex, weight index)]
    for mi, m in enumerate(models):
        names = [b.name.lower() for b in m.bones]
        for si, s in enumerate(m.surfaces):
            pos, wts = [], []
            for vi, v in enumerate(s.verts):
                ws = [(names[bi], bw, off) for bi, bw, off in v.weights if 0 <= bi < len(names)]
                if not ws:
                    pos.append((0.0, 0.0, 0.0))
                    wts.append([])
                    continue
                primary = max(range(len(ws)), key=lambda i: ws[i][1])
                pb = ws[primary][0]
                p = R[pb] @ np.array(list(ws[primary][2]) + [1.0])
                pos.append(tuple(p[:3]))
                row = []
                for i, (bn, bw, off) in enumerate(ws):
                    row.append([skeleton.by_name[bn].name, bw])
                    if i == primary or bn == pb:
                        continue
                    q = R[bn] @ np.array(list(off) + [1.0])
                    if np.abs(q[:3] - p[:3]).max() > tolerance:
                        groups.setdefault((mi, bn, pb), []).append((si, vi, len(row) - 1, off, ws[primary][2]))
                wts.append(row)
            out.positions[(mi, si)] = pos
            out.weights[(mi, si)] = wts

    counts = {}

    def proxy_for(bone, matrix):
        for name, (real, m) in out.proxies.items():
            if real == bone and np.abs(np.array(m) - matrix).max() < 1e-4:
                return name
        counts[bone] = counts.get(bone, 0) + 1
        name = "%s~%d" % (skeleton.by_name[bone].name, counts[bone])
        out.proxies[name] = (bone, [list(r) for r in matrix])
        return name

    for (mi, bn, pb), items in groups.items():
        src = [it[3] for it in items]
        dst = [it[4] for it in items]
        fits = []
        if len(items) >= 3:
            T, res, S = _kabsch(src, dst)
            if res <= tolerance and S[1] > 1e-6 * max(S[0], 1e-12):
                fits.append((T, items))
        if not fits:
            # too few points to fit a rotation, or not rigid: keep the bone's rotation and
            # move the rest to each vertex
            for it in items:
                T = np.linalg.inv(R[bn]) @ R[pb]
                Rb = R[bn][:3, :3]
                t = (R[pb] @ np.array(list(it[4]) + [1.0]))[:3] - Rb @ np.array(it[3])
                M = np.eye(4)
                M[:3, :3] = Rb
                M[:3, 3] = t
                fits.append((np.linalg.inv(R[pb]) @ M, [it]))
        for T, its in fits:
            name = proxy_for(bn, R[pb] @ T)
            for si, vi, wi, _, _ in its:
                out.weights[(mi, si)][vi][wi][0] = name
    return out
