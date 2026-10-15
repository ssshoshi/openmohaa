"""MOHAA skeletons (.skd) and animations (.skc): read, write, and pose them.

The forward kinematics here follow code/skeletor/skeletorbones.cpp step for
step, including the leg IK, so a pose computed here is the pose the game draws.
Matrices are kept in the usual column convention (a bone's world rotation G
maps its local axes into model space); the engine stores the transpose, which
is why a channel quaternion is the conjugate of quat(G_local).

Only the version 13 .skc layout is written: a header, one 48 byte record per
frame, then numChannels x vec4 per frame, then the 32 byte channel names. The
engine converts these on load (tiki_files.cpp, SkeletorCacheFileCallback).
"""
import math
import struct

import numpy as np

ROTATION, POSROT, IKSHOULDER, IKELBOW, IKWRIST, HOSEROT, AVROT, ZERO = range(8)

SKC_IDENT = b"SKAN"
SKC_VERSION = 13


# ---- quaternions (x, y, z, w), matching q_math.c --------------------------

def quat_to_mat(q):
    """q_math.c QuatToMat: the standard rotation matrix of q."""
    x, y, z, w = q
    x2, y2, z2 = x + x, y + y, z + z
    xx, xy, xz = x * x2, x * y2, x * z2
    yy, yz, zz = y * y2, y * z2, z * z2
    wx, wy, wz = w * x2, w * y2, w * z2
    return np.array([
        [1 - (yy + zz), xy - wz, xz + wy],
        [xy + wz, 1 - (xx + zz), yz - wx],
        [xz - wy, yz + wx, 1 - (xx + yy)],
    ])


def mat_to_quat(m):
    """Inverse of quat_to_mat, w kept non-negative."""
    t = m[0, 0] + m[1, 1] + m[2, 2]
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    q = np.array(q)
    q /= np.linalg.norm(q)
    return -q if q[3] < 0 else q


def channel_to_rot(q):
    """A rotation channel as the column-convention rotation it applies."""
    return quat_to_mat(q).T


def rot_to_channel(g):
    return mat_to_quat(g.T)


# ---- skeleton --------------------------------------------------------------

def read_skd(data):
    """Bones in file order (parents first): name, parent, type, offset, length."""
    _, num_bones, ofs_bones = struct.unpack_from("<3i", data, 72)
    bones, o = [], ofs_bones
    for _ in range(num_bones):
        name = data[o:o + 32].split(b"\0")[0].decode()
        parent = data[o + 32:o + 64].split(b"\0")[0].decode()
        btype, ofs_base, _, _, ofs_end = struct.unpack_from("<5i", data, o + 64)
        base = struct.unpack_from("<7f", data, o + ofs_base)
        b = dict(name=name, parent=None if parent == "worldbone" else parent, type=btype,
                 offset=np.zeros(3), length=0.0)
        if btype == ROTATION:
            b["offset"] = np.array(base[0:3])
        elif btype == IKSHOULDER:
            b["offset"] = np.array(base[4:7])
        elif btype in (IKELBOW, IKWRIST):
            b["length"] = float(np.linalg.norm(base[0:3]))
        bones.append(b)
        o += ofs_end
    return bones


def ik_chains(bones):
    """shoulder name -> (elbow name, wrist name), from the parent links."""
    by_parent = {}
    for b in bones:
        by_parent.setdefault(b["parent"], []).append(b)
    chains = {}
    for b in bones:
        if b["type"] != IKSHOULDER:
            continue
        elbow = next(c for c in by_parent[b["name"]] if c["type"] == IKELBOW)
        wrist = next(c for c in by_parent[elbow["name"]] if c["type"] == IKWRIST)
        chains[b["name"]] = (elbow, wrist)
    return chains


def fk(bones, ch):
    """World (rotation, position) per bone for one frame of channels.

    ch maps a channel name to its 4 floats. Helper bones (hose, averaged
    rotations) are skipped: nothing is animated through them.
    """
    chains = ik_chains(bones)
    out, ik = {}, {}

    for b in bones:
        n, t = b["name"], b["type"]
        parent = out.get(b["parent"])

        if t == ROTATION:
            lr = channel_to_rot(ch.get(n + " rot", (0, 0, 0, 1)))
            lp = b["offset"]
        elif t == POSROT:
            lr = channel_to_rot(ch.get(n + " rot", (0, 0, 0, 1)))
            lp = np.array(ch.get(n + " pos", (0, 0, 0, 0))[:3])
        elif t == IKSHOULDER:
            out[n] = ik[n] = _ik_shoulder(b, chains[b["name"]], parent, ch)
            continue
        elif t == IKELBOW:
            out[n] = ik[b["parent"]]["elbow"]
            continue
        elif t == IKWRIST:
            out[n] = ik[bones_parent(bones, b["parent"])]["wrist"]
            continue
        else:
            continue

        if parent is None:
            out[n] = (lr, lp.astype(float))
        else:
            pr, pp = parent
            out[n] = (pr @ lr, pp + pr @ lp)
    return out


