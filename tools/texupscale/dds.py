"""Mipmapped, block-compressed DDS files the GL2 renderer loads.

R_LoadDDS (code/renderergl2/tr_image_dds.c) reads DXT1 and DXT5 from the
legacy FourCC header and BC7 from a DX10 header. R_FindImageFile keeps a DDS's
mipmaps only when its chain reaches a single block (4x4 or smaller), so every
chain here goes down to that.
"""

import struct

import etcpak
import numpy as np

# name: (etcpak encoder, bytes per 4x4 block, FourCC, DXGI format or None)
FORMATS = {
    "dxt1": (etcpak.compress_bc1, 8, b"DXT1", None),
    "dxt5": (etcpak.compress_bc3, 16, b"DXT5", None),
    "bc7": (etcpak.compress_bc7, 16, b"DX10", 98),  # DXGI_FORMAT_BC7_UNORM
}

_DDSD = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000  # caps..mipcount, linsize
_DDSCAPS = 0x8 | 0x1000 | 0x400000                    # complex, texture, mipmap
_DDPF_FOURCC = 0x4


def srgb_to_linear(x):
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(x):
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.0031308, x * 12.92,
                    1.055 * x ** (1.0 / 2.4) - 0.055)


def _halve(a):
    """2x2 box filter; an axis already at 1 is left alone."""
    h, w = a.shape[:2]
    if h > 1:
        a = (a[0::2] + a[1::2]) * 0.5
    if w > 1:
        a = (a[:, 0::2] + a[:, 1::2]) * 0.5
    return a


def _coverage(alpha, ref=0.5):
    return float(np.mean(alpha >= ref))


def _keep_coverage(alpha, target):
    """Scale alpha so as many texels pass an alpha test as at full size.

    Box-filtered mips of alpha-tested foliage and fences lose texels to the
    test level by level, so they thin out and vanish with distance.
    """
    lo, hi = 0.0, 4.0
    for _ in range(12):
        mid = (lo + hi) * 0.5
        if _coverage(np.clip(alpha * mid, 0, 1)) < target:
            lo = mid
        else:
            hi = mid
    return np.clip(alpha * hi, 0, 1)


def mip_chain(rgba, alpha_test=False):
    """Full-size float RGBA (power-of-two sides) -> list of uint8 levels.

    Colour is averaged in linear light, alpha as is.
    """
    lin = srgb_to_linear(rgba[..., :3])
    alpha = rgba[..., 3]
    target = _coverage(alpha) if alpha_test else None
    levels = []
    while True:
        a = alpha if target is None or not levels else \
            _keep_coverage(alpha, target)
        level = np.concatenate([linear_to_srgb(lin), a[..., None]], axis=2)
        levels.append(np.round(level * 255).astype(np.uint8))
        h, w = lin.shape[:2]
        if h <= 4 and w <= 4:
            return levels
        lin, alpha = _halve(lin), _halve(alpha)


def _encode(level, encoder):
    h, w = level.shape[:2]
    ph, pw = -h % 4, -w % 4
    if ph or pw:
        level = np.pad(level, ((0, ph), (0, pw), (0, 0)), mode="edge")
    return encoder(np.ascontiguousarray(level).tobytes(), w + pw, h + ph)


def write_dds(levels, fmt):
    """DDS bytes for a mip chain in one of FORMATS."""
    encoder, block, fourcc, dxgi = FORMATS[fmt]
    h, w = levels[0].shape[:2]
    top = ((w + 3) // 4) * ((h + 3) // 4) * block
    head = struct.pack(
        "<4sIIIIIII44xII4sIIIII5I", b"DDS ", 124, _DDSD, h, w, top, 0,
        len(levels), 32, _DDPF_FOURCC, fourcc, 0, 0, 0, 0, 0,
        _DDSCAPS, 0, 0, 0, 0)
    if dxgi is not None:
        head += struct.pack("<5I", dxgi, 3, 0, 1, 0)  # TEXTURE2D, 1 slice
    return head + b"".join(_encode(l, encoder) for l in levels)
