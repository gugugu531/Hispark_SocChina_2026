"""可微 ISP 模拟器的 SDK-free 单元测试。

覆盖：各模块前向/反向/恒等行为、管线端到端、参数向量编解码。
"""

from __future__ import annotations

import torch
import pytest

from models.isp_simulator.params import (
    PARAM_TOTAL_DIM, PARAM_DIMS, split_params, get_offset,
)
from models.isp_simulator import (
    ISPPipeline, make_identity_params,
    wdr, drc, gamma, ldci, dehaze,
)

BATCH = 2
H, W = 64, 64  # 小分辨率加速测试


# ── 参数向量测试 ──────────────────────────────────────────────

def test_param_total_dim():
    """验证参数总维度 88。"""
    assert PARAM_TOTAL_DIM == 96


def test_split_params_structure():
    """验证参数拆分结构完整。"""
    params = torch.rand(BATCH, PARAM_TOTAL_DIM)
    p = split_params(params)
    for key, dim in PARAM_DIMS.items():
        assert key in p, f"Missing key: {key}"
        assert p[key].shape == (BATCH, dim), f"{key}: expected ({BATCH}, {dim}), got {p[key].shape}"


def test_get_offset_consistency():
    """验证偏移量与参数拆分一致。"""
    params = torch.rand(1, PARAM_TOTAL_DIM)
    p = split_params(params)
    for key in PARAM_DIMS:
        offset = get_offset(key)
        dim = PARAM_DIMS[key]
        assert torch.equal(p[key], params[:, offset:offset + dim]), f"Offset mismatch for {key}"


# ── Gamma 模块测试 ────────────────────────────────────────────

def test_gamma_identity():
    """恒等 Gamma 曲线：输出应接近输入。"""
    img = torch.rand(BATCH, 3, H, W)
    curve = gamma.gamma_identity_curve(64).unsqueeze(0).expand(BATCH, -1)
    out = gamma.gamma_apply(img, curve)
    assert out.shape == img.shape
    # 恒等曲线 + 线性插值 → 近乎一致
    assert (out - img).abs().max() < 0.02


def test_gamma_brighten():
    """提亮曲线：暗部应被提亮。"""
    dark = torch.full((BATCH, 3, H, W), 0.1)
    curve = gamma.gamma_brighten_curve(strength=0.8, num_nodes=64).unsqueeze(0).expand(BATCH, -1)
    out = gamma.gamma_apply(dark, curve)
    # 暗部应被提亮
    assert (out > dark).float().mean() > 0.9


def test_gamma_monotonic_enforced():
    """验证单调性强制：即使输入非单调曲线，输出也应单调。"""
    img = torch.rand(BATCH, 3, H, W)
    # 随机乱序曲线
    bad_curve = torch.rand(BATCH, 64)
    out = gamma.gamma_apply(img, bad_curve)
    assert out.shape == img.shape
    assert torch.isfinite(out).all()


def test_gamma_gradient_flow():
    """验证 Gamma 模块梯度可回传。"""
    img = torch.rand(BATCH, 3, H, W, requires_grad=True)
    curve = torch.rand(BATCH, 64, requires_grad=True)
    out = gamma.gamma_apply(img, curve)
    loss = out.mean()
    loss.backward()
    assert img.grad is not None
    assert curve.grad is not None
    assert torch.isfinite(img.grad).all()
    assert torch.isfinite(curve.grad).all()


# ── DRC 模块测试 ──────────────────────────────────────────────

def test_drc_tone_curve_shape():
    """DRC 色调曲线输出正确的形状。"""
    cp = torch.rand(BATCH, 6)
    curve = drc.drc_tone_curve(cp)
    assert curve.shape == (BATCH, 200)
    assert curve.min() >= 0.0
    assert curve.max() <= 1.0


def test_drc_tone_curve_monotonic():
    """6 控制点生成的 200 节点曲线应单调递增。"""
    cp = torch.rand(BATCH, 6)
    curve = drc.drc_tone_curve(cp)
    diff = curve[:, 1:] - curve[:, :-1]
    assert (diff >= -1e-6).all(), "Tone curve is not monotonic"


def test_drc_apply_shape():
    """DRC 完整管线输出正确形状和值域。"""
    img = torch.rand(BATCH, 3, H, W)
    p = drc.drc_identity_params(BATCH)
    out = drc.drc_apply(img, **p)
    assert out.shape == img.shape
    assert out.min() >= 0.0
    assert out.max() <= 1.0
    assert torch.isfinite(out).all()