def bones_parent(bones, name):
    return next(b["parent"] for b in bones if b["name"] == name)


def _ik_shoulder(b, chain, parent, ch):
    """skelBone_IKshoulder::GetDirtyTransform, with the elbow and wrist."""
    elbow, wrist = chain
    upper, lower = elbow["length"], wrist["length"]
    pr, pp = parent
    # baseMatrix rows are the parent's axes: columns of pr. Axes 0 and 2 flipped.
    base_pos = pp + pr @ b["offset"]
    base = [-pr[:, 0], pr[:, 1], -pr[:, 2]]

    wq = ch[wrist["name"] + " rot"]
    target = quat_to_mat(wq)  # rows = the engine's targetMatrix rows
    wpos = np.array(ch[wrist["name"] + " pos"][:3], dtype=float)

    ax0 = wpos - base_pos
    dist = np.linalg.norm(ax0)
    ax0 /= dist
    ax1 = np.cross(base[2], ax0) + np.cross(target[2], ax0)
    n1 = np.linalg.norm(ax1)
    ax1 = ax1 / n1 if n1 else np.array([0.0, 1.0, 0.0])
    ax2 = np.cross(ax0, ax1)

    reach = upper + lower - 0.001
    if dist > reach:
        dist = reach
        wpos = base_pos + ax0 * reach

    c = (dist * dist + upper * upper - lower * lower) / (2 * dist * upper)
    c = min(c, 1.0)
    cos_elbow = -((lower * lower + upper * upper - dist * dist) / (2 * upper * lower))
    s = -math.sqrt(max(0.0, 1.0 - c * c))

    r0 = ax0 * c - ax1 * s
    r1 = ax1 * c + ax0 * s
    shoulder = (np.column_stack([r0, r1, ax2]), base_pos)

    el_pos = base_pos + r0 * upper
    s2 = math.sqrt(max(0.0, 1.0 - cos_elbow * cos_elbow))
    e0 = r0 * cos_elbow - r1 * s2
    e1 = r1 * cos_elbow + r0 * s2
    elbow_t = (np.column_stack([e0, e1, ax2]), el_pos)

    wrist_t = (target.T, wpos)
    return _IK(shoulder, elbow_t, wrist_t)


class _IK(tuple):
    """The shoulder's own transform, carrying the elbow's and wrist's."""

    def __new__(cls, shoulder, elbow, wrist):
        t = super().__new__(cls, shoulder)
        t.parts = dict(elbow=elbow, wrist=wrist)
        return t

    def __getitem__(self, k):
        if isinstance(k, str):
            return self.parts[k]
        return super().__getitem__(k)


# ---- animation -------------------------------------------------------------

def read_skc(data):
    ident = data[0:4]
    version, flags, _, frame_time = struct.unpack_from("<iiif", data, 4)
    if ident != SKC_IDENT or version != SKC_VERSION:
        raise ValueError("not a version 13 .skc (%r %d)" % (ident, version))
    total_delta = np.array(struct.unpack_from("<3f", data, 20))
    total_angle, num_ch, ofs_names, num_frames = struct.unpack_from("<fiii", data, 32)
    names = [data[ofs_names + i * 32:ofs_names + i * 32 + 32].split(b"\0")[0].decode() for i in range(num_ch)]
    first = 48 + 48 * num_frames
    values = np.frombuffer(data, "<f4", num_frames * num_ch * 4, first).reshape(num_frames, num_ch, 4)
    meta = []
    for i in range(num_frames):
        f = struct.unpack_from("<11f", data, 48 + 48 * i)
        meta.append(dict(bounds=(f[0:3], f[3:6]), radius=f[6], delta=f[7:10], angle_delta=f[10]))
    return dict(flags=flags, frame_time=frame_time, total_delta=total_delta, total_angle=total_angle,
                names=names, values=values.astype(float), meta=meta)


def write_skc(anim):
    names, values, meta = anim["names"], np.asarray(anim["values"], dtype="<f4"), anim["meta"]
    num_frames, num_ch = values.shape[0], len(names)
    first = 48 + 48 * num_frames
    ofs_names = first + values.nbytes
    size = ofs_names + 32 * num_ch

    out = bytearray()
    out += SKC_IDENT
    out += struct.pack("<iiif", SKC_VERSION, anim["flags"], size, anim["frame_time"])
    out += struct.pack("<3f", *anim["total_delta"])
    out += struct.pack("<fiii", anim["total_angle"], num_ch, ofs_names, num_frames)
    for i, m in enumerate(meta):
        out += struct.pack("<11fi", *m["bounds"][0], *m["bounds"][1], m["radius"], *m["delta"],
                           m["angle_delta"], first + i * num_ch * 16)
    out += values.tobytes()
    for n in names:
        out += n.encode().ljust(32, b"\0")
    assert len(out) == size
    return bytes(out)


def frame_channels(anim, i):
    return {n: anim["values"][i, c] for c, n in enumerate(anim["names"])}
