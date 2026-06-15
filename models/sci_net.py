"""SCI (Self-Calibrated Illumination) 推理结构 —— 速率对照基线。

论文：Toward Fast, Flexible, and Robust Low-Light Image Enhancement, CVPR 2022（Ma et al.）。
官方实现：vis-opt-group/SCI（`model.py` 的 `EnhanceNetwork`）。

借用来源 / Borrowed from
-----------------------
* 官方 `EnhanceNetwork`（光照估计模块 IEM）：in_conv → N 个残差 conv 块 → out_conv(Sigmoid)，
  得自校正光照残差 `illu = fea + x`，钳到 (0,1]，增强 `out = x / illu`（Retinex 提亮）。
* SCI 的核心卖点：**推理期只用一个 IEM**（训练期的自校正/多阶段在部署时退化为单块），
  默认 `channels=3, layers=3` → 极轻(全程全分辨率，但通道数仅 3)。

相对官方做了什么 / What we changed and why
-----------------------------------------
1. **本文件是"部署态"结构**：官方 conv 块含 `BatchNorm2d`；BN 在部署时标准做法是折叠进前置 Conv，
   故这里直接用 `Conv+激活`（等价于 BN 已折叠），导出图无独立 BN 算子（BN 不在 NNN 绿名单）。
2. **激活用 Clip(0,6)** 而非官方 ReLU：绿名单含 Clip 不含 Relu（语义近似，上板更稳）。与本仓库
   ExpoCurveNet 一致。速率上二者等价。
3. **残差块用独立权重**（官方为权重共享同一 conv）：对**计算量/速率无影响**（ATC 看到的是 N 个
   Conv 算子），只是导出图更直观、参数量略大。
4. 仅绿名单算子：Conv / Clip / Sigmoid / Add / Div。**全分辨率运行、无降采样**（与 Zero-DCE Lite
   同类，速率受全分辨率访存主导）。
5. **方向性提醒**：SCI 是**单向提亮**（illu≤1 → x/illu ≥ x，只能拉亮、不能压高光）。本项目要的是
   **双向曝光校正**，SCI 仅作**速率对照基线**，并非可直接落地的双向方案。
"""

from __future__ import annotations

import torch
import torch.nn as nn


class SCIEnhance(nn.Module):
    """SCI 推理期增强网络（部署态，BN 已折叠）。

    参数
    ----
    layers : 残差 conv 块数（官方默认 3）。
    channels : 中间通道数（官方默认 3，极薄）。
    act_max : 隐层 Clip 上界（6.0=ReLU6，保证导出 Clip 算子）。
    clip_output : 末尾是否 Clip 到 [0,1]。
    """

    def __init__(
        self,
        layers: int = 3,
        channels: int = 3,
        in_channels: int = 3,
        act_max: float | None = 6.0,
        clip_output: bool = True,
    ) -> None:
        super().__init__()
        self.layers = layers
        self.channels = channels
        self.in_channels = in_channels
        self.act_max = act_max
        self.clip_output = clip_output

        c, ch = in_channels, channels
        self.in_conv = nn.Conv2d(c, ch, 3, 1, 1)
        self.blocks = nn.ModuleList([nn.Conv2d(ch, ch, 3, 1, 1) for _ in range(layers)])
        self.out_conv = nn.Conv2d(ch, c, 3, 1, 1)

    def _act(self, x: torch.Tensor) -> torch.Tensor:
        if self.act_max is None:
            return torch.clamp(x, min=0.0)
        return torch.clamp(x, 0.0, self.act_max)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        fea = self._act(self.in_conv(x))
        for blk in self.blocks:
            fea = fea + self._act(blk(fea))  # 残差块
        fea = torch.sigmoid(self.out_conv(fea))
        illu = fea + x  # 自校正光照残差
        illu = torch.clamp(illu, 1e-4, 1.0)  # 避免除零（绿名单 Clip）
        out = x / illu  # Retinex 提亮（绿名单 Div）
        if self.clip_output:
            out = torch.clamp(out, 0.0, 1.0)
        return out


def build_model(layers: int = 3, channels: int = 3, **kwargs) -> SCIEnhance:
    model = SCIEnhance(layers=layers, channels=channels, **kwargs)
    model.eval()
    return model


if __name__ == "__main__":
    torch.manual_seed(0)
    net = build_model()
    dummy = torch.rand(1, 3, 576, 1024)
    with torch.no_grad():
        y = net(dummy)
    n = sum(p.numel() for p in net.parameters())
    print(f"SCI layers={net.layers} channels={net.channels} params={n}")
    print(f"out shape={tuple(y.shape)} range=[{y.min():.4f},{y.max():.4f}]")
