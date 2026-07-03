"""可微 Dehaze（去雾）模块。

SS928 Dehaze 基于图像统计值估计雾浓度，自适应调整去雾强度。
支持自动/手动模式，手动模式下强度由用户配置 [0, 255]。
另有 dehaze_lut[256] 按亮度分阶控制去雾强度。

可微代理使用暗通道先验（Dark Channel Prior）实现：
  1. 暗通道提取（min filter）
  2. 大气光估计
  3. 透射率图估计
  4. 场景辐射恢复

参考：SS928 ISP 调优指南 §4.19；PQ 工具 §2.1.5.8。
"""

from __future__ import annotations

import torch
import torch.nn.functional as F


def _dark_channel(image: torch.Tensor, patch_size: int = 7) -> torch.Tensor:
    """计算暗通道：每个像素邻域内 RGB 最小值。

    Args:
        image: (B, 3, H, W) [0, 1]
        patch_size: 邻域大小
    Returns:
        (B, 1, H, W)
    """
    # 每个像素取 RGB min
    dc = image.min(dim=1, keepdim=True).values  # (B, 1, H, W)
    # 用 -max_pool(-x) 实现 min filter（保持可微）
    pad = patch_size // 2
    # reflect padding 在旧版 torch 中只支持 4D 输入，先 reshape
    dc_4d = F.pad(dc, (pad, pad, pad, pad), mode="replicate")
    dc = -F.max_pool2d(-dc_4d, patch_size, 1, 0)
    return dc


def _atmospheric_light(image: torch.Tensor, dark_ch: torch.Tensor,
                       top_pct: float = 0.001) -> torch.Tensor:
    """从暗通道最亮的像素估计大气光。

    Args:
        image: (B, 3, H, W)
        dark_ch: (B, 1, H, W)
        top_pct: 选取最亮像素的比例
    Returns:
        (B, 3, 1, 1) 大气光 RGB
    """
    B, C, H, W = image.shape
    num_pixels = H * W
    k = max(1, int(num_pixels * top_pct))

    # 取暗通道最亮的 k 个位置
    dc_flat = dark_ch.view(B, -1)  # (B, H*W)
    _, top_idx = torch.topk(dc_flat, k, dim=-1)  # (B, k)

    # 在原始图像中取对应位置的平均 RGB 作为大气光
    img_flat = image.view(B, C, -1)  # (B, 3, H*W)
    top_idx_exp = top_idx.unsqueeze(1).expand(-1, C, -1)  # (B, 3, k)
    top_vals = torch.gather(img_flat, -1, top_idx_exp)  # (B, 3, k)
    A = top_vals.mean(dim=-1, keepdim=True).unsqueeze(-1)  # (B, 3, 1, 1)
    return A


def dehaze_apply(
    image: torch.Tensor,
    strength: torch.Tensor,  # (B,) [0, 1] → HW [0, 255]
    omega: float = 0.95,
    t0: float = 0.1,
) -> torch.Tensor:
    """暗通道先验去雾。

    Args:
        image: (B, 3, H, W) [0, 1]
        strength: (B,) [0, 1] 去雾强度
        omega: 透射率保留系数（0.95 = 保留少量雾感，避免不自然）
        t0: 最小透射率下限

    Returns:
        (B, 3, H, W) [0, 1]
    """
    B = image.shape[0]
    s = strength.mean()  # 标量强度

    # 暗通道
    dark = _dark_channel(image, patch_size=7)  # (B, 1, H, W)

    # 大气光（仅在需要时计算）
    A = _atmospheric_light(image, dark)  # (B, 3, 1, 1)

    # 归一化
    norm = image / (A + 1e-6)

    # 归一化图像的暗通道 → 粗略透射率
    norm_dark = _dark_channel(norm, patch_size=7)
    t = 1.0 - omega * norm_dark  # (B, 1, H, W)

    # 强度调制
    t = 1.0 - s * (1.0 - t)
    t = t.clamp(t0, 1.0)

    # 恢复
    t_exp = t.expand(-1, 3, -1, -1)
    J = (image - A) / (t_exp + 1e-6) + A
    J = J.clamp(0.0, 1.0)

    # 强度混合：s=0 时完全直通
    output = image + s * (J - image)
    return output.clamp(0.0, 1.0)
