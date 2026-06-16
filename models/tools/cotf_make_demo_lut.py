"""生成一个**可见的**曝光校正 3D-LUT（提亮暗部的 tone-curve），经同一 host→硬件桥打包为
SS928 ISP CLUT 的 5508 节点 u32 .bin。用于板端实时演示：CLUT 关=原始相机图，CLUT 开=CoTF 校正。

这不是训练好的 param-net 输出（那需要训练），而是一条语义明确、对任意场景都成立的全局色调
校正：lift shadows（gamma<1）+ 轻微 S 形对比 + 轻微提饱和。toggle 时效果一眼可辨。
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np

from models.tools.cotf_lut_pack import HW_LUT_LENGTH, pack_cubic_to_hw, write_lut_bin

MODELS_DIR = Path(__file__).resolve().parents[1]


def tone_curve(x: np.ndarray, gamma: float, s_strength: float) -> np.ndarray:
    """提亮暗部：先 gamma(<1) 抬亮，再叠一个温和 S 形保住对比。"""
    y = np.power(np.clip(x, 0, 1), gamma)              # lift shadows
    s = y + s_strength * np.sin(2 * np.pi * y) / (2 * np.pi)  # 轻 S 形（端点不动）
    return np.clip(s, 0.0, 1.0)


def build_demo_cubic(dim: int, gamma: float, s_strength: float, sat: float) -> np.ndarray:
    axis = np.linspace(0.0, 1.0, dim)
    r, g, b = np.meshgrid(axis, axis, axis, indexing="ij")
    rgb = np.stack([r, g, b], axis=-1)
    out = tone_curve(rgb, gamma, s_strength)
    # 轻微提饱和：朝离亮度的方向推
    luma = (0.299 * out[..., 0] + 0.587 * out[..., 1] + 0.114 * out[..., 2])[..., None]
    out = np.clip(luma + (1.0 + sat) * (out - luma), 0.0, 1.0)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lut-dim", type=int, default=17)
    ap.add_argument("--gamma", type=float, default=0.55, help="<1 提亮暗部")
    ap.add_argument("--s", type=float, default=0.15, help="S 形对比强度")
    ap.add_argument("--sat", type=float, default=0.12, help="提饱和量")
    ap.add_argument("--out", default=str(MODELS_DIR / "weights" / "cotf_clut.bin"))
    args = ap.parse_args()

    cubic = build_demo_cubic(args.lut_dim, args.gamma, args.s, args.sat)
    packed = pack_cubic_to_hw(cubic)
    write_lut_bin(args.out, packed)
    print(f"demo tone-LUT: gamma={args.gamma} s={args.s} sat={args.sat} dim={args.lut_dim}")
    print(f"  → hw {HW_LUT_LENGTH} nodes, u32 range [{packed.min():#010x},{packed.max():#010x}]")
    print(f"  wrote {args.out} ({packed.size*4} bytes)")
    # 自检：中灰 0.5 经曲线应明显变亮
    mid = tone_curve(np.array([0.5]), args.gamma, args.s)[0]
    print(f"  sanity: input mid-gray 0.50 → {mid:.3f} (应 >0.5，提亮)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
