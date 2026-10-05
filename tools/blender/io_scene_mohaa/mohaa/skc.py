"""Skeletal animations: .skc (SKAN version 13, and the processed version 14).

Version 13 (every game) is plain: a header, one record per frame, then every channel's
value for every frame. Version 14 (Spearhead and Breakthrough) is the processed form,
written through a Huffman coded msg_t, with each channel run-length encoded. Loaders:
skeletor_c::ConvertSkelFileToGame and LoadProcessedAnimEx in
code/skeletor/skeletor_loadanimation.cpp.

A channel is named after its bone: "<bone> rot" is a quaternion (x, y, z, w), "<bone> pos"
a position in the parent bone's space; any other name is one value (a morph target weight).
"""

import struct

from . import huffman
from .binio import Reader, Writer, fixed

IDENT = b"SKAN"

CHANNEL_NONE, CHANNEL_ROTATION, CHANNEL_POSITION, CHANNEL_VALUE = range(4)



def channel_type(name):
    """GetBoneChannelType (code/skeletor/bonetable.cpp)."""
    if "Bip0" in name and "Bip01" not in name and "Footsteps" not in name:
        return CHANNEL_NONE
    if len(name) < 4:
        return CHANNEL_VALUE
    if name.endswith(" rot"):
        return CHANNEL_ROTATION
    if name.endswith(" pos"):
        return CHANNEL_POSITION
    if name.endswith(" rotFK"):
        return CHANNEL_NONE
    return CHANNEL_VALUE


class Frame:
    __slots__ = ("bounds", "radius", "delta", "angle_delta")

    def __init__(self, bounds=((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)), radius=0.0, delta=(0.0, 0.0, 0.0),
                 angle_delta=0.0):
        self.bounds = (tuple(bounds[0]), tuple(bounds[1]))
        self.radius = radius
        self.delta = tuple(delta)
        self.angle_delta = angle_delta


class Animation:
    def __init__(self):
        self.version = 13
        self.flags = 0
        self.frame_time = 0.05
        self.total_delta = (0.0, 0.0, 0.0)
        self.total_angle_delta = 0.0
        self.frames = []       # Frame per frame
        self.channels = []     # channel names
        self.values = []       # values[frame][channel] = 4 floats

    @property
    def num_frames(self):
        return len(self.frames)

    def channel(self, name):
        """[frame] -> value for one channel, or None."""
        try:
            c = self.channels.index(name)
        except ValueError:
            return None
        return [f[c] for f in self.values]


def read(data):
    if data[:4] != IDENT:
        raise ValueError("not an skc file")
    version = struct.unpack_from("<i", data, 4)[0]
    if version == 13:
        return _read_13(data)
    if version == 14:
        return _read_14(data)
    raise ValueError("unsupported skc version %d (the engine loads 13 and 14)" % version)


def _read_13(data):
    r = Reader(data, 8)
    a = Animation()
    a.version = 13
    a.flags, _bytes_used, a.frame_time = r.unpack("iif")
    a.total_delta = r.vec(3)
    a.total_angle_delta = r.f32()
    num_channels, ofs_names, num_frames = r.unpack("3i")
    for _ in range(num_frames):
        b = r.vec(6)
        radius = r.f32()
        delta = r.vec(3)
        angle_delta = r.f32()
        r.i32()  # iOfsChannels
        a.frames.append(Frame((b[:3], b[3:]), radius, delta, angle_delta))
    r.seek(ofs_names)
    a.channels = [r.fixed_str(32) for _ in range(num_channels)]
    first = 48 + 48 * num_frames
    for i in range(num_frames):
        flat = struct.unpack_from("<%df" % (4 * num_channels), data, first + 16 * num_channels * i)
        a.values.append([flat[j:j + 4] for j in range(0, len(flat), 4)])
    return a


def _read_14(data):
    r = Reader(huffman.decode(data[8:]))
    a = Animation()
    a.version = 14
    num_channels = r.i16()
    a.flags, a.frame_time = r.unpack("if")
    a.total_delta = r.vec(3)
    a.total_angle_delta = r.f32()
    num_frames = r.i32()
    r.unpack("3B")  # bHasDelta, bHasUpper, bHasMorph: worked out again from the channel names
    a.channels = [r.cstring() for _ in range(num_channels)]
    for _ in range(num_frames):
        b = r.vec(6)
        radius = r.f32()
        delta = r.vec(3)
        a.frames.append(Frame((b[:3], b[3:]), radius, delta, r.f32()))
    r.vec(6)  # bounds of the whole animation
    a.values = [[None] * num_channels for _ in range(num_frames)]
    for c, name in enumerate(a.channels):
        kind = channel_type(name)
        count = r.i16()
        keys = []
        if kind != CHANNEL_NONE:
            n = {CHANNEL_ROTATION: 4, CHANNEL_POSITION: 3, CHANNEL_VALUE: 1}[kind]
            for _ in range(count):
                frame, _prev = r.unpack("hh")
                v = r.vec(n)
                keys.append((frame, tuple(v) + (0.0,) * (4 - n)))
        # A frame takes the last key at or before it (skeletor_c::GetFrame's search).
        k = 0
        current = keys[0][1] if keys else (0.0, 0.0, 0.0, 1.0 if kind == CHANNEL_ROTATION else 0.0)
        for f in range(num_frames):
            while k < len(keys) and keys[k][0] <= f:
                current = keys[k][1]
                k += 1
            a.values[f][c] = current
    return a


def write(a):
    """Version 13 bytes: every game loads them (Spearhead and Breakthrough too)."""
    num_frames = len(a.frames)
    num_channels = len(a.channels)
    if num_frames != len(a.values):
        raise ValueError("values has %d frames, frames has %d" % (len(a.values), num_frames))
    first = 48 + 48 * num_frames
    ofs_names = first + 16 * num_channels * num_frames
    size = ofs_names + 32 * num_channels
    w = Writer()
    w.raw(IDENT)
    w.pack("iiif", 13, a.flags, size, a.frame_time)  # nBytesUsed: the file's size
    w.pack("3ff", *a.total_delta, a.total_angle_delta)
    w.pack("3i", num_channels, ofs_names, num_frames)
    for i, f in enumerate(a.frames):
        w.pack("6f", *f.bounds[0], *f.bounds[1])
        w.pack("f3ff", f.radius, *f.delta, f.angle_delta)
        w.pack("i", first + 16 * num_channels * i)
    for row in a.values:
        for v in row:
            v = tuple(v) + (0.0,) * (4 - len(v))
            w.pack("4f", *v[:4])
    for name in a.channels:
        w.raw(fixed(name, 32))
    return w.getvalue()
