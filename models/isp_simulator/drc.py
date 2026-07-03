"""可微 DRC（Dynamic Range Compression）模块。

SS928 DRC 的硬件管线：全局 Tone Mapping（200 节点 LUT） + 局域细节增强
（bilateral-filter-like 分解 + 局部混叠曲线）+ Filter/FilterX 双路亮度混合。

本可微代理将 DRC 拆为两级：
  1. 全局 Tone Mapping：NN 预测 6 个控制点 → Catmull-Rom 插值为 200 节点 → 1D LUT 施加
  2. 局域细节增强：引导滤波提取 base/detail 层 → detail 增益调制 → 回叠

参考：SS928 ISP 调优指南 §4.6；ctbg_isp_map.c 的启发式 DRC 参数映射。
"""

from __future__ import annotations

import torch
import torch.nn.functional as F


# ── 色调曲线辅助函数 ──────────────────────────────────────────

def _catmull_rom(x: torch.Tensor, cp: torch.Tensor) -> torch.Tensor:
    """Catmull-Rom 样条插值：6 控制点 → 200 节点曲线。

    Args:
        x: (200,) 等距归一化横坐标 [0, 1]
        cp: (B, 6) 控制点 y 值 [0, 1]
    Returns:
        (B, 200) 插值曲线
    """
    B = cp.shape[0]
    n_cp = 6
    # 控制点等距分布在 [0, 1]
    cp_x = torch.linspace(0.0, 1.0, n_cp, device=cp.device)  # (6,)

    # 查找每个 x 落在哪两个控制点之间
    # 对每个 B，为每个 x 找区间
    dx = cp_x[1] - cp_x[0]
    x_clamped = x.clamp(cp_x[0], cp_x[-1])
    idx_f = (x_clamped - cp_x[0]) / dx  # (200,) float indices [0, 5]
    idx0 = idx_f.long().clamp(0, n_cp - 2)  # (200,)

    t = (idx_f - idx0.float())  # (200,) 局部参数 [0, 1)

    # 取 4 个控制点做 CR 插值
    idx = torch.stack([
        (idx0 - 1).clamp(0, n_cp - 1),
        idx0,
        (idx0 + 1).clamp(0, n_cp - 1),
        (idx0 + 2).clamp(0, n_cp - 1),
    ], dim=1)  # (200, 4)

    # gather 控制点值
    cp_exp = cp[:, idx]  # (B, 200, 4)

    # Catmull-Rom 系数
    t = t.unsqueeze(0).unsqueeze(-1)  # (1, 200, 1)
    tt = t * t
    ttt = tt * t
    w = torch.cat([
        0.5 * (-t + 2.0 * tt - ttt),
        0.5 * (2.0 - 5.0 * tt + 3.0 * ttt),
        0.5 * (t + 4.0 * tt - 3.0 * ttt),
        0.5 * (-tt + ttt),
    ], dim=-1)  # (1, 200, 4)

    curve = (cp_exp * w).sum(dim=-1)  # (B, 200)
    return curve.clamp(0.0, 1.0)


def _apply_1d_lut(image: torch.Tensor, lut: torch.Tensor) -> torch.Tensor:
    """用 1D LUT 对图像做逐像素色调映射。

    Args:
        image: (B, C, H, W) [0, 1]
        lut: (B, C, N) [0, 1] 每个通道独立的 LUT
    Returns:
        (B, C, H, W) [0, 1]
    """
    B, C, H, W = image.shape
    N = lut.shape[-1]
    # 用 grid_sample: 将 LUT 放在宽度维
    lut_4d = lut.unsqueeze(2)  # (B, C, 1, N)
    grid_x = image * 2.0 - 1.0  # [0,1] → [-1,1]
    grid = torch.stack((grid_x, torch.zeros_like(grid_x)), dim=-1)  # (B, C, H, W, 2)
    grid_bc = grid.reshape(B * C, H, W, 2)
    lut_bc = lut_4d.reshape(B * C, 1, 1, N)
    out = F.grid_sample(lut_bc, grid_bc, mode="bilinear", padding_mode="border",
                        align_corners=True)
    return out.reshape(B, C, H, W)


# ── 引导滤波（简化可微版）────────────────────────────────────

