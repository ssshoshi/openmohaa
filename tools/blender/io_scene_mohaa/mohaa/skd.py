"""Skeletal meshes: .skd (SKMD version 5 and 6) and the older .skb (SKL version 3 and 4).

Layouts follow code/tiki/tiki_shared.h and code/skeletor/skeletor_model_file_format.h;
the loader is TIKI_CacheFileSkel in code/tiki/tiki_skel.cpp. Everything is little-endian.

A vertex has no position of its own: each weight holds an offset in its bone's space, and
the renderer places it at sum(weight * (offset * boneMatrix + boneOrigin)).
"""

import struct

from .binio import Reader, Writer, cstr, fixed

SKD_IDENT = b"SKMD"
SKB_IDENT = b"SKL "
SURF_IDENT = b"SKL "

# boneType_t
ROTATION, POSROT, IKSHOULDER, IKELBOW, IKWRIST, HOSEROT, AVROT, ZERO = range(8)
BONE_TYPE_NAMES = ["ROTATION", "POSROT", "IKSHOULDER", "IKELBOW", "IKWRIST", "HOSEROT", "AVROT", "ZERO"]

MAX_VERTS = 1000      # TIKI_MAX_VERTEXES
MAX_TRIS = 2000       # TIKI_MAX_TRIANGLES
MAX_BONES = 100       # TIKI_MAX_BONES
NAME_LEN = 32         # boneFileData_t names
CHANNEL_LEN = 32      # skelChannelName_t, so a bone name plus " rot" must fit in 31


class Bone:
    def __init__(self, name, parent=None, type=POSROT, offset=(0.0, 0.0, 0.0)):
        self.name = name
        self.parent = parent      # parent bone name, None for the world bone
        self.type = type
        self.offset = tuple(offset)  # base position in the parent's space (ROTATION and the like)
        # For the bone types the engine computes (IK, hose, average), the record's base data,
        # channel names and bone references, kept as they were so an export writes them back.
        self.base_data = b""
        self.channels = []
        self.refs = []

    def __repr__(self):
        return "Bone(%r, parent=%r, %s)" % (self.name, self.parent, BONE_TYPE_NAMES[self.type])


class Vertex:
    __slots__ = ("normal", "uv", "weights", "morphs")

    def __init__(self, normal, uv, weights, morphs=()):
        self.normal = tuple(normal)
        self.uv = tuple(uv)
        self.weights = list(weights)  # (bone index, weight, (x, y, z) in bone space)
        self.morphs = list(morphs)    # (morph target index, (x, y, z))


class Surface:
    def __init__(self, name):
        self.name = name
        self.triangles = []  # (a, b, c)
        self.verts = []
        self.collapse = None        # LOD collapse map, one entry per vertex, or None
        self.collapse_index = None


class Model:
    def __init__(self, name=""):
        self.name = name
        self.version = 5
        self.bones = []
        self.surfaces = []
        self.lod_index = [0] * 10
        self.boxes = []          # hit boxes, one bone index each
        self.morph_names = []
        self.scale = 1.0         # version 6 only

    def bone_index(self, name):
        for i, b in enumerate(self.bones):
            if b.name.lower() == name.lower():
                return i
        return -1


def rotation_channel(bone_name):
    return bone_name + " rot"


def position_channel(bone_name):
    return bone_name + " pos"


# --------------------------------------------------------------------------- reading

def read(data):
    ident = data[:4]
    if ident == SKD_IDENT:
        return _read_skd(data)
    if ident == SKB_IDENT:
        return _read_skb(data)
    raise ValueError("not an skd or skb file (ident %r)" % ident)


def _read_surfaces(r, data, first, count, version, old_verts):
    surfaces = []
    pos = first
    for _ in range(count):
        r.seek(pos)
        r.unpack("i")  # ident
        name = r.fixed_str(64)
        num_tris, num_verts, _static, ofs_tris, ofs_verts, ofs_collapse, ofs_end = r.unpack("7i")
        ofs_collapse_index = r.i32() if version > 3 else 0
        s = Surface(name)

        r.seek(pos + ofs_tris)
        flat = r.unpack("%di" % (num_tris * 3))
        s.triangles = [flat[i:i + 3] for i in range(0, len(flat), 3)]

        r.seek(pos + ofs_verts)
        for _ in range(num_verts):
            nx, ny, nz, u, v = r.vec(5)
            num_weights = r.i32()
            num_morphs = 0 if old_verts else r.i32()
            morphs = []
            for _ in range(num_morphs):
                idx = r.i32()
                morphs.append((idx, r.vec(3)))
            weights = []
            for _ in range(num_weights):
                bi, bw, ox, oy, oz = r.unpack("if3f")
                weights.append((bi, bw, (ox, oy, oz)))
            s.verts.append(Vertex((nx, ny, nz), (u, v), weights, morphs))

        if num_verts:
            r.seek(pos + ofs_collapse)
            s.collapse = list(r.unpack("%di" % num_verts))
            if version > 3:
                r.seek(pos + ofs_collapse_index)
                s.collapse_index = list(r.unpack("%di" % num_verts))
        surfaces.append(s)
        pos += ofs_end
    return surfaces


