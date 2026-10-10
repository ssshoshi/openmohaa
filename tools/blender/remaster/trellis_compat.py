"""Running TRELLIS on GPUs older or smaller than the ones it is written for.

TRELLIS assumes an Ampere-class GPU with 16 GB: flash-attention, half-precision weights, spconv
kernels built for the card, and nvdiffrast for its mesh clean-up. Tested on a GTX 1080
(Pascal, 8 GB), where none of that holds:

  - flash-attention needs sm_80, so xformers is used (ATTN_BACKEND, set before TRELLIS loads);
  - spconv has no prebuilt kernels for sm_61 and its run-time NVRTC build fails, so its 3x3x3
    submanifold convolutions (the only kind TRELLIS uses with kernels) run in plain PyTorch
    here instead;
  - Pascal runs half precision at a fraction of its single-precision rate (a sampling step
    took 25 s in fp16, 2.6 s in fp32), so the models run in fp32;
  - fp32 weights don't fit in 8 GB together, so each model waits in fp16 in system memory and
    is moved to the GPU (and widened to fp32) only while it runs;
  - nvdiffrast isn't needed: the hidden faces it finds are found here by ray casting.
"""

import itertools
import os
import sys
import threading
import time

import numpy as np


def needed(device=0):
    """Whether the GPU needs these workarounds: anything before Ampere (sm_80)."""
    import torch
    return torch.cuda.get_device_capability(device) < (8, 0)


def before_import():
    """Settings that must be made before TRELLIS is imported."""
    os.environ.setdefault("ATTN_BACKEND", "xformers")


def after_import():
    # xformers 0.0.28 has BlockDiagonalMask in fmha.attn_bias only; TRELLIS looks in fmha
    import xformers.ops.fmha as fmha
    from xformers.ops.fmha import attn_bias
    if not hasattr(fmha, "BlockDiagonalMask"):
        fmha.BlockDiagonalMask = attn_bias.BlockDiagonalMask


def memory_watchdog(min_mb=1024):
    """Abort when system memory runs low, rather than swapping the machine to a halt
    (on WSL that freezes everything until it is restarted)."""
    def watch():
        while True:
            with open("/proc/meminfo") as f:
                for line in f:
                    if line.startswith("MemAvailable"):
                        mb = int(line.split()[1]) // 1024
                        if mb < min_mb:
                            sys.stderr.write("only %d MB of memory left: stopping TRELLIS\n" % mb)
                            os._exit(3)
            time.sleep(1)
    if os.path.exists("/proc/meminfo"):
        threading.Thread(target=watch, daemon=True).start()


# ----------------------------------------------------------------------------- sparse conv

def _keys(coords, shape):
    c = coords.long()
    return ((c[:, 0] * shape[0] + c[:, 1]) * shape[1] + c[:, 2]) * shape[2] + c[:, 3]


def _subm_forward(self, t):
    """spconv.SubMConv3d.forward for odd kernels: every output site sums its neighbours'
    features times the kernel tap at their offset (weights are out, k, k, k, in)."""
    import torch
    w = self.weight
    k = w.shape[1]
    feats, coords, shape = t.features, t.indices, t.spatial_shape
    keys = _keys(coords, shape)
    order = keys.argsort()
    sorted_keys = keys[order]
    dil = self.dilation[0]
    r = k // 2
    bound = torch.tensor(shape, device=coords.device)
    out = feats.new_zeros(len(feats), w.shape[0])
    for i, j, l in itertools.product(range(k), repeat=3):
        off = torch.tensor([0, (i - r) * dil, (j - r) * dil, (l - r) * dil], device=coords.device)
        nc = coords.long() + off
        inside = ((nc[:, 1:] >= 0) & (nc[:, 1:] < bound)).all(1)
        nk = _keys(nc, shape)
        pos = torch.searchsorted(sorted_keys, nk).clamp_(max=len(sorted_keys) - 1)
        hit = inside & (sorted_keys[pos] == nk)
        if hit.any():
            out[hit] += feats[order[pos[hit]]] @ w[:, i, j, l, :].t()
    if self.bias is not None:
        out = out + self.bias
    return t.replace_feature(out)


