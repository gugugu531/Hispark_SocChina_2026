"""可微 Gamma 模块（1D LUT 色调映射）。

SS928 硬件行为：R/G/B 共用同一 Gamma 表（1025 节点、12-bit、等距线性插值）。
本模块用 64 节点可微曲线模拟，通过 ``F.grid_sample`` 施加。
"""

from __future__ import annotations

import torch
import torch.nn.functional as F


def _enforce_monotonic(curve: torch.Tensor) -> torch.Tensor:
    """逐样本 cumulative max 保证单调性，同时保持可微。"""
    # curve: (..., N)
    mono = torch.cummax(curve, dim=-1).values
    # 固定端点: 0, 1
    mono[..., 0] = 0.0
    mono[..., -1] = 1.0
    return torch.clamp(mono, 0.0, 1.0)


def gamma_apply(image: torch.Tensor, curve: torch.Tensor) -> torch.Tensor:
    """施加 Gamma 1D 曲线。

    Args:
        image: (B, 3, H, W) 归一化 [0,1]
        curve: (B, 64) 归一化 [0,1]，自动强制单调

    Returns:
        (B, 3, H, W) 归一化 [0,1]
    """
    B = image.shape[0]
    assert curve.shape == (B, 64), f"Expected ({B}, 64), got {curve.shape}"

    # 强制单调 + 端点固定
    curve = _enforce_monotonic(curve)  # (B, 64)

    # 上采样到 1025 节点（匹配硬件 OT_ISP_GAMMA_NODE_NUM）
    curve_1025 = F.interpolate(
        curve.unsqueeze(1),  # (B, 1, 64)
        size=1025,
        mode="linear",
        align_corners=True,
    ).squeeze(1)  # (B, 1025)

    # 逐 batch、逐通道线性插值查 LUT
    # 展平空间维度: (B, 3, H*W)
    _, C, H, W = image.shape
    flat = image.reshape(B, C, -1)  # (B, 3, N)
    N = flat.shape[-1]

    # 浮点索引: [0, 1024]
    idx = flat * 1024.0
    lo = idx.floor().long().clamp(0, 1023)  # (B, 3, N)
    frac = (idx - lo.float()).clamp(0.0, 1.0)  # (B, 3, N)

    # 从 curve_1025 取 lo 和 lo+1 的值
    # curve_1025: (B, 1025) → expand to (B, 3, 1025)
    c = curve_1025.unsqueeze(1).expand(-1, C, -1)  # (B, 3, 1025)
    # 展平 batch/channel 以便 gather: (B*3, 1025)
    c_flat = c.reshape(B * C, 1025)
    lo_flat = lo.reshape(B * C, N)  # (B*3, N)
    lo_vals = c_flat.gather(1, lo_flat).reshape(B, C, N)  # (B, 3, N)
    hi_vals = c_flat.gather(1, (lo_flat + 1).clamp(0, 1024)).reshape(B, C, N)  # (B, 3, N)

    out_flat = lo_vals * (1.0 - frac) + hi_vals * frac
    return out_flat.reshape(B, C, H, W).clamp(0.0, 1.0)


def gamma_identity_curve(num_nodes: int = 64) -> torch.Tensor:
    """生成恒等 Gamma 曲线 Y=X。"""
    return torch.linspace(0.0, 1.0, num_nodes)


def gamma_brighten_curve(strength: float = 0.5, num_nodes: int = 64) -> torch.Tensor:
    """生成提亮暗部 Gamma 曲线。

    标准公式 out = in^(1/gamma)，gamma > 1 时提亮暗部（上凸曲线）。
    strength=0→gamma=1(恒等), strength=1→gamma=3.0(强提亮)。
    """
    gamma = 1.0 + strength * 2.0  # γ ∈ [1.0, 3.0]
    x = torch.linspace(0.0, 1.0, num_nodes)
    return x ** (1.0 / gamma)
