"""SS928 ISP 参数空间定义与编解码。

NN 输出一个 96 维连续向量，本模块将其映射到各 ISP 硬件块的参数结构。
所有值域均为归一化 [0,1]，由各模块内部映射到硬件实际范围。

参数布局（共 96 维）::

    [0]         WDR 曝光比 ×64 归一化
    [1:65]      Gamma 曲线 64 节点
    [65:71]     DRC tone 6 控制点
    [71:83]     DRC mix 12 (bright_x 6 + dark_x 6 keypoints)
    [83:86]     DRC ctrl 3 (spatial_filter, range_filter, contrast_ctrl)
    [86:87]     DRC blend 1 (luma_max)
    [87:95]     LDCI 8 (he_pos 3 + he_neg 3 + blc_ctrl + gauss_lpf_sigma)
    [95]        Dehaze 1
"""

from __future__ import annotations

import torch

# ── 参数向量维度 ──────────────────────────────────────────────
DIM_WDR = 1
DIM_GAMMA = 64
DIM_DRC_TONE = 6
DIM_DRC_MIX = 12  # 6 bright_x keypoints + 6 dark_x keypoints
DIM_DRC_CTRL = 3  # spatial_filter, range_filter, contrast_ctrl
DIM_DRC_BLEND = 1  # blend_luma_max (其余 blend 参数由启发式派生)
DIM_LDCI = 8  # he_pos(3) + he_neg(3) + blc_ctrl + gauss_lpf_sigma
DIM_DEHAZE = 1

PARAM_DIMS = {
    "wdr": DIM_WDR,
    "gamma": DIM_GAMMA,
    "drc_tone": DIM_DRC_TONE,
    "drc_mix": DIM_DRC_MIX,
    "drc_ctrl": DIM_DRC_CTRL,
    "drc_blend": DIM_DRC_BLEND,
    "ldci": DIM_LDCI,
    "dehaze": DIM_DEHAZE,
}

# 累积偏移量，用于从 flat vector 切片
_offsets: dict[str, int] = {}
_pos = 0
for key, dim in PARAM_DIMS.items():
    _offsets[key] = _pos
    _pos += dim

PARAM_TOTAL_DIM = _pos  # 88


def split_params(params: torch.Tensor) -> dict[str, torch.Tensor]:
    """将 (..., 88) 参数向量拆分为各模块参数字典。"""
    assert params.shape[-1] == PARAM_TOTAL_DIM, (
        f"Expected {PARAM_TOTAL_DIM} params, got {params.shape[-1]}"
    )
    return {key: params[..., _offsets[key]: _offsets[key] + dim]
            for key, dim in PARAM_DIMS.items()}


def get_offset(key: str) -> int:
    """返回某模块参数在 flat vector 中的起始索引。"""
    return _offsets[key]