def patch_sparse_conv(module):
    """Route a model's spconv submanifold convolutions through _subm_forward. 1x1x1 ones are
    already a plain matrix multiply in spconv and are left alone."""
    import spconv.pytorch as spconv
    n = 0
    for m in module.modules():
        if isinstance(m, spconv.SubMConv3d) and not m.conv1x1:
            m.forward = _subm_forward.__get__(m)
            n += 1
    return n


def check_sparse_conv():
    """_subm_forward against spconv's own CPU implementation."""
    import torch
    import spconv.pytorch as spconv
    g = torch.Generator().manual_seed(0)
    coords = torch.unique(torch.randint(0, 16, (1500, 3), generator=g), dim=0)
    batch = torch.randint(0, 2, (len(coords), 1), generator=g)
    x = spconv.SparseConvTensor(torch.randn(len(coords), 8, generator=g),
                                torch.cat([batch, coords], 1).int(), [16, 16, 16], 2)
    conv = spconv.SubMConv3d(8, 5, 3, algo=spconv.ConvAlgo.Native)
    with torch.no_grad():
        conv.weight.normal_(generator=g)
        conv.bias.normal_(generator=g)
        ref = conv(x).features
        ours = _subm_forward(conv, x).features
    return float((ref - ours).abs().max() / ref.abs().mean())


# ----------------------------------------------------------------------------- models

def fp32_offloaded(pipeline):
    """Each model in fp32 on the GPU while it runs, in fp16 in system memory otherwise. Only
    the mesh decoder is kept."""
    import torch
    for k in ("slat_decoder_gs", "slat_decoder_rf"):
        pipeline.models.pop(k, None)
    models = list(pipeline.models.values())
    for m in models:
        if hasattr(m, "use_fp16"):
            m.use_fp16, m.dtype = False, torch.float32
        m.half().cpu().eval()

    def to_gpu(mod, _args):
        if next(mod.parameters()).device.type == "cuda":
            return
        for other in models:
            if other is not mod and next(other.parameters()).device.type == "cuda":
                other.half().cpu()
        torch.cuda.empty_cache()
        mod.cuda().float()

    for m in models:
        m.register_forward_pre_hook(to_gpu)
        patch_sparse_conv(m)
    # the samplers make their noise on pipeline.device, which would read the CPU
    type(pipeline).device = property(lambda self: torch.device("cuda"))


# ----------------------------------------------------------------------------- clean-up

def visible_faces(vertices, faces, directions=96, resolution=384):
    """Faces seen from at least one of `directions` orthographic views around the mesh:
    what TRELLIS's postprocess_mesh keeps (it renders with nvdiffrast) without its
    decimation, which remaster-build does itself."""
    import open3d as o3d
    scene = o3d.t.geometry.RaycastingScene()
    scene.add_triangles(o3d.core.Tensor(vertices.astype(np.float32)),
                        o3d.core.Tensor(faces.astype(np.uint32)))
    centre = (vertices.max(0) + vertices.min(0)) / 2
    radius = np.linalg.norm(vertices - centre, axis=1).max() * 1.05
    seen = np.zeros(len(faces), dtype=bool)
    # directions spread evenly over the sphere (golden spiral)
    i = np.arange(directions) + 0.5
    phi = np.arccos(1 - 2 * i / directions)
    theta = np.pi * (1 + 5 ** 0.5) * i
    dirs = np.stack([np.cos(theta) * np.sin(phi), np.sin(theta) * np.sin(phi), np.cos(phi)], 1)
    grid = (np.arange(resolution) + 0.5) / resolution * 2 - 1
    gu, gv = np.meshgrid(grid, grid)
    for d in dirs:
        u = np.cross(d, [0, 0, 1] if abs(d[2]) < 0.9 else [1, 0, 0])
        u /= np.linalg.norm(u)
        v = np.cross(d, u)
        origins = centre - d * radius * 2 + (gu.reshape(-1, 1) * u + gv.reshape(-1, 1) * v) * radius
        rays = np.concatenate([origins, np.broadcast_to(d, origins.shape)], 1).astype(np.float32)
        ids = scene.cast_rays(o3d.core.Tensor(rays))["primitive_ids"].numpy()
        seen[ids[ids != scene.INVALID_ID]] = True
    return seen
