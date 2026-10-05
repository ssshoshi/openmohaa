"""Decoding of the engine's msg_t bit stream (non-OOB reads)."""

from ._huffcodes import CODES

_tree = None


def _build():
    # A node is [left, right]; a leaf is the symbol as an int.
    root = [None, None]
    for sym, bits in enumerate(CODES):
        node = root
        for b in bits[:-1]:
            i = b == "1"
            if node[i] is None:
                node[i] = [None, None]
            node = node[i]
        node[bits[-1] == "1"] = sym
    return root


def decode(data):
    """Every whole byte symbol in data, read bit by bit from each byte's low bit
    up, the way MSG_ReadBits reads a msg_t that is not out of band."""
    global _tree
    if _tree is None:
        _tree = _build()
    root = _tree
    out = bytearray()
    node = root
    for byte in data:
        for shift in range(8):
            node = node[(byte >> shift) & 1]
            if node.__class__ is int:
                out.append(node)
                node = root
            elif node is None:
                return bytes(out)
    return bytes(out)
