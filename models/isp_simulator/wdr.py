"""可微 WDR（Wide Dynamic Range）模块——多曝光融合代理。

SS928 WDR 硬件行为：传感器交替输出长/短曝光帧，ISP 做运动检测+融合。
长曝光捕获暗部细节，短曝光保留亮部信息。曝光比（ratio_x64）控制长/短比。

可微代理：从单张输入模拟两帧曝光，用亮度引导的加权融合模拟 WDR 效果。
这不模拟时序/运动检测，仅模拟曝光融合的色调压缩效果。

参考：SS928 ISP 调优指南 §4.7；PQ 工具 §4.2.4。
"""

from __future__ import annotations

import torch


def wdr_apply(
    image: torch.Tensor,
    exposure_ratio: torch.Tensor,  # (B,) [0, 1] → HW [1, 64]
) -> torch.Tensor:
    """WDR 多曝光融合代理。

    从单图模拟短/长两条曝光路径，按亮度融合。

    Args:
        image: (B, 3, H, W) [0, 1]
        exposure_ratio: (B,) [0, 1] → 映射到 [1, 64] ×64

    Returns:
        (B, 3, H, W) [0, 1] WDR 融合结果
    """
    B = image.shape[0]

    # 映射到硬件范围：ratio_x64 ∈ [1, 64]
    ratio = (exposure_ratio * 63.0 + 1.0).clamp(1.0, 64.0)  # (B,)
    ratio = ratio.view(B, 1, 1, 1)

    # 模拟曝光比 → gamma 等效
    # 长曝光 = 提亮暗部（gamma < 1），短曝光 = 压暗亮部（gamma > 1）
    # 对数域：ratio=R 表示长曝光时间是短曝光的 R 倍
    # 用 gamma 建模：短曝光 = x^(1/R), 长曝光 = x^R
    # 但此处用更直观的方式：
    # 短曝光：整体变暗，亮部保留细节
    gamma_short = ratio  # > 1，压制高光
    # 长曝光：整体变亮，暗部细节显现
    gamma_long = 1.0 / (ratio + 1e-6)  # < 1，提亮暗部

    # 同时限制极端值
    gamma_short = gamma_short.clamp(0.5, 4.0)
    gamma_long = gamma_long.clamp(0.25, 2.0)

    short_exposure = image.pow(gamma_short)
    long_exposure = image.pow(gamma_long)

    # 亮度引导融合：亮区信任短曝光，暗区信任长曝光
    luma = (0.299 * image[:, 0:1] + 0.587 * image[:, 1:2] + 0.114 * image[:, 2:3])

    # 融合权重：亮区 → short weight=1，暗区 → long weight=1
    # 用 sigmoid 平滑过渡
    w_short = torch.sigmoid((luma - 0.5) * 8.0)  # (B, 1, H, W)

    fused = w_short * short_exposure + (1.0 - w_short) * long_exposure

    # 强度控制：ratio 越接近 1 表示 WDR 效果越弱
    wdr_strength = ((ratio.squeeze() - 1.0) / 63.0).view(B, 1, 1, 1).clamp(0.0, 1.0)
    output = image + wdr_strength * (fused - image)

    return output.clamp(0.0, 1.0)
