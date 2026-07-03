"""完整 ISP 管线：WDR → DRC → Gamma → LDCI → Dehaze。

按 SS928 ISP 硬件管线顺序串接各可微模块，提供统一的 forward 接口。
"""

from __future__ import annotations

import torch

from models.isp_simulator.params import PARAM_TOTAL_DIM, split_params, PARAM_DIMS
from models.isp_simulator import wdr, drc, gamma, ldci, dehaze


class ISPPipeline:
    """可微 ISP 管线。

    将 NN 输出的 88 维参数向量解码后，按硬件顺序施加各模块:
        WDR → DRC → Gamma → LDCI → Dehaze

    每个模块可通过强度控制部分/完全 bypass。
    """

    def __init__(self) -> None:
        pass

    def forward(
        self,
        image: torch.Tensor,
        params: torch.Tensor,
        *,
        enable_wdr: bool = True,
        enable_drc: bool = True,
        enable_gamma: bool = True,
        enable_ldci: bool = True,
        enable_dehaze: bool = True,
    ) -> dict[str, torch.Tensor]:
        """完整 ISP 前向。

        Args:
            image: (B, 3, H, W) RGB [0, 1]
            params: (B, 88) NN 预测的归一化参数向量
            enable_*: 各模块开关（用于消融实验）

        Returns:
            字典，包含:
            - "output": (B, 3, H, W) 最终输出
            - "stages": 各阶段中间结果 dict
        """
        assert params.shape[-1] == PARAM_TOTAL_DIM, (
            f"Expected {PARAM_TOTAL_DIM} params, got {params.shape[-1]}"
        )
        B = image.shape[0]
        p = split_params(params)
        stages: dict[str, torch.Tensor] = {}
        x = image

        # Stage 1: WDR（曝光融合压缩动态范围）
        if enable_wdr:
            x = wdr.wdr_apply(x, p["wdr"])
        stages["wdr"] = x

        # Stage 2: DRC（全局 Tone Mapping + 局域细节增强）
        if enable_drc:
            tone_cp = p["drc_tone"]
            bright_mix = p["drc_mix"][..., :6]
            dark_mix = p["drc_mix"][..., 6:12]
            ctrl = p["drc_ctrl"]
            blend = p["drc_blend"]
            x = drc.drc_apply(
                x,
                tone_cp=tone_cp,
                bright_mix=bright_mix,
                dark_mix=dark_mix,
                spatial_filter=ctrl[..., 0],
                range_filter=ctrl[..., 1],
                contrast_ctrl=ctrl[..., 2],
                blend_luma_max=blend[..., 0],
            )
        stages["drc"] = x

        # Stage 3: Gamma（色调曲线）
        if enable_gamma:
            x = gamma.gamma_apply(x, p["gamma"])
        stages["gamma"] = x

        # Stage 4: LDCI（局域对比度增强）
        if enable_ldci:
            ldci_p = p["ldci"]
            x = ldci.ldci_apply(
                x,
                he_pos_wgt=ldci_p[..., 0:3],
                he_neg_wgt=ldci_p[..., 3:6],
                blc_ctrl=ldci_p[..., 6],
                gauss_lpf_sigma=ldci_p[..., 7],
            )
        stages["ldci"] = x

        # Stage 5: Dehaze（去雾 / 对比度恢复）
        if enable_dehaze:
            x = dehaze.dehaze_apply(x, p["dehaze"])
        stages["dehaze"] = x

        return {"output": x.clamp(0.0, 1.0), "stages": stages}

    def __call__(self, *args, **kwargs):
        return self.forward(*args, **kwargs)


def make_identity_params(batch_size: int = 1) -> torch.Tensor:
    """生成恒等参数向量（各模块直通，输出≈输入）。"""
    p = torch.zeros(batch_size, PARAM_TOTAL_DIM)

    # WDR: ratio=1（不融合，直通）
    p[..., 0] = 0.0

    # Gamma: 恒等曲线 Y=X
    from models.isp_simulator.params import get_offset
    off = get_offset("gamma")
    p[..., off:off + PARAM_DIMS["gamma"]] = torch.linspace(
        0.0, 1.0, PARAM_DIMS["gamma"],
    ).unsqueeze(0).expand(batch_size, -1)

    # DRC tone: 恒等直线控制点
    off = get_offset("drc_tone")
    p[..., off:off + PARAM_DIMS["drc_tone"]] = torch.linspace(
        0.0, 1.0, PARAM_DIMS["drc_tone"],
    ).unsqueeze(0).expand(batch_size, -1)

    # DRC mix: 中等值
    off = get_offset("drc_mix")
    p[..., off:off + PARAM_DIMS["drc_mix"]] = 0.5

    # DRC ctrl: 中等值
    off = get_offset("drc_ctrl")
    p[..., off:off + PARAM_DIMS["drc_ctrl"]] = 0.5

    # DRC blend: 中等混合
    off = get_offset("drc_blend")
    p[..., off:off + PARAM_DIMS["drc_blend"]] = 0.5

    # LDCI: 近乎恒等（极小 wgt）
    off = get_offset("ldci")
    p[..., off:off + 3] = torch.tensor([0.01, 0.5, 0.25])  # he_pos
    p[..., off + 3:off + 6] = torch.tensor([0.01, 0.5, 0.75])  # he_neg
    p[..., off + 6] = 0.0   # blc_ctrl
    p[..., off + 7] = 0.5   # gauss_lpf_sigma

    # Dehaze: 强度=0（不去雾）
    off = get_offset("dehaze")
    p[..., off] = 0.0

    return p
