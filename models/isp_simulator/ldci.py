"""可微 LDCI（Local Dynamic Contrast Improvement）模块。

SS928 LDCI 基于 9×9 局域直方图均衡（CLAHE-like），用 Gaussian LPF 滤波控制
局域程度，通过 he_pos_wgt/he_neg_wgt（Gaussian 权重曲线）分别控制提亮/压暗强度。

参考：SS928 ISP 调优指南 §4.18；PQ 工具 §2.1.5.7。
"""

from __future__ import annotations

import torch
import torch.nn.functional as F


def _gaussian_weight(luma: torch.Tensor, wgt: torch.Tensor, sigma: torch.Tensor,
                     mean: torch.Tensor) -> torch.Tensor:
    """生成 Gaussian 形亮度权重曲线。

    Args:
        luma: (B, 1, H, W) 亮度
        wgt: (B, 1, 1) Gaussian 峰值
        sigma: (B, 1, 1) Gaussian 方差
        mean: (B, 1, 1) Gaussian 期望（亮度位置）

    Returns:
        (B, 1, H, W) 权重
    """
    # 统一到 4D：PyTorch 1.11 对 (B,1,H,W)-(B,1,1) 的 broadcasting 有 bug
    B, _, H, W = luma.shape
    w4 = wgt.view(B, 1, 1, 1)
    s4 = sigma.view(B, 1, 1, 1)
    m4 = mean.view(B, 1, 1, 1)

    luma_255 = luma * 255.0
    weight = w4 * torch.exp(-0.5 * ((luma_255 - m4) / (s4 + 1e-6)) ** 2)
    return weight / 255.0  # (B, 1, H, W)


def _differentiable_clahe(
    image: torch.Tensor, tile_size: int = 9, clip_limit: float = 2.0,
) -> torch.Tensor:
    """可微 CLAHE 近似：局部均值/标准差归一化 + 对比度拉伸。

    用 avg_pool2d 模拟分块统计，避免不可微的直方图操作。

    Args:
        image: (B, C, H, W) [0, 1]
        tile_size: 等效 9×9 窗口
        clip_limit: 对比度裁剪阈值
    Returns:
        (B, C, H, W) [0, 1] 局域均衡化结果
    """
    B, C, H, W = image.shape
    k = tile_size
    pad = k // 2

    # 局域均值
    local_mean = F.avg_pool2d(
        F.pad(image, (pad, pad, pad, pad), mode="reflect"), k, 1, 0,
    )
    # 局域标准差
    local_sq = F.avg_pool2d(
        F.pad(image * image, (pad, pad, pad, pad), mode="reflect"), k, 1, 0,
    )
    local_std = torch.sqrt((local_sq - local_mean * local_mean).clamp(min=1e-6))

    # 局域归一化 + 对比度拉伸
    normalized = (image - local_mean) / (local_std + 1e-4)
    # Clip 限制
    normalized = normalized.clamp(-clip_limit, clip_limit)
    # 重新映射到 [0, 1]
    enhanced = (normalized + clip_limit) / (2.0 * clip_limit)
    return enhanced.clamp(0.0, 1.0)


