"""CoTF（Real-Time Exposure Correction, CVPR 2024, HUST-IAL/CoTF）速率/可行性探针。

CoTF 思路：低分辨率编码器预测**全局变换 + 3D-LUT**，全分辨率用 3D-LUT 施加（roadmap §8 首选方向，
可对接 ISP 硬件 3D-LUT）。**双向**曝光校正、实时、有开源码。

本文件把 CoTF 拆成两块，分别回答两个问题：
  1. `CoTFParamNet`：低分辨率"出参数"的 NN —— 它在 NPU 上要多久？（预期很快，因为只在低分辨率跑）
  2. `lut_apply_graph`：全分辨率 3D-LUT 施加（三线性插值）—— 这个算子能否落 NNN AICore？
     （这是 roadmap §9 点 8 标注"待评估"的关键风险：3D-LUT 三线性插值≈grid_sample，可能红名单。）

相对官方 / 说明
--------------
* 不复刻官方自适应采样/协同变换全部细节；`CoTFParamNet` 是"低分辨率 conv 编码器 → 全局池化 →
  预测 3D-LUT 系数"的代理，足以测 NN-only 的量级。
* `lut_apply_graph` 用 `F.grid_sample`（3D）实现标准 3D-LUT 三线性查表 —— 这是判定该算子能否上板的关键。
* 仅作探针：随机权重，测速率 + 算子可行性。
"""

from __future__ import annotations

import torch
import torch.nn as nn
import torch.nn.functional as F


class CoTFParamNet(nn.Module):
    """低分辨率编码器：下采样输入 → conv 栈 → 全局池化 → 预测 3D-LUT 系数（NN-only 部分）。"""

    def __init__(self, down: int = 8, ch: int = 32, lut_dim: int = 9, in_channels: int = 3,
                 act_max: float = 6.0) -> None:
        super().__init__()
        self.down = nn.AvgPool2d(down, down)
        a = act_max
        self.enc = nn.Sequential(
            nn.Conv2d(in_channels, ch, 3, 2, 1), nn.Hardtanh(0.0, a),
            nn.Conv2d(ch, ch, 3, 2, 1), nn.Hardtanh(0.0, a),
            nn.Conv2d(ch, 2 * ch, 3, 2, 1), nn.Hardtanh(0.0, a),
            nn.Conv2d(2 * ch, 2 * ch, 3, 1, 1), nn.Hardtanh(0.0, a),
        )
        self.gap = nn.AdaptiveAvgPool2d(1)
        self.head = nn.Conv2d(2 * ch, in_channels * lut_dim ** 3, 1)  # 预测 LUT 系数
        self.lut_dim = lut_dim
        self.in_channels = in_channels

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        f = self.enc(self.down(x))
        f = self.gap(f)
        return self.head(f)  # (1, 3*D^3, 1, 1) —— 全图 3D-LUT 系数


class LUTApply(nn.Module):
    """全分辨率 3D-LUT 三线性施加（grid_sample 3D）。用于判定该算子能否上 NNN。"""

    def __init__(self, lut_dim: int = 9, in_channels: int = 3) -> None:
        super().__init__()
        self.lut_dim = lut_dim
        # 固定一个 identity-ish LUT 作常量（探针只看算子，不看数值）
        self.register_buffer("lut", torch.rand(1, in_channels, lut_dim, lut_dim, lut_dim))

    def forward(self, img: torch.Tensor) -> torch.Tensor:
        # img (1,3,H,W) in [0,1] → 取 RGB 作 3D 网格坐标 → grid_sample 查 LUT
        grid = (img * 2.0 - 1.0).permute(0, 2, 3, 1).unsqueeze(1)  # (1,1,H,W,3)
        out = F.grid_sample(self.lut, grid, mode="bilinear", align_corners=True)  # (1,3,1,H,W)
        return out.squeeze(2)


def build_param_net(**kwargs) -> CoTFParamNet:
    m = CoTFParamNet(**kwargs)
    m.eval()
    return m


def build_lut_apply(**kwargs) -> LUTApply:
    m = LUTApply(**kwargs)
    m.eval()
    return m


if __name__ == "__main__":
    torch.manual_seed(0)
    pn = build_param_net()
    x = torch.rand(1, 3, 576, 1024)
    p = pn(x)
    print(f"ParamNet params={sum(t.numel() for t in pn.parameters())} out={tuple(p.shape)}")
    la = build_lut_apply()
    print(f"LUTApply out={tuple(la(x).shape)}")