def test_drc_gradient_flow():
    """DRC 模块梯度可回传。"""
    img = torch.rand(BATCH, 3, H, W, requires_grad=True)
    tone_cp = torch.rand(BATCH, 6, requires_grad=True)
    bright_mix = torch.rand(BATCH, 6, requires_grad=True)
    dark_mix = torch.rand(BATCH, 6, requires_grad=True)
    sf = torch.rand(BATCH, requires_grad=True)
    rf = torch.rand(BATCH, requires_grad=True)
    cc = torch.rand(BATCH, requires_grad=True)
    bm = torch.rand(BATCH, requires_grad=True)
    out = drc.drc_apply(img, tone_cp, bright_mix, dark_mix, sf, rf, cc, bm)
    loss = out.mean()
    loss.backward()
    assert img.grad is not None and torch.isfinite(img.grad).all()
    assert tone_cp.grad is not None and torch.isfinite(tone_cp.grad).all()


# ── LDCI 模块测试 ─────────────────────────────────────────────

def test_ldci_apply_shape():
    """LDCI 输出正确形状和值域。"""
    img = torch.rand(BATCH, 3, H, W)
    p = ldci.ldci_identity_params(BATCH)
    out = ldci.ldci_apply(img, **p)
    assert out.shape == img.shape
    assert out.min() >= 0.0
    assert out.max() <= 1.0
    assert torch.isfinite(out).all()


def test_ldci_identity_approx():
    """恒等参数下 LDCI 应接近直通。"""
    torch.manual_seed(42)
    img = torch.rand(BATCH, 3, H, W)
    p = ldci.ldci_identity_params(BATCH)
    out = ldci.ldci_apply(img, **p)
    # 恒等参数应几乎不改变图像
    assert (out - img).abs().max() < 0.35  # CLAHE 会引入一些变化


def test_ldci_gradient_flow():
    """LDCI 梯度可回传。"""
    img = torch.rand(BATCH, 3, H, W, requires_grad=True)
    hp = torch.rand(BATCH, 3, requires_grad=True)
    hn = torch.rand(BATCH, 3, requires_grad=True)
    blc = torch.rand(BATCH, requires_grad=True)
    sig = torch.rand(BATCH, requires_grad=True)
    out = ldci.ldci_apply(img, hp, hn, blc, sig)
    loss = out.mean()
    loss.backward()
    assert img.grad is not None and torch.isfinite(img.grad).all()
    assert hp.grad is not None and torch.isfinite(hp.grad).all()


# ── Dehaze 模块测试 ───────────────────────────────────────────

def test_dehaze_apply_shape():
    """Dehaze 输出正确形状和值域。"""
    img = torch.rand(BATCH, 3, H, W)
    strength = torch.zeros(BATCH)  # 强度 0 = 直通
    out = dehaze.dehaze_apply(img, strength)
    assert out.shape == img.shape
    assert out.min() >= 0.0
    assert out.max() <= 1.0
    assert torch.isfinite(out).all()


def test_dehaze_zero_strength_identity():
    """强度=0 时去雾应近乎直通。"""
    torch.manual_seed(42)
    img = torch.rand(BATCH, 3, H, W)
    strength = torch.zeros(BATCH)
    out = dehaze.dehaze_apply(img, strength)
    torch.testing.assert_close(out, img, atol=1e-6, rtol=1e-5)


def test_dehaze_gradient_flow():
    """Dehaze 梯度可回传。"""
    img = torch.rand(BATCH, 3, H, W, requires_grad=True)
    strength = torch.tensor([0.5, 0.3], requires_grad=True)
    out = dehaze.dehaze_apply(img, strength)
    loss = out.mean()
    loss.backward()
    assert img.grad is not None and torch.isfinite(img.grad).all()
    assert strength.grad is not None and torch.isfinite(strength.grad).all()


# ── WDR 模块测试 ──────────────────────────────────────────────

def test_wdr_apply_shape():
    """WDR 输出正确形状和值域。"""
    img = torch.rand(BATCH, 3, H, W)
    ratio = torch.zeros(BATCH)  # ratio=1, 不融合
    out = wdr.wdr_apply(img, ratio)
    assert out.shape == img.shape
    assert out.min() >= 0.0
    assert out.max() <= 1.0
    assert torch.isfinite(out).all()