def _box_filter(x: torch.Tensor, r: int) -> torch.Tensor:
    """Box mean filter（用 avg_pool2d 实现）。"""
    kernel = 2 * r + 1
    # 需要 pad 以保持尺寸
    return F.avg_pool2d(
        F.pad(x, (r, r, r, r), mode="reflect"), kernel, 1, 0,
    )


def guided_filter(img: torch.Tensor, r: int = 8, eps: float = 1e-4) -> tuple[torch.Tensor, torch.Tensor]:
    """简化引导滤波：提取 base 层和 detail 层。

    Args:
        img: (B, C, H, W) [0, 1]
        r: 滤波半径
        eps: 正则化参数
    Returns:
        base: (B, C, H, W) 平滑基底层
        detail: (B, C, H, W) 细节层 = img - base
    """
    B, C, H_img, W_img = img.shape
    # 为确保 avg_pool 可用，将 B*C 合并到 batch
    flat = img.reshape(B * C, 1, H_img, W_img)
    mean_i = _box_filter(flat, r)
    corr_i = _box_filter(flat * flat, r)
    var_i = corr_i - mean_i * mean_i
    a = var_i / (var_i + eps)
    b = mean_i - a * mean_i
    mean_a = _box_filter(a, r)
    mean_b = _box_filter(b, r)
    base = (mean_a * flat + mean_b).reshape(B, C, H_img, W_img)
    detail = img - base
    return base, detail


# ── DRC 主模块 ────────────────────────────────────────────────

def drc_tone_curve(cp: torch.Tensor, num_nodes: int = 200) -> torch.Tensor:
    """从 6 个控制点生成 DRC 色调映射曲线 (200 节点)。

    Args:
        cp: (B, 6) 控制点 [0, 1]
    Returns:
        (B, 200) 色调曲线 [0, 1]
    """
    x = torch.linspace(0.0, 1.0, num_nodes, device=cp.device)
    curve = _catmull_rom(x, cp)  # (B, 200)
    # 确保单调
    curve = torch.cummax(curve, dim=-1).values
    curve[..., 0] = 0.0
    curve[..., -1] = 1.0
    return curve


def drc_local_detail(
    image: torch.Tensor,
    bright_mix: torch.Tensor,
    dark_mix: torch.Tensor,
    spatial_coef: float = 3.0,
    range_coef: float = 5.0,
) -> torch.Tensor:
    """局域细节增强：基于引导滤波的 detail 回叠。

    Args:
        image: (B, 3, H, W) [0, 1]
        bright_mix: (B, 6) bright_x keypoints [0, 1]（映射到 [0, 128]）
        dark_mix: (B, 6) dark_x keypoints [0, 1]（映射到 [0, 128]）
        spatial_coef: 空间滤波强度 [0, 5]
        range_coef: 值域滤波强度 [0, 10]
    Returns:
        (B, 3, H, W) [0, 1] 局域增强后的图像
    """
    B, C, H, W = image.shape
    r = max(int(spatial_coef * 1.6), 1)  # spatial_coef → 滤波半径
    eps = 1e-3 / (range_coef + 1.0)  # range_coef → 正则化强度

    _, detail = guided_filter(image, r=r, eps=eps)

    # 亮度权重：亮区用 bright_mix，暗区用 dark_mix
    luma = (0.299 * image[:, 0:1] + 0.587 * image[:, 1:2] + 0.114 * image[:, 2:3])

    # 将 6 个 keypoints 插值为 33 节点（匹配硬件 local_mixing 表尺寸）
    x6 = torch.linspace(0.0, 1.0, 6, device=image.device)
    x33 = torch.linspace(0.0, 1.0, 33, device=image.device)
    bm_33 = _catmull_rom(x33, bright_mix)  # (B, 33)
    dm_33 = _catmull_rom(x33, dark_mix)    # (B, 33)

    # 按亮度查表获得增益系数（线性插值，避免复杂的 grid_sample 维度问题）
    # bm_33: (B, 33), dm_33: (B, 33), luma: (B, 1, H, W)
    B = image.shape[0]
    luma_idx = (luma.squeeze(1) * 32.0).clamp(0.0, 32.0)  # (B, H, W), float [0, 32]
    lo = luma_idx.floor().long().clamp(0, 31)  # (B, H, W)
    frac = (luma_idx - lo.float()).clamp(0.0, 1.0)  # (B, H, W)

    # 用 gather 查 LUT
    bm_lo = bm_33.gather(1, lo.reshape(B, -1)).reshape(B, H, W)  # (B, H, W)
    bm_hi = bm_33.gather(1, (lo + 1).clamp(0, 32).reshape(B, -1)).reshape(B, H, W)
    dm_lo = dm_33.gather(1, lo.reshape(B, -1)).reshape(B, H, W)
    dm_hi = dm_33.gather(1, (lo + 1).clamp(0, 32).reshape(B, -1)).reshape(B, H, W)

    bm_weight = (bm_lo * (1.0 - frac) + bm_hi * frac).unsqueeze(1)  # (B, 1, H, W)
    dm_weight = (dm_lo * (1.0 - frac) + dm_hi * frac).unsqueeze(1)

    # bright_x 增益（正）增强亮区细节，dark_x 增益（负）抑制暗区噪声
    gain = bm_weight * 0.5 + (1.0 - dm_weight) * 0.5  # (B, 1, H, W)
    gain = gain.clamp(0.0, 1.5).expand(-1, C, -1, -1)

    enhanced = image + gain * detail
    return enhanced.clamp(0.0, 1.0)


