"""Real-ESRGAN's 4x network (RRDBNet), run in tiles on the GPU.

The architecture is written out here rather than taken from basicsr, whose
releases no longer import against current torchvision. The weights are the
official RealESRGAN_x4plus.pth (BSD-3), downloaded on first use; any 4x RRDBNet
checkpoint in the same layout works with --model.
"""

import hashlib
import os
import urllib.request

import numpy as np
import torch
from torch import nn
from torch.nn import functional as F

WEIGHTS_URL = ("https://github.com/xinntao/Real-ESRGAN/releases/download/"
               "v0.1.0/RealESRGAN_x4plus.pth")
WEIGHTS_SHA256 = \
    "4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1"
CACHE_DIR = os.path.expanduser("~/.cache/openmohaa-texupscale")
SCALE = 4


class ResidualDenseBlock(nn.Module):
    def __init__(self, nf=64, gc=32):
        super().__init__()
        self.conv1 = nn.Conv2d(nf, gc, 3, 1, 1)
        self.conv2 = nn.Conv2d(nf + gc, gc, 3, 1, 1)
        self.conv3 = nn.Conv2d(nf + 2 * gc, gc, 3, 1, 1)
        self.conv4 = nn.Conv2d(nf + 3 * gc, gc, 3, 1, 1)
        self.conv5 = nn.Conv2d(nf + 4 * gc, nf, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, inplace=True)

    def forward(self, x):
        x1 = self.lrelu(self.conv1(x))
        x2 = self.lrelu(self.conv2(torch.cat((x, x1), 1)))
        x3 = self.lrelu(self.conv3(torch.cat((x, x1, x2), 1)))
        x4 = self.lrelu(self.conv4(torch.cat((x, x1, x2, x3), 1)))
        x5 = self.conv5(torch.cat((x, x1, x2, x3, x4), 1))
        return x5 * 0.2 + x


class RRDB(nn.Module):
    def __init__(self, nf, gc=32):
        super().__init__()
        self.rdb1 = ResidualDenseBlock(nf, gc)
        self.rdb2 = ResidualDenseBlock(nf, gc)
        self.rdb3 = ResidualDenseBlock(nf, gc)

    def forward(self, x):
        return self.rdb3(self.rdb2(self.rdb1(x))) * 0.2 + x


class RRDBNet(nn.Module):
    def __init__(self, nb=23, nf=64, gc=32):
        super().__init__()
        self.conv_first = nn.Conv2d(3, nf, 3, 1, 1)
        self.body = nn.Sequential(*[RRDB(nf, gc) for _ in range(nb)])
        self.conv_body = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up1 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up2 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_hr = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_last = nn.Conv2d(nf, 3, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, inplace=True)

    def forward(self, x):
        feat = self.conv_first(x)
        feat = feat + self.conv_body(self.body(feat))
        feat = self.lrelu(self.conv_up1(
            F.interpolate(feat, scale_factor=2, mode="nearest")))
        feat = self.lrelu(self.conv_up2(
            F.interpolate(feat, scale_factor=2, mode="nearest")))
        return self.conv_last(self.lrelu(self.conv_hr(feat)))


def fetch_weights():
    """The default weights, downloaded once and checked."""
    path = os.path.join(CACHE_DIR, os.path.basename(WEIGHTS_URL))
    if not os.path.exists(path):
        os.makedirs(CACHE_DIR, exist_ok=True)
        print("downloading %s" % WEIGHTS_URL)
        urllib.request.urlretrieve(WEIGHTS_URL, path + ".part")
        os.replace(path + ".part", path)
    with open(path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    if digest != WEIGHTS_SHA256:
        raise RuntimeError("%s: unexpected sha256 %s" % (path, digest))
    return path


class Upscaler:
    """4x upscaling of float RGB arrays in [0, 1], tile by tile."""

    def __init__(self, model_path=None, tile=256, overlap=16, device=None):
        self.device = torch.device(device or (
            "cuda" if torch.cuda.is_available() else "cpu"))
        state = torch.load(model_path or fetch_weights(),
                           map_location="cpu", weights_only=True)
        for key in ("params_ema", "params"):
            if key in state:
                state = state[key]
                break
        net = RRDBNet()
        net.load_state_dict(state, strict=True)
        # fp32: the GTX 10 series runs fp16 at a fraction of fp32's rate
        self.net = net.eval().to(self.device)
        self.tile, self.overlap = tile, overlap

    def name(self):
        if self.device.type == "cuda":
            return torch.cuda.get_device_name(self.device)
        return "CPU"

    @torch.inference_mode()
    def upscale(self, rgb):
        """(h, w, 3) float32 -> (4h, 4w, 3) float32."""
        h, w, _ = rgb.shape
        t = torch.from_numpy(np.ascontiguousarray(rgb.transpose(2, 0, 1)))
        t = t.unsqueeze(0).to(self.device)
        out = torch.empty((1, 3, h * SCALE, w * SCALE), device=self.device)
        step, ov = self.tile, self.overlap
        for y in range(0, h, step):
            for x in range(0, w, step):
                # run the tile with a margin, keep only its own part
                y0, y1 = max(0, y - ov), min(h, y + step + ov)
                x0, x1 = max(0, x - ov), min(w, x + step + ov)
                piece = self.net(t[:, :, y0:y1, x0:x1])
                ty1, tx1 = min(h, y + step), min(w, x + step)
                out[:, :, y * SCALE:ty1 * SCALE, x * SCALE:tx1 * SCALE] = \
                    piece[:, :, (y - y0) * SCALE:(ty1 - y0) * SCALE,
                          (x - x0) * SCALE:(tx1 - x0) * SCALE]
        out = out.clamp_(0, 1)[0].permute(1, 2, 0).cpu().numpy()
        return out