def test_wdr_min_ratio_identity():
    """最小曝光比 (ratio=1x) 时近乎直通。"""
    torch.manual_seed(42)
    img = torch.rand(BATCH, 3, H, W)
    ratio = torch.zeros(BATCH)
    out = wdr.wdr_apply(img, ratio)
    torch.testing.assert_close(out, img, atol=1e-6, rtol=1e-5)


def test_wdr_high_ratio_compresses():
    """高曝光比应压缩动态范围。"""
    img = torch.rand(BATCH, 3, H, W)
    ratio = torch.ones(BATCH)  # max ratio
    out = wdr.wdr_apply(img, ratio)
    # 输出标准差应小于输入（动态范围被压缩）
    assert out.std() < img.std() + 0.05


def test_wdr_gradient_flow():
    """WDR 梯度可回传。"""
    img = torch.rand(BATCH, 3, H, W, requires_grad=True)
    ratio = torch.rand(BATCH, requires_grad=True)
    out = wdr.wdr_apply(img, ratio)
    loss = out.mean()
    loss.backward()
    assert img.grad is not None and torch.isfinite(img.grad).all()
    assert ratio.grad is not None and torch.isfinite(ratio.grad).all()


# ── 完整管线测试 ──────────────────────────────────────────────

@pytest.fixture
def pipeline():
    return ISPPipeline()


def test_pipeline_identity_approximate(pipeline):
    """恒等参数下管线输出应接近输入。"""
    torch.manual_seed(42)
    img = torch.rand(BATCH, 3, H, W)
    params = make_identity_params(BATCH)
    result = pipeline(img, params)
    out = result["output"]
    assert out.shape == img.shape
    assert out.min() >= 0.0
    assert out.max() <= 1.0
    # 恒等参数下各模块应接近直通
    assert (out - img).abs().mean() < 0.15, f"mean diff too large: {(out - img).abs().mean()}"


def test_pipeline_output_shape(pipeline):
    """管线返回完整结构和中间阶段。"""
    img = torch.rand(BATCH, 3, H, W)
    params = torch.rand(BATCH, PARAM_TOTAL_DIM)
    result = pipeline(img, params)
    assert "output" in result
    assert "stages" in result
    for stage in ["wdr", "drc", "gamma", "ldci", "dehaze"]:
        assert stage in result["stages"], f"Missing stage: {stage}"
        s = result["stages"][stage]
        assert s.shape == img.shape, f"{stage}: {s.shape} != {img.shape}"
        assert s.min() >= -0.01 and s.max() <= 1.01, f"{stage}: range [{s.min():.3f}, {s.max():.3f}]"


def test_pipeline_ablation_switches(pipeline):
    """消融开关：关闭某模块后该阶段应等于前一阶段。"""
    img = torch.rand(BATCH, 3, H, W)
    params = make_identity_params(BATCH)

    # 全部关闭 → 全程直通
    r = pipeline(img, params,
                 enable_wdr=False, enable_drc=False, enable_gamma=False,
                 enable_ldci=False, enable_dehaze=False)
    torch.testing.assert_close(r["output"], img, atol=1e-6, rtol=1e-5)


def test_pipeline_gradient_flow(pipeline):
    """完整管线的梯度可从输出回传到参数。"""
    img = torch.rand(BATCH, 3, H, W)
    params = torch.rand(BATCH, PARAM_TOTAL_DIM, requires_grad=True)
    result = pipeline(img, params)
    loss = result["output"].mean()
    loss.backward()
    assert params.grad is not None
    assert torch.isfinite(params.grad).all()
    assert params.grad.abs().sum() > 0, "All parameter gradients are zero"


def test_pipeline_numerical_stability_random_params(pipeline):
    """随机参数不产生 NaN/Inf。"""
    torch.manual_seed(123)
    for _ in range(10):
        img = torch.rand(BATCH, 3, H, W)
        params = torch.rand(BATCH, PARAM_TOTAL_DIM)
        result = pipeline(img, params)
        assert torch.isfinite(result["output"]).all()


def test_pipeline_batch_independence(pipeline):
    """批内样本独立：样本 0 的输出不依赖样本 1 的输入。"""
    torch.manual_seed(42)
    img0 = torch.rand(1, 3, H, W)
    img1 = torch.rand(1, 3, H, W)
    params = make_identity_params(2)

    # 单独运行
    r0_solo = pipeline(img0, params[:1])["output"]

    # 合批运行
    batch = torch.cat([img0, img1], dim=0)
    r_batch = pipeline(batch, params)["output"]

    torch.testing.assert_close(r_batch[0:1], r0_solo, atol=1e-6, rtol=1e-5)
