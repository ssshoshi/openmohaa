"""Little-endian helpers for the engine's binary formats."""

import struct


class Reader:
    def __init__(self, data, pos=0):
        self.data = data
        self.pos = pos

    def seek(self, pos):
        self.pos = pos

    def unpack(self, fmt):
        fmt = "<" + fmt
        v = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += struct.calcsize(fmt)
        return v

    def i32(self):
        return self.unpack("i")[0]

    def i16(self):
        return self.unpack("h")[0]

    def u8(self):
        return self.unpack("B")[0]

    def f32(self):
        return self.unpack("f")[0]

    def vec(self, n):
        return self.unpack("%df" % n)

    def fixed_str(self, n):
        raw = self.data[self.pos:self.pos + n]
        self.pos += n
        return cstr(raw)

    def cstring(self):
        end = self.data.find(b"\0", self.pos)
        if end < 0:  # a truncated table (manon.skd): the engine reads up to the end too
            end = len(self.data)
        s = self.data[self.pos:end].decode("latin-1")
        self.pos = end + 1
        return s


def cstr(raw):
    end = raw.find(b"\0")
    if end >= 0:
        raw = raw[:end]
    return raw.decode("latin-1")


def fixed(s, n):
    """s as a NUL padded field of n bytes (always NUL terminated)."""
    raw = s.encode("latin-1")[:n - 1]
    return raw + b"\0" * (n - len(raw))


class Writer:
    def __init__(self):
        self.buf = bytearray()

    def tell(self):
        return len(self.buf)

    def pack(self, fmt, *args):
        self.buf += struct.pack("<" + fmt, *args)

    def pack_at(self, pos, fmt, *args):
        struct.pack_into("<" + fmt, self.buf, pos, *args)

    def raw(self, b):
        self.buf += b

    def align(self, n):
        while len(self.buf) % n:
            self.buf.append(0)

    def getvalue(self):
        return bytes(self.buf)
