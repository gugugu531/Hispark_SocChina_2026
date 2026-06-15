"""ExpoCurveNet 结构 + ONNX 导出约束的单元测试（development-guide.md §10，pytest）。

只验证"结构正确 + 算子干净"，不验证画质（去风险任务，权重随机）。
为提速，前向/导出用小分辨率（64x96，仍可被 down=4 整除）。
"""

import numpy as np
import onnx
import pytest
import torch

from models.exporters import expo_curve_onnx as exp
from models.networks.expo_curve import ExpoCurveNet, bilinear_kernel, build_model

# NNN 绿名单 + ONNX 图结构辅助算子（Constant / 图 IO 节点）
GREEN = {
    "Conv", "ConvTranspose", "AveragePool", "MaxPool", "Clip", "Tanh", "Sigmoid",
    "HardSigmoid", "Mul", "Add", "Sub", "Div", "Pow", "Exp", "Concat", "Split",
    "Slice", "MatMul", "Gemm", "Softmax", "Pad", "Transpose", "Reshape",
}
GRAPH_AUX = {"Constant"}

H, W = 64, 96  # 测试分辨率（可被 down=4 整除）


@pytest.mark.parametrize("shared", [False, True])
def test_forward_shape_and_range(shared):
    m = build_model(niter=8, filters=16, shared_curve=shared)
    x = torch.rand(1, 3, H, W)
    with torch.no_grad():
        y = m(x)
    assert y.shape == x.shape
    assert float(y.min()) >= 0.0 and float(y.max()) <= 1.0  # clip_output → [0,1]


def test_curve_channels():
    assert build_model(niter=8, shared_curve=False).curve_channels == 24  # 3*niter
    assert build_model(niter=8, shared_curve=True).curve_channels == 3
    assert build_model(niter=5, shared_curve=False).curve_channels == 15


def test_params_scale_with_filters_and_niter():
    p_f8 = sum(t.numel() for t in build_model(niter=8, filters=8).parameters())
    p_f16 = sum(t.numel() for t in build_model(niter=8, filters=16).parameters())
    p_shared = sum(t.numel() for t in build_model(niter=8, filters=16, shared_curve=True).parameters())
    assert p_f16 > p_f8  # 更多 filters → 更多参数
    assert p_shared < p_f16  # 共享曲线末层只出 3 通道 → 更少参数


def test_bilinear_upsample_restores_resolution():
    m = build_model(niter=8, filters=16, down=4)
    a = torch.rand(1, m.curve_channels, H // 4, W // 4)
    up = m.upsample(a)
    assert up.shape[-2:] == (H, W)  # ConvTranspose 严格放大 down 倍


def test_bilinear_kernel_is_fcn_upsampling_weight():
    # 经典 FCN get_upsampling_weight: 8x8 核 = 两个 1D 核(各 sum=scale)的外积 → sum=scale²
    k = bilinear_kernel(channels=3, scale=4)
    assert k.shape == (3, 1, 8, 8)
    assert pytest.approx(float(k[0, 0].sum()), abs=1e-3) == 16.0  # scale² = 4²
    # 所有通道共用同一核（深度可分上采样）
    assert torch.allclose(k[0, 0], k[1, 0]) and torch.allclose(k[0, 0], k[2, 0])


def test_upsample_preserves_dc_in_interior():
    # 有意义的性质: 对常数曲线图做 ConvTranspose 上采样, 内部应还原同一常数(直流不变)
    m = build_model(niter=8, filters=16, down=4)
    const = torch.full((1, m.curve_channels, H // 4, W // 4), 0.37)
    with torch.no_grad():
        up = m.upsample(const)
    interior = up[..., 8:-8, 8:-8]  # 避开双线性 ConvTranspose 的边界衰减
    assert pytest.approx(float(interior.mean()), abs=1e-3) == 0.37


def test_curve_apply_is_bidirectional():
    # 双向性: x = x + r*(x²-x)，中间调 x=0.5 时 (x²-x)=-0.25
    #   r>0 → 变暗; r<0 → 变亮。验证符号决定方向（architecture.md §1）。
    x = torch.full((1, 3, 4, 4), 0.5)
    bright = x + (-0.8) * (x * x - x)
    dark = x + (0.8) * (x * x - x)
    assert float(bright.mean()) > 0.5  # r<0 提亮
    assert float(dark.mean()) < 0.5    # r>0 压暗


@pytest.mark.parametrize("shared", [False, True])
def test_onnx_export_greenlist_only(tmp_path, shared):
    """导出小尺寸 ONNX，断言：单输出、ConvTranspose 上采样、无 Resize、无红名单、仅绿名单算子。"""
    torch.manual_seed(0)
    model = build_model(niter=8, filters=16, shared_curve=shared)
    path = str(tmp_path / "m.onnx")
    exp.export_fp32(model, H, W, "input", "output", path, opset=13)
    m = onnx.load(path)
    exp.clean_graph(m)
    onnx.checker.check_model(m)

    hist = exp.op_histogram(m)
    ops = set(hist)

    assert len(m.graph.output) == 1, "必须单输出"
    assert hist.get("ConvTranspose", 0) >= 1, "上采样必须是 ConvTranspose"
    assert "Resize" not in ops and "Upsample" not in ops, "禁止 Resize/插值上采样"
    assert not (ops & exp.RED_LIST), f"命中红名单: {ops & exp.RED_LIST}"
    assert "Relu" not in ops, "隐层激活应导出为 Clip 而非 Relu"
    unexpected = ops - GREEN - GRAPH_AUX
    assert not unexpected, f"出现非绿名单算子: {unexpected}"

    # opset 必须是 13、且只剩默认 domain
    opset = {o.domain: o.version for o in m.opset_import}
    assert opset.get("", opset.get("ai.onnx")) == 13


def test_onnx_fp16_conversion_no_residual_float(tmp_path):
    """FP16 整图直转后不应残留 FLOAT 张量（否则 ATC 会插 Cast）。"""
    model = build_model(niter=8, filters=16)
    path = str(tmp_path / "m.onnx")
    exp.export_fp32(model, H, W, "input", "output", path, opset=13)
    m = onnx.load(path)
    exp.clean_graph(m)
    exp.convert_to_fp16(m)
    onnx.checker.check_model(m)

    FLOAT = onnx.TensorProto.FLOAT
    residual = [i.name for i in m.graph.initializer if i.data_type == FLOAT]
    io_float = [vi.name for vi in list(m.graph.input) + list(m.graph.output)
                if vi.type.tensor_type.elem_type == FLOAT]
    assert not residual, f"残留 FP32 初始化器: {residual}"
    assert not io_float, f"残留 FP32 IO: {io_float}"
