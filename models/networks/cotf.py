"""受 CoTF 启发的 AICore 友好 param-net。

CoTF 思路：低分辨率编码器预测**全局变换 + 3D-LUT**，全分辨率用 ISP CLUT 施加。
该路线支持双向曝光校正，并把模型移出每帧关键路径。

本文件把 CoTF 拆成两块，分别回答两个问题：
  1. `CoTFParamNet`：低分辨率"出参数"的 NN —— 它在 NPU 上要多久？（预期很快，因为只在低分辨率跑）
  2. `lut_apply_graph`：全分辨率 3D-LUT 施加（三线性插值）—— 这个算子能否落 NNN AICore？
     （这是 architecture.md §4.1 已验证的关键风险：3D-LUT 三线性插值≈grid_sample。）

相对官方 / 说明
--------------
* 不复刻官方自适应采样/协同变换全部细节；`CoTFParamNet` 是"低分辨率 conv 编码器 → 全局池化 →
  预测 3D-LUT 系数"的代理，足以测 NN-only 的量级。
* `lut_apply_graph` 用 `F.grid_sample`（3D）实现标准 3D-LUT 三线性查表 —— 这是判定该算子能否上板的关键。
* 参数网络现在可正式训练：输出以恒等 3D-LUT 初始化，并限制在 ``[0, 1]``。
* ``LUTApply`` 仅供主机训练/验证，不进入部署 ONNX；部署图仍只包含 param-net。
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
        self.reset_parameters()

    def reset_parameters(self) -> None:
        """让未训练模型输出恒等 LUT，避免随机权重直接破坏板端画面。"""
        nn.init.zeros_(self.head.weight)
        axis = torch.linspace(0.0, 1.0, self.lut_dim)
        r, g, b = torch.meshgrid(axis, axis, axis, indexing="ij")
        identity = torch.stack((r, g, b), dim=0).reshape(-1)
        with torch.no_grad():
            self.head.bias.copy_(identity)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        f = self.enc(self.down(x))
        f = self.gap(f)
        return F.hardtanh(self.head(f), 0.0, 1.0)  # (N, 3*D^3, 1, 1)

    def as_lut(self, flat: torch.Tensor) -> torch.Tensor:
        """把扁平输出变为训练用 ``(N, 3, D, D, D)`` LUT。"""
        return flat.reshape(flat.shape[0], self.in_channels, self.lut_dim, self.lut_dim,
                            self.lut_dim)


class LUTApply(nn.Module):
    """主机训练用 3D-LUT 三线性施加；该模块不导出到板端。"""

    def __init__(self, lut_dim: int = 9, in_channels: int = 3) -> None:
        super().__init__()
        self.lut_dim = lut_dim
        axis = torch.linspace(0.0, 1.0, lut_dim)
        r, g, b = torch.meshgrid(axis, axis, axis, indexing="ij")
        self.register_buffer("lut", torch.stack((r, g, b), dim=0).unsqueeze(0))

    def forward(self, img: torch.Tensor, lut: torch.Tensor | None = None) -> torch.Tensor:
        # grid_sample 的坐标顺序是 W/H/D；LUT 轴是 R/G/B，因此网格需按 B/G/R 排列。
        active_lut = self.lut if lut is None else lut
        if active_lut.shape[0] == 1 and img.shape[0] != 1:
            active_lut = active_lut.expand(img.shape[0], -1, -1, -1, -1)
        grid = (img[:, [2, 1, 0]] * 2.0 - 1.0).permute(0, 2, 3, 1).unsqueeze(1)
        out = F.grid_sample(active_lut, grid, mode="bilinear", padding_mode="border",
                            align_corners=True)
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
