#!/usr/bin/env python3
"""合成 RAW 场景生成器——不动相机扩展保真度闸门的输入多样性。

闸门测的是"同一输入下参数变化的输出排序保真"，输入的关键属性是亮度直方图与
局部结构分布而非语义内容；合成场景可网格化覆盖直方图空间，比随机换真实场景
更系统（docs/isp-param-tuning-research.md §5）。

工作流:
    # 1. 板端采一帧裸 bayer 作布局/黑电平参照（compress NONE）
    ./test_raw_replay --compress-none --save-raw --outdir out
    # 2. 主机判定 12bpp packed 布局并读取黑电平统计
    python -m models.isp_simulator.synth_raw parse --raw out/raw_ref.raw
    # 3. 生成合成场景集（按判定布局打包）
    python -m models.isp_simulator.synth_raw gen --outdir models/weights/fidelity/synth \
        --layout <parse 判定的布局> --black <parse 报告的黑电平> --white <p99>
    # 4. 板端文件回灌 × sweep（见 fidelity_gate gen 打印的命令加 --raw-file）

12bpp packed 布局候选（每 2 像素 3 字节）:
    lsb:  b0=p0[7:0], b1=p1[3:0]<<4|p0[11:8], b2=p1[11:4]   （连续 little-endian 位流）
    mipi: b0=p0[11:4], b1=p1[11:4], b2=p1[3:0]<<4|p0[3:0]   （MIPI CSI-2 RAW12）
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

SENSOR_W, SENSOR_H = 3840, 2160
STRIDE = (SENSOR_W * 12 + 7) // 8  # 5760，已 16 对齐


def unpack12(data: np.ndarray, layout: str) -> np.ndarray:
    """(H, stride) uint8 → (H, W) uint16。"""
    b = data.reshape(-1, 3).astype(np.uint16)
    if layout == "lsb":
        p0 = b[:, 0] | ((b[:, 1] & 0x0F) << 8)
        p1 = (b[:, 1] >> 4) | (b[:, 2] << 4)
    elif layout == "mipi":
        p0 = (b[:, 0] << 4) | (b[:, 2] & 0x0F)
        p1 = (b[:, 1] << 4) | (b[:, 2] >> 4)
    else:
        raise ValueError(layout)
    out = np.empty(b.shape[0] * 2, dtype=np.uint16)
    out[0::2] = p0
    out[1::2] = p1
    return out.reshape(SENSOR_H, SENSOR_W)


def pack12(img: np.ndarray, layout: str) -> bytes:
    """(H, W) uint16 [0,4095] → 裸 packed bytes（stride*H）。"""
    flat = img.astype(np.uint16).reshape(-1)
    p0, p1 = flat[0::2], flat[1::2]
    b = np.empty((p0.size, 3), dtype=np.uint8)
    if layout == "lsb":
        b[:, 0] = p0 & 0xFF
        b[:, 1] = ((p1 & 0x0F) << 4) | (p0 >> 8)
        b[:, 2] = p1 >> 4
    elif layout == "mipi":
        b[:, 0] = p0 >> 4
        b[:, 1] = p1 >> 4
        b[:, 2] = ((p1 & 0x0F) << 4) | (p0 & 0x0F)
    else:
        raise ValueError(layout)
    return b.tobytes()


# ── sRGB → sensor 域 RAW（板端回灌公开数据集用）──────────────
# 三个域转换常量均为板端实测（2026-07-04，docs/isp-param-tuning-research.md §5.8）：
#   - bayer 相序 BGGR（OS08A20 pub_attr 实配，RGGB 会导致品红）
#   - 逆 AWB 增益（ISP 按真实 sensor 色彩响应标定，灰输入实测 R/G=1.77 B/G=1.85）
#   - 黑电平 256（条带实验定位 ISP BLC 减除点；基准错位的残留 DC 经 AWB 放大成暗部紫偏）
SENSOR_BLC = 256
SENSOR_WHITE = 3700
INV_AWB_R, INV_AWB_B = 1.77, 1.85


def rgb_to_sensor_raw(rgb: np.ndarray) -> bytes:
    """sRGB float [0,1] (H=2160, W=3840, 3) → OS08A20 sensor 域裸 12bpp packed RAW。

    逆变换链：sRGB→线性(γ2.2) → 逆 AWB（模拟 sensor 色彩响应）→ BGGR 马赛克
    → BLC 基准映射 → pack12(lsb)。回灌后 ISP（AWB/CCM/gamma）近似还原原图。
    """
    assert rgb.shape == (SENSOR_H, SENSOR_W, 3), rgb.shape
    lin = rgb.astype(np.float32) ** 2.2
    lin = lin.copy()
    lin[..., 0] /= INV_AWB_R
    lin[..., 2] /= INV_AWB_B
    bayer = np.empty((SENSOR_H, SENSOR_W), np.float32)
    bayer[0::2, 0::2] = lin[0::2, 0::2, 2]  # B
    bayer[0::2, 1::2] = lin[0::2, 1::2, 1]  # G
    bayer[1::2, 0::2] = lin[1::2, 0::2, 1]  # G
    bayer[1::2, 1::2] = lin[1::2, 1::2, 0]  # R
    raw = np.clip(SENSOR_BLC + bayer * (SENSOR_WHITE - SENSOR_BLC), 0, 4095).astype(np.uint16)
    return pack12(raw, "lsb")


def cmd_parse(args) -> int:
    raw = np.fromfile(args.raw, dtype=np.uint8)
    expect = STRIDE * SENSOR_H
    if raw.size != expect:
        sys.exit(f"{args.raw}: {raw.size} bytes, 期望 {expect}（stride {STRIDE} x {SENSOR_H}；"
                 "请确认板端用了 --compress-none）")

    print(f"{args.raw}: {raw.size} bytes")
    best, best_grad = None, None
    for layout in ("lsb", "mipi"):
        img = unpack12(raw, layout).astype(np.float32)
        # 正确布局下相邻像素强相关 → 水平梯度显著更小
        grad = float(np.abs(np.diff(img[::16, :], axis=1)).mean())
        p1, p50, p99 = np.percentile(img, [1, 50, 99])
        print(f"  layout={layout}: 水平梯度={grad:.1f}  p1={p1:.0f} p50={p50:.0f} p99={p99:.0f}")
        if best_grad is None or grad < best_grad:
            best, best_grad = layout, grad

    img = unpack12(raw, best).astype(np.float32)
    p1, p99 = np.percentile(img, [1, 99])
    print(f"\n判定布局: {best}")
    print(f"建议合成参数: --layout {best} --black {int(p1)} --white {int(p99)}")

    if args.preview:
        from PIL import Image
        prev = np.clip((img - p1) / max(p99 - p1, 1) * 255.0, 0, 255).astype(np.uint8)
        Image.fromarray(prev[::4, ::4]).save(args.preview)
        print(f"预览: {args.preview}")
    return 0


def build_scenes(black: int, white: int, rng: np.random.Generator) -> dict[str, np.ndarray]:
    """受控直方图场景集。灰度 bayer（RGGB 同值）→ AWB 中性；值以实测黑电平/白点为界。"""
    H, W = SENSOR_H, SENSOR_W
    span = white - black

    def lvl(f):
        return black + f * span

    yy, xx = np.meshgrid(np.linspace(0, 1, H), np.linspace(0, 1, W), indexing="ij")
    scenes: dict[str, np.ndarray] = {}

    # 1-2 平坦低/中调（参数响应的基准点）
    scenes["flat_dark"] = np.full((H, W), lvl(0.06), np.float32)
    scenes["flat_mid"] = np.full((H, W), lvl(0.30), np.float32)
    # 3 水平渐变（全直方图扫掠）
    scenes["hgrad"] = lvl(0.02) + xx.astype(np.float32) * span * 0.9
    # 4 逆光双峰：暗背景 + 亮窗
    img = np.full((H, W), lvl(0.05), np.float32)
    img[H // 5:H * 3 // 5, W // 2:W * 9 // 10] = lvl(0.85)
    scenes["backlit"] = img
    # 5 中调高频纹理（局部对比特征）
    tex = np.sin(xx * 240 * np.pi) * np.sin(yy * 135 * np.pi)
    scenes["texture_mid"] = lvl(0.35) + tex.astype(np.float32) * span * 0.12
    # 6 暗调纹理（暗部细节场景）
    scenes["dark_texture"] = lvl(0.08) + (tex.astype(np.float32) + 1) * span * 0.05
    # 7 垂直渐变 + 亮斑（混合分布）
    img = lvl(0.05) + yy.astype(np.float32) * span * 0.35
    for cx, cy in ((0.2, 0.3), (0.7, 0.6), (0.5, 0.15)):
        r2 = (xx - cx) ** 2 + (yy - cy) ** 2
        img += np.exp(-r2 / 0.003).astype(np.float32) * span * 0.55
    scenes["vgrad_spots"] = img
    # 8 低照噪声（暗 + 传感器样噪声）
    scenes["lowlight_noise"] = lvl(0.05) + rng.normal(0, span * 0.02, (H, W)).astype(np.float32)

    # 全部叠加轻噪声（避免 ISP 对纯平坦输入的异常路径），限幅 12bit
    out = {}
    for name, img in scenes.items():
        img = img + rng.normal(0, span * 0.004, (H, W)).astype(np.float32)
        out[name] = np.clip(img, 0, 4095).astype(np.uint16)
    return out


def cmd_gen(args) -> int:
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(20260703)
    scenes = build_scenes(args.black, args.white, rng)
    for name, img in scenes.items():
        (outdir / f"synth_{name}.raw").write_bytes(pack12(img, args.layout))
        print(f"  synth_{name}.raw  (p1={np.percentile(img, 1):.0f} "
              f"p50={np.percentile(img, 50):.0f} p99={np.percentile(img, 99):.0f})")
    print(f"\n{len(scenes)} 个合成场景 -> {outdir}（布局 {args.layout}，"
          f"black={args.black} white={args.white}）")
    print("板端: ./test_raw_replay --settle 8 " +
          " ".join(f"--raw-file synth_{n}.raw" for n in scenes) + " --blob ...")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("parse", help="判定板端裸 RAW 的 12bpp 布局与黑电平")
    p.add_argument("--raw", required=True)
    p.add_argument("--preview", help="可选 PNG 预览输出路径")

    g = sub.add_parser("gen", help="生成合成场景集")
    g.add_argument("--outdir", default="models/weights/fidelity/synth")
    g.add_argument("--layout", required=True, choices=("lsb", "mipi"))
    g.add_argument("--black", type=int, required=True, help="实测黑电平（parse 的 p1）")
    g.add_argument("--white", type=int, required=True, help="实测白点（parse 的 p99）")

    args = ap.parse_args()
    return cmd_parse(args) if args.cmd == "parse" else cmd_gen(args)


if __name__ == "__main__":
    sys.exit(main())
