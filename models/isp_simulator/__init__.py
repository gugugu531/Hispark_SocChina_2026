"""可微 ISP 模拟器（Differentiable ISP Simulator）。

SS928 ISP 管线 (WDR → DRC → Gamma → LDCI → Dehaze) 的 PyTorch 可微代理实现。
用于 ISP 参数自动调优（Phase 1）：训练 NN 预测 ISP 参数，在模拟器中施加，
Loss 对比 GT 进行监督学习。

模块:
    - params: 88 维参数空间定义与编解码
    - wdr: 多曝光融合代理
    - drc: 动态范围压缩（Tone Mapping + 局部细节增强）
    - gamma: 1D LUT 色调映射
    - ldci: 局域对比度增强
    - dehaze: 暗通道先验去雾
    - pipeline: 完整 ISP 管线

用法::

    from models.isp_simulator import ISPPipeline, make_identity_params
    import torch

    pipeline = ISPPipeline()
    image = torch.rand(2, 3, 512, 288)
    params = make_identity_params(2)  # 恒等参数
    result = pipeline(image, params)
    print(result["output"].shape)  # (2, 3, 512, 288)
"""

from models.isp_simulator.params import (
    PARAM_TOTAL_DIM,
    PARAM_DIMS,
    split_params,
    get_offset,
)
from models.isp_simulator.pipeline import ISPPipeline, make_identity_params
from models.isp_simulator import wdr, drc, gamma, ldci, dehaze

__all__ = [
    "PARAM_TOTAL_DIM",
    "PARAM_DIMS",
    "split_params",
    "get_offset",
    "ISPPipeline",
    "make_identity_params",
    "wdr",
    "drc",
    "gamma",
    "ldci",
    "dehaze",
]