def ldci_apply(
    image: torch.Tensor,
    he_pos_wgt: torch.Tensor,   # (B, 3) [wgt, sigma, mean] in [0, 1]
    he_neg_wgt: torch.Tensor,   # (B, 3) [wgt, sigma, mean] in [0, 1]
    blc_ctrl: torch.Tensor,     # (B,) [0, 1] → HW [0, 511]
    gauss_lpf_sigma: torch.Tensor,  # (B,) [0, 1] → HW [1, 255]
) -> torch.Tensor:
    """LDCI 模块：局域直方图均衡 + 亮暗区别控制 + 暗区噪声抑制。

    Args:
        image: (B, 3, H, W) [0, 1]
        he_pos_wgt: (B, 3) 提亮 Gaussian 权重参数 [wgt, sigma, mean]
        he_neg_wgt: (B, 3) 压暗 Gaussian 权重参数 [wgt, sigma, mean]
        blc_ctrl: (B,) 暗区抑制强度
        gauss_lpf_sigma: (B,) LPF 局域程度控制

    Returns:
        (B, 3, H, W) [0, 1]
    """
    B = image.shape[0]
    # 映射参数到硬件范围
    sigma_val = (gauss_lpf_sigma.mean() * 254.0 + 1.0).clamp(1.0, 255.0).item()
    tile_size = max(3, int(sigma_val / 28.0 * 9.0))  # sigma 越小 tile 越小→越局域
    tile_size = tile_size if tile_size % 2 == 1 else tile_size + 1  # 奇数

    # CLAHE 局域均衡
    clahe_out = _differentiable_clahe(image, tile_size=tile_size)  # (B, 3, H, W)

    # 亮度权重
    luma = (0.299 * image[:, 0:1] + 0.587 * image[:, 1:2] + 0.114 * image[:, 2:3])

    # 提亮权重（he_pos_wgt：低亮度区域权重大 → 提亮暗部）
    pos_w = _gaussian_weight(
        luma,
        wgt=he_pos_wgt[:, 0].unsqueeze(1).unsqueeze(2) * 255.0 + 1.0,
        sigma=he_pos_wgt[:, 1].unsqueeze(1).unsqueeze(2) * 254.0 + 1.0,
        mean=he_pos_wgt[:, 2].unsqueeze(1).unsqueeze(2) * 255.0,
    )  # (B, 1, H, W)

    # 压暗权重（he_neg_wgt：高亮度区域权重大 → 压暗亮部）
    neg_w = _gaussian_weight(
        luma,
        wgt=he_neg_wgt[:, 0].unsqueeze(1).unsqueeze(2) * 255.0 + 1.0,
        sigma=he_neg_wgt[:, 1].unsqueeze(1).unsqueeze(2) * 254.0 + 1.0,
        mean=he_neg_wgt[:, 2].unsqueeze(1).unsqueeze(2) * 255.0,
    )  # (B, 1, H, W)

    # 融合：clahe_out 提亮暗部、image 保持亮部不变
    pos_boost = pos_w.expand(-1, 3, -1, -1) * (clahe_out - image)  # 暗区增强
    neg_suppress = neg_w.expand(-1, 3, -1, -1) * (image - clahe_out)  # 亮区抑制

    output = image + pos_boost - neg_suppress
    output = output.clamp(0.0, 1.0)

    # 暗区噪声抑制 (blc_ctrl)
    # 统一到 4D 避免 broadcasting bug
    blc = (blc_ctrl.view(B, 1, 1, 1) * 511.0).clamp(0.0, 511.0)  # (B, 1, 1, 1)
    dark_mask = (luma < 0.3).float()  # (B, 1, H, W)
    suppress = dark_mask * (blc / 511.0) * 0.5  # (B, 1, H, W)
    suppress = suppress.expand(-1, 3, -1, -1)  # (B, 3, H, W)
    output = image * suppress + output * (1.0 - suppress)

    return output.clamp(0.0, 1.0)


def ldci_identity_params(batch_size: int = 1) -> dict[str, torch.Tensor]:
    """生成恒等 LDCI 参数（直通）。"""
    return {
        # he_pos_wgt: 极小 wgt → 不提亮
        "he_pos_wgt": torch.tensor([[0.01, 0.5, 0.25]]).expand(batch_size, -1),
        # he_neg_wgt: 极小 wgt → 不压暗
        "he_neg_wgt": torch.tensor([[0.01, 0.5, 0.75]]).expand(batch_size, -1),
        "blc_ctrl": torch.zeros(batch_size),
        "gauss_lpf_sigma": torch.full((batch_size,), 0.5),  # 中等局域
    }