def _read_skb(data):
    r = Reader(data)
    _ident, version = r.unpack("4si")
    if version not in (3, 4):
        raise ValueError("unsupported skb version %d" % version)
    m = Model(r.fixed_str(64))
    m.version = version
    num_surfaces, num_bones, ofs_bones, ofs_surfaces, _ofs_end = r.unpack("5i")
    if version >= 4:
        m.lod_index = list(r.unpack("10i"))
        num_boxes, ofs_boxes = r.unpack("2i")
        if num_boxes > 0 and ofs_boxes > 0:
            r.seek(ofs_boxes)
            m.boxes = list(r.unpack("%di" % num_boxes))

    r.seek(ofs_bones)
    raw = []
    for _ in range(num_bones):
        parent, _box, _flags = r.unpack("hhi")
        raw.append((r.fixed_str(64), parent))
    for name, parent in raw:
        m.bones.append(Bone(name, raw[parent][0] if parent >= 0 else None, POSROT))

    m.surfaces = _read_surfaces(r, data, ofs_surfaces, num_surfaces, version, old_verts=True)
    return m


def _read_skd(data):
    r = Reader(data)
    _ident, version = r.unpack("4si")
    if version not in (5, 6):
        raise ValueError("unsupported skd version %d" % version)
    m = Model(r.fixed_str(64))
    m.version = version
    num_surfaces, num_bones, ofs_bones, ofs_surfaces, _ofs_end = r.unpack("5i")
    m.lod_index = list(r.unpack("10i"))
    num_boxes, ofs_boxes, num_morphs, ofs_morphs = r.unpack("4i")
    if version >= 6:
        m.scale = r.f32()

    pos = ofs_bones
    for _ in range(num_bones):
        r.seek(pos)
        name = r.fixed_str(32)
        parent = r.fixed_str(32)
        btype, ofs_base, ofs_chan, ofs_refs, ofs_end = r.unpack("5i")
        b = Bone(name, None if parent.lower() == "worldbone" else parent, btype)
        b.base_data = data[pos + ofs_base:pos + ofs_chan]
        b.channels = _names(data, pos + ofs_chan, pos + ofs_refs)
        b.refs = _names(data, pos + ofs_refs, pos + ofs_end)
        b.offset = decode_offset(btype, b.base_data)
        m.bones.append(b)
        pos += ofs_end

    m.surfaces = _read_surfaces(r, data, ofs_surfaces, num_surfaces, version, old_verts=False)

    if num_boxes > 0 and 0 < ofs_boxes < len(data):
        r.seek(ofs_boxes)
        m.boxes = list(r.unpack("%di" % num_boxes))
    if num_morphs > 0 and 0 < ofs_morphs < len(data):
        r.seek(ofs_morphs)
        m.morph_names = [r.cstring() for _ in range(num_morphs)]
    return m


def decode_offset(btype, base_data):
    """A bone's base position from its record's base data (LoadBoneFromBuffer2)."""
    base = Reader(base_data)
    n = len(base_data) // 4
    if btype == ROTATION and n >= 3:
        return base.vec(3)
    if btype == IKSHOULDER and n >= 7:
        base.seek(16)
        return base.vec(3)
    if btype == AVROT and n >= 4:
        base.seek(4)
        return base.vec(3)
    if btype == HOSEROT and n >= 6:
        base.seek(12)
        return base.vec(3)
    if btype in (IKELBOW, IKWRIST) and n >= 3:
        return base.vec(3)
    return (0.0, 0.0, 0.0)


def _names(data, start, end):
    if end <= start:
        return []
    return [cstr(s.encode("latin-1")) for s in data[start:end].decode("latin-1").split("\0") if s]


# --------------------------------------------------------------------------- writing

