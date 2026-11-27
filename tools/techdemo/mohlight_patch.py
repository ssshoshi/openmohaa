"""A copy of MOHlight.exe with room for more lightmaps.

MOHlight keeps the map's lighting in a fixed 8 MB array (MAX_MAP_LIGHTING,
0x800000 at 0x63eaea0), 170 lightmaps of 128 by 128, and stops past it. The
biomes need more. This makes a copy (never touching the original) with a new
section of 12 MB, nothing on disk, as the array: every reference to the old
array (25 of them, its address plus 0 to 2, absolute: the exe has no
relocations and a fixed base) is moved to it, and the limit raised to 12 MB,
256 lightmaps (what GL1 can load, MAX_LIGHTMAPS).

    tools/techdemo/mohlight_patch.py <MOHlight.exe> <out.exe>

Only the MOHlight.exe of the MOHTools release (1.48) is patched: its SHA-1
is checked first.
"""

import hashlib
import struct
import sys

OLD_ARRAY = 0x63EAEA0
LIMIT_AT = 0x2CB70          # file offset of the cmp's immediate (cmp eax, 0x800000 at 0x42cb6f)
TEXT = (0x1000, 0x1000 + 0x42C18)
NEW_SIZE = 0xC00000
REFERENCES = 25
# the MOHTools release's MOHlight.exe (1.48)
SHA1 = "c088bca209b4d69d96616f51a8dc8e0b4f5cc9ca"


def patch(src, dst):
    d = bytearray(open(src, "rb").read())
    if hashlib.sha1(d).hexdigest() != SHA1:
        sys.exit("%s is not the MOHlight.exe this knows how to patch" % src)
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    size_hdr = struct.unpack_from("<I", d, opt + 60)[0]
    size_img = struct.unpack_from("<I", d, opt + 56)[0]
    base = struct.unpack_from("<I", d, opt + 28)[0]
    last = opt + optsz + 40 * nsec
    if last + 40 > size_hdr or any(d[last:last + 40]):
        sys.exit("no room for another section header")
    # the new section: uninitialized data, read/write
    hdr = bytearray(40)
    hdr[0:8] = b".lmbig\0\0"
    struct.pack_into("<IIIIIIHHI", hdr, 8, NEW_SIZE, size_img, 0, 0, 0, 0, 0, 0, 0xC0000080)
    d[last:last + 40] = hdr
    struct.pack_into("<H", d, pe + 6, nsec + 1)
    struct.pack_into("<I", d, opt + 56, size_img + NEW_SIZE)
    new = base + size_img
    moved = 0
    for k in range(3):
        old_b, new_b = struct.pack("<I", OLD_ARRAY + k), struct.pack("<I", new + k)
        i = TEXT[0]
        while True:
            i = d.find(old_b, i, TEXT[1])
            if i < 0:
                break
            d[i:i + 4] = new_b
            moved += 1
            i += 4
    if moved != REFERENCES:
        sys.exit("found %d references to the lighting array, expected %d" % (moved, REFERENCES))
    struct.pack_into("<I", d, LIMIT_AT, NEW_SIZE)
    open(dst, "wb").write(d)
    return hashlib.sha1(d).hexdigest()


if __name__ == "__main__":
    print(patch(sys.argv[1], sys.argv[2]))
