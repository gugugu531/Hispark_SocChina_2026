"""ISP 参数 blob 生成器——将模拟器参数编码为板端 isp_load_blob_and_apply() 可读取的二进制格式。

Blob 格式 (little-endian):
    0-3:   magic = 0x49535000
    4-7:   version = 1
    8-11:  flags (bit0=DRC, bit1=LDCI)
    -- DRC (if flags & 1) --
    12-15: drc_enable (uint32)
    16-415: drc_tone_mapping_value[200] (uint16 × 200)
    416-448: drc_bright_x[33] (uint8 × 33)
    449-481: drc_dark_x[33] (uint8 × 33)
    482: drc_spatial_filter (uint8)
    483: drc_range_filter (uint8)
    484: drc_contrast_ctrl (uint8)
    485: drc_blend_luma_max (uint8)
    486-487: drc_manual_strength (uint16, 0-1023; v2 起) —— 独立于 tone 曲线的
             整体亮度维度(SDK: 值越大越亮), v1 板端默认 512
    -- LDCI (if flags & 2, 偏移随 DRC 段版本顺延) --
    ldci_enable (uint32)
    ldci_he_pos_wgt[3] (uint8 × 3)
    ldci_he_neg_wgt[3] (uint8 × 3)
    ldci_blc_ctrl (uint16)
    ldci_gauss_lpf_sigma (uint8)
"""

from __future__ import annotations

import struct
import torch
import numpy as np
from pathlib import Path