def _bone_record(b):
    w = Writer()
    w.raw(fixed(b.name, 32))
    w.raw(fixed(b.parent if b.parent else "worldbone", 32))
    if b.type == ROTATION:
        # offset, then length, weight and bendRatio (unused by a rotation bone)
        tail = b.base_data[12:24] if len(b.base_data) >= 24 else struct.pack("<3f", 1.0, 1.0, 1.0)
        base = struct.pack("<3f", *b.offset) + tail
        channels = [rotation_channel(b.name)]
        refs = []
    elif b.type == POSROT:
        base = b.base_data if len(b.base_data) == 12 else struct.pack("<3f", 1.0, 1.0, 1.0)
        channels = [rotation_channel(b.name), position_channel(b.name)]
        refs = []
    elif b.type == ZERO:
        base, channels, refs = b"", [], []
    else:
        base, channels, refs = b.base_data, b.channels, b.refs
    chan = b"".join(c.encode("latin-1") + b"\0" for c in channels)
    ref = b"".join(c.encode("latin-1") + b"\0" for c in refs)
    ofs_base = 84
    ofs_chan = ofs_base + len(base)
    ofs_refs = ofs_chan + len(chan)
    ofs_end = ofs_refs + len(ref)
    w.pack("5i", b.type, ofs_base, ofs_chan, ofs_refs, ofs_end)
    w.raw(base + chan + ref)
    return w.getvalue()


def _surface_record(s):
    nv = len(s.verts)
    w = Writer()
    w.raw(SURF_IDENT)
    w.raw(fixed(s.name, 64))
    header_at = w.tell()
    w.pack("8i", 0, 0, 0, 0, 0, 0, 0, 0)
    ofs_tris = w.tell()
    for t in s.triangles:
        w.pack("3i", *t)
    ofs_verts = w.tell()
    for v in s.verts:
        w.pack("5f", *v.normal, *v.uv)
        w.pack("2i", len(v.weights), len(v.morphs))
        for idx, off in v.morphs:
            w.pack("i3f", idx, *off)
        for bi, bw, off in v.weights:
            w.pack("if3f", bi, bw, *off)
    collapse = s.collapse if s.collapse and len(s.collapse) == nv else list(range(nv))
    collapse_index = s.collapse_index if s.collapse_index and len(s.collapse_index) == nv else [0] * nv
    ofs_collapse = w.tell()
    w.pack("%di" % nv, *collapse)
    ofs_collapse_index = w.tell()
    w.pack("%di" % nv, *collapse_index)
    ofs_end = w.tell()
    w.pack_at(header_at, "8i", len(s.triangles), nv, 0, ofs_tris, ofs_verts, ofs_collapse, ofs_end,
              ofs_collapse_index)
    return w.getvalue()


def validate(m):
    """Problems that would stop the engine loading the model."""
    errors = []
    if len(m.bones) > MAX_BONES:
        errors.append("%d bones, the engine allows %d" % (len(m.bones), MAX_BONES))
    for b in m.bones:
        if len(rotation_channel(b.name)) >= CHANNEL_LEN:
            errors.append("bone name %r is too long (at most %d characters)" % (b.name, CHANNEL_LEN - 5))
    for s in m.surfaces:
        if len(s.verts) > MAX_VERTS:
            errors.append("surface %r has %d vertices, the engine allows %d" % (s.name, len(s.verts), MAX_VERTS))
        if len(s.triangles) > MAX_TRIS:
            errors.append("surface %r has %d triangles, the engine allows %d"
                          % (s.name, len(s.triangles), MAX_TRIS))
    return errors


def write(m, version=5):
    """The model as .skd bytes. Version 5 loads in every game; 6 (Spearhead and
    Breakthrough) adds a scale."""
    if version not in (5, 6):
        raise ValueError("can only write skd version 5 or 6")
    header_size = 148 if version == 5 else 152
    bones = b"".join(_bone_record(b) for b in m.bones)
    surfaces = b"".join(_surface_record(s) for s in m.surfaces)
    ofs_bones = header_size
    ofs_surfaces = ofs_bones + len(bones)
    ofs_boxes = ofs_surfaces + len(surfaces)
    boxes = b"".join(int(b).to_bytes(4, "little", signed=True) for b in m.boxes)
    ofs_morphs = ofs_boxes + len(boxes)
    morphs = b"".join(n.encode("latin-1") + b"\0" for n in m.morph_names)
    ofs_end = ofs_morphs + len(morphs)

    w = Writer()
    w.raw(SKD_IDENT)
    w.pack("i", version)
    w.raw(fixed(m.name, 64))
    w.pack("5i", len(m.surfaces), len(m.bones), ofs_bones, ofs_surfaces, ofs_end)
    lod = (list(m.lod_index) + [0] * 10)[:10]
    w.pack("10i", *lod)
    w.pack("4i", len(m.boxes), ofs_boxes, len(m.morph_names), ofs_morphs)
    if version == 6:
        w.pack("f", m.scale)
    w.raw(bones + surfaces + boxes + morphs)
    return w.getvalue()
