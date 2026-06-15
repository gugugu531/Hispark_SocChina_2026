"""MSEC（Learning Multi-Scale Photo Exposure Correction）速率对照。

官方：mahmoudnafifi/Exposure_Correction。本质是**多尺度（拉普拉斯金字塔）coarse-to-fine
U-Net**：在最粗尺度用主 U-Net 子网处理，逐级上采样细化。**双向**（过曝+欠曝同框架），
且自带过+欠曝配对数据集；本项目主要参考其数据价值，不采用其部署结构。

借用来源 / Borrowed from
-----------------------
* 官方多尺度 encoder-decoder（U-Net）结构与跳连。

相对官方做了什么 / What we changed and why（**结构/速率代理，非官方权重**）
-------------------------------------------------------------------
1. **以单个 U-Net backbone 作 MSEC 的算力代理**：MSEC 的主导开销是全分辨率 encoder-decoder；
   这里实现一个 depth=3 的 U-Net（encoder 3 级 MaxPool 降采样 + decoder ConvTranspose 上采样 +
   concat 跳连），未显式搭拉普拉斯金字塔（金字塔的额外全分辨率卷积只会更慢，不影响"偏重"结论）。
2. **上采样用 ConvTranspose** 而非官方 bilinear（NNN 禁 Resize，dev-guide §6）。
3. **激活 Clip(0,6)**（绿名单，非 Relu）；输出 Clip[0,1]。
4. 仅绿名单算子：Conv / MaxPool / ConvTranspose / Clip / Concat。**全分辨率 U-Net → 预期最重。**
5. `base_ch` 参数化（官方主网约 24–32）。随机权重，只测速率。
"""

from __future__ import annotations

import torch
import torch.nn as nn


def _bilinear_kernel(channels: int, scale: int = 2) -> torch.Tensor:
    ksize = 2 * scale
    factor = (ksize + 1) // 2
    center = factor - 1.0 if ksize % 2 == 1 else factor - 0.5
    og = torch.arange(ksize, dtype=torch.float32)
    f1 = 1.0 - torch.abs(og - center) / factor
    filt = f1[:, None] * f1[None, :]
    w = torch.zeros(channels, 1, ksize, ksize)
    w[:, 0] = filt
    return w


class MSECUNet(nn.Module):
    """MSEC 类多尺度 U-Net（depth=3），ConvTranspose 上采样，绿名单算子。"""

    def __init__(self, base_ch: int = 24, in_channels: int = 3,
                 act_max: float | None = 6.0, clip_output: bool = True) -> None:
        super().__init__()
        self.base_ch = base_ch
        self.act_max = act_max
        self.clip_output = clip_output
        c = in_channels
        C = base_ch

        def blk(i, o):
            return nn.Sequential(nn.Conv2d(i, o, 3, 1, 1), nn.Hardtanh(0.0, act_max if act_max else 6.0),
                                 nn.Conv2d(o, o, 3, 1, 1), nn.Hardtanh(0.0, act_max if act_max else 6.0))

        self.pool = nn.MaxPool2d(2, 2)
        self.enc0 = blk(c, C)          # full,  C
        self.enc1 = blk(C, 2 * C)      # /2,    2C
        self.enc2 = blk(2 * C, 4 * C)  # /4,    4C
        self.bott = blk(4 * C, 8 * C)  # /8,    8C

        def up(ch):
            m = nn.ConvTranspose2d(ch, ch, 4, 2, 1, groups=ch, bias=False)
            m.weight.data.copy_(_bilinear_kernel(ch))
            m.weight.requires_grad_(False)
            return m

        self.up2 = up(8 * C)
        self.dec2 = blk(8 * C + 4 * C, 4 * C)
        self.up1 = up(4 * C)
        self.dec1 = blk(4 * C + 2 * C, 2 * C)
        self.up0 = up(2 * C)
        self.dec0 = blk(2 * C + C, C)
        self.out_conv = nn.Conv2d(C, c, 3, 1, 1)

    def forward(self, x):
        e0 = self.enc0(x)
        e1 = self.enc1(self.pool(e0))
        e2 = self.enc2(self.pool(e1))
        b = self.bott(self.pool(e2))
        d2 = self.dec2(torch.cat([self.up2(b), e2], 1))
        d1 = self.dec1(torch.cat([self.up1(d2), e1], 1))
        d0 = self.dec0(torch.cat([self.up0(d1), e0], 1))
        out = self.out_conv(d0)
        if self.clip_output:
            out = torch.clamp(out, 0.0, 1.0)
        return out


def build_model(base_ch: int = 24, **kwargs) -> MSECUNet:
    m = MSECUNet(base_ch=base_ch, **kwargs)
    m.eval()
    return m


if __name__ == "__main__":
    torch.manual_seed(0)
    net = build_model()
    y = net(torch.rand(1, 3, 576, 1024))
    print(f"MSEC-UNet base_ch={net.base_ch} params={sum(p.numel() for p in net.parameters())}")
    print(f"out={tuple(y.shape)} range=[{y.min():.3f},{y.max():.3f}]")