def sim_params_to_blob(params: torch.Tensor, enable_drc: bool = True,
                       enable_ldci: bool = True, drc_on: bool = True,
                       ldci_on: bool = True,
                       drc_strength: int | None = None,
                       gamma_on: bool = False, gamma_strength: float = 1.0,
                       guard: dict | None = None,
                       color: dict | None = None) -> bytes:
    """将模拟器 97 维参数向量编码为 ISP blob 二进制（v3）。

    Args:
        params: (1, 97) 或 (97,) 模拟器参数，值域 [0, 1]
        enable_drc: blob 是否携带 DRC 段（不携带 = 板端不动该模块）
        enable_ldci: blob 是否携带 LDCI 段
        drc_on: DRC 段 enable 字段值（False = 板端显式关闭 DRC，用于采中性帧）
        ldci_on: LDCI 段 enable 字段值
        drc_strength: DRC manual strength 0-1023 显式覆盖；None = 从参数向量
            drc_strength 维读取（×1023）。模拟器以 drc_strength_apply 建模该维度。
        gamma_on: 携带 Gamma 段（曲线取参数向量 gamma 64 维，板端经
            isp_gamma_apply_curve 施加）
        gamma_strength: Gamma 叠加强度 [0,1]
        guard: DRC 护栏子段（高光抑制直控），dict:
            bright_gain_limit/bright_gain_limit_step [0,15]、
            dark_gain_limit_luma/dark_gain_limit_chroma [0,133]；None = 不携带
        color: DRC 色彩补偿子段，dict: ctrl(bool)、low_sat/high_sat [0,15]、
            lut(33 个 [0,1024]，None = 全 1024 中性)；None = 不携带

    Returns:
        bytes: ISP blob 二进制数据
    """
    from models.isp_simulator.params import split_params
    from models.isp_simulator.drc import drc_tone_curve

    if params.dim() == 1:
        params = params.unsqueeze(0)
    p = split_params(params)
    buf = bytearray()
    flags = 0
    if enable_drc: flags |= 1
    if enable_ldci: flags |= 2
    if gamma_on: flags |= 4
    if enable_drc and guard is not None: flags |= 8
    if enable_drc and color is not None: flags |= 16

    # Header
    buf += struct.pack('<III', 0x49535000, 3, flags)

    # DRC
    if enable_drc:
        buf += struct.pack('<I', 1 if drc_on else 0)  # drc_enable

        # Tone curve: 6 控制点 → 200 节点 [0, 65535]
        tone_cp = p["drc_tone"]  # (1, 6)
        tone_200 = drc_tone_curve(tone_cp).squeeze(0)  # (200,) [0, 1]
        tmv = (tone_200 * 65535.0).clamp(0, 65535).round().to(torch.int32).tolist()
        for v in tmv:
            buf += struct.pack('<H', int(v))

        # Local mixing: 6 keypoints → 33 nodes [0, 128]
        bright_cp = p["drc_mix"][0, :6]  # (6,)
        dark_cp = p["drc_mix"][0, 6:12]  # (6,)
        # 线性插值 6→33
        x6 = torch.linspace(0, 1, 6)
        x33 = torch.linspace(0, 1, 33)
        bx33 = np.interp(x33.numpy(), x6.numpy(), bright_cp.numpy()) * 128.0
        dx33 = np.interp(x33.numpy(), x6.numpy(), dark_cp.numpy()) * 128.0
        for v in bx33.clip(0, 128).astype(np.uint8):
            buf += struct.pack('B', int(v))
        for v in dx33.clip(0, 128).astype(np.uint8):
            buf += struct.pack('B', int(v))

        # Scalar controls
        sf = int(p["drc_ctrl"][0, 0].item() * 5.0 + 0.5)
        rf = int(p["drc_ctrl"][0, 1].item() * 10.0 + 0.5)
        cc = int(p["drc_ctrl"][0, 2].item() * 15.0 + 0.5)
        bm = int(p["drc_blend"][0, 0].item() * 255.0 + 0.5)
        buf += struct.pack('BBBB',
                           max(0, min(5, sf)),
                           max(0, min(10, rf)),
                           max(0, min(15, cc)),
                           max(0, min(255, bm)))
        strength = (int(p["drc_strength"][0, 0].item() * 1023.0 + 0.5)
                    if drc_strength is None else int(drc_strength))
        buf += struct.pack('<H', max(0, min(1023, strength)))

        # v3: Filter 主通路 local mixing（与 FilterX 通路同值——模拟器单路 mixing
        # 对应硬件双路；v2 只写 X 通路，blend 偏主通路时无效应）
        for v in bx33.clip(0, 128).astype(np.uint8):
            buf += struct.pack('B', int(v))
        for v in dx33.clip(0, 128).astype(np.uint8):
            buf += struct.pack('B', int(v))

        # v3 可选子段：护栏（bit3）
        if guard is not None:
            buf += struct.pack('BBBB',
                               max(0, min(15, int(guard.get("bright_gain_limit", 15)))),
                               max(0, min(15, int(guard.get("bright_gain_limit_step", 0)))),
                               max(0, min(133, int(guard.get("dark_gain_limit_luma", 133)))),
                               max(0, min(133, int(guard.get("dark_gain_limit_chroma", 133)))))

        # v3 可选子段：色彩补偿（bit4）
        if color is not None:
            buf += struct.pack('BBB',
                               1 if color.get("ctrl", True) else 0,
                               max(0, min(15, int(color.get("low_sat", 8)))),
                               max(0, min(15, int(color.get("high_sat", 8)))))
            lut = color.get("lut")
            if lut is None:
                lut = [1024] * 33
            for v in lut:
                buf += struct.pack('<H', max(0, min(1024, int(v))))

    # LDCI
    if enable_ldci:
        buf += struct.pack('<I', 1 if ldci_on else 0)  # ldci_enable
        ld = p["ldci"]
        hp = (ld[0, 0:3] * 255.0).clamp(0, 255).round().to(torch.uint8).tolist()
        hn = (ld[0, 3:6] * 255.0).clamp(0, 255).round().to(torch.uint8).tolist()
        blc = int(ld[0, 6].item() * 511.0 + 0.5)
        sigma = int(ld[0, 7].item() * 254.0 + 1.0 + 0.5)
        for v in hp:
            buf += struct.pack('B', int(v))
        for v in hn:
            buf += struct.pack('B', int(v))
        buf += struct.pack('<H', max(0, min(511, blc)))
        buf += struct.pack('B', max(1, min(255, sigma)))

    # v3: Gamma 段（bit2，位于 LDCI 段之后）
    if gamma_on:
        curve = (p["gamma"][0] * 65535.0).clamp(0, 65535).round().to(torch.int32).tolist()
        for v in curve:
            buf += struct.pack('<H', int(v))
        buf += struct.pack('<H', max(0, min(1024, int(gamma_strength * 1024.0 + 0.5))))

    return bytes(buf)


def test_roundtrip():
    """验证 blob 生成（v3 各段长度）。"""
    from models.isp_simulator import make_identity_params
    params = make_identity_params(1)
    blob = sim_params_to_blob(params, enable_drc=True, enable_ldci=True)
    print(f"Identity blob: {len(blob)} bytes, magic={blob[:4].hex()}")
    # v3 基础 = v2(501) + 主通路 mixing 66
    assert len(blob) == 567, f"Expected 567 bytes (v3), got {len(blob)}"

    params2 = make_identity_params(1)
    params2[0, 65:71] = torch.tensor([0.0, 0.2, 0.5, 0.5, 0.8, 1.0])  # S-curve
    blob2 = sim_params_to_blob(params2, drc_strength=0, gamma_on=True,
                               guard={"bright_gain_limit": 8},
                               color={"low_sat": 10})
    # + guard 4 + color 3+66 + gamma 128+2
    assert len(blob2) == 567 + 4 + 69 + 130, f"got {len(blob2)}"
    print("Roundtrip OK")


if __name__ == "__main__":
    test_roundtrip()