def drc_apply(
    image: torch.Tensor,
    tone_cp: torch.Tensor,
    bright_mix: torch.Tensor,
    dark_mix: torch.Tensor,
    spatial_filter: torch.Tensor,
    range_filter: torch.Tensor,
    contrast_ctrl: torch.Tensor,
    blend_luma_max: torch.Tensor,
) -> torch.Tensor:
    """完整的 DRC 模块：全局 Tone Mapping + 局域细节增强。

    Args:
        image: (B, 3, H, W) [0, 1]
        tone_cp: (B, 6) Tone Mapping 控制点 [0, 1]
        bright_mix: (B, 6) bright_x keypoints [0, 1]
        dark_mix: (B, 6) dark_x keypoints [0, 1]
        spatial_filter: (B,) 空间滤波系数 [0, 1] → HW [0, 5]
        range_filter: (B,) 值域滤波系数 [0, 1] → HW [0, 10]
        contrast_ctrl: (B,) 对比度控制 [0, 1] → HW [0, 15]
        blend_luma_max: (B,) Filter/FilterX 混合权重 [0, 1] → HW [0, 255]

    Returns:
        (B, 3, H, W) [0, 1]
    """
    B = image.shape[0]

    # 1. 全局 Tone Mapping
    tone_curve = drc_tone_curve(tone_cp)  # (B, 200)
    tone_lut = tone_curve.unsqueeze(1).expand(-1, 3, -1)  # (B, 3, 200)
    tonemapped = _apply_1d_lut(image, tone_lut)

    # 2. 局域细节增强
    sf = (spatial_filter * 5.0).clamp(0.5, 5.0).mean()  # 标量（简化）
    rf = (range_filter * 10.0).clamp(0.5, 10.0).mean()
    cc = (contrast_ctrl * 15.0).clamp(0.0, 15.0).mean()

    local_enhanced = drc_local_detail(tonemapped, bright_mix, dark_mix, sf.item(), rf.item())

    # 3. contrast_ctrl 控制全局对比度
    if cc.item() > 0:
        # 简单 S-curve 增强对比度
        strength = cc.item() / 15.0 * 0.3
        local_enhanced = local_enhanced + strength * (local_enhanced - 0.5) * (1.0 - (local_enhanced - 0.5).abs() * 2.0)
        local_enhanced = local_enhanced.clamp(0.0, 1.0)

    # 4. blend_luma_max 控制最终输出与原始输入的混合程度
    bm = blend_luma_max.mean().clamp(0.0, 1.0)
    output = local_enhanced * bm + tonemapped * (1.0 - bm)

    return output.clamp(0.0, 1.0)


def drc_identity_params(batch_size: int = 1) -> dict[str, torch.Tensor]:
    """生成恒等 DRC 参数（直通，不改图像）。"""
    return {
        "tone_cp": torch.linspace(0.0, 1.0, 6).unsqueeze(0).expand(batch_size, -1),
        "bright_mix": torch.full((batch_size, 6), 0.5),
        "dark_mix": torch.full((batch_size, 6), 0.5),
        "spatial_filter": torch.full((batch_size,), 0.6),  # 3/5
        "range_filter": torch.full((batch_size,), 0.5),     # 5/10
        "contrast_ctrl": torch.zeros(batch_size),
        "blend_luma_max": torch.full((batch_size,), 0.5),   # 128/255
    }
