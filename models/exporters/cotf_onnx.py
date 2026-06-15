"""导出 CoTF param-net（出 LUT 的 NN）为干净静态 ONNX（FP32+FP16）。复用 ExpoCurveNet 导出/清理逻辑。

CoTF 路线：NN 只在低分辨率出全局 3D-LUT（NPU），全分辨率施加交给 ISP 硬件 CLUT（零 NPU）。
本脚本只导 NN 端；LUT-apply（grid_sample）无法上 NPU（见 cotf-route-verification.md），不导出。
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path

import onnx
import torch

from models.exporters.expo_curve_onnx import clean_graph, convert_to_fp16, op_histogram
from models.networks.cotf import build_param_net

MODELS_DIR = Path(__file__).resolve().parents[1]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lut-dim", type=int, default=17, help="3D-LUT 每轴节点数（17³≈硬件 5508）")
    ap.add_argument("--down", type=int, default=8, help="param-net 输入降采样倍率")
    ap.add_argument("--ch", type=int, default=32)
    ap.add_argument("--height", type=int, default=576, help="param-net 输入高（喂缩略图更省，与显示分辨率无关）")
    ap.add_argument("--width", type=int, default=1024)
    ap.add_argument("--opset", type=int, default=13)
    ap.add_argument("--out-dir", default=str(MODELS_DIR / "weights"))
    ap.add_argument("--tag", default="")
    args = ap.parse_args()

    torch.manual_seed(0)
    os.makedirs(args.out_dir, exist_ok=True)
    model = build_param_net(down=args.down, ch=args.ch, lut_dim=args.lut_dim)
    n = sum(t.numel() for t in model.parameters())
    stem = f"cotf_paramnet_{args.width}x{args.height}{args.tag}"
    fp32 = os.path.join(args.out_dir, f"{stem}.onnx")
    fp16 = os.path.join(args.out_dir, f"{stem}_fp16.onnx")

    dummy = torch.rand(1, 3, args.height, args.width)
    torch.onnx.export(model, dummy, fp32, input_names=["input"], output_names=["lut"],
                      opset_version=args.opset, do_constant_folding=True)
    m32 = onnx.load(fp32); clean_graph(m32); onnx.checker.check_model(m32); onnx.save(m32, fp32)
    m16 = onnx.load(fp32); convert_to_fp16(m16); onnx.checker.check_model(m16); onnx.save(m16, fp16)

    hist = op_histogram(m16)
    print(f"CoTF param-net lut_dim={args.lut_dim} params={n} in=1x3x{args.height}x{args.width}")
    print(f"  LUT 输出通道 = 3*{args.lut_dim}^3 = {3 * args.lut_dim ** 3}")
    print(f"  ops = {dict(sorted(hist.items()))}")
    red = {"Resize", "Upsample", "LayerNormalization", "GridSample", "Attention"}
    print(f"  红名单命中: {sorted(set(hist) & red) or 'NONE ✓'}")
    print(f"  FP16 ONNX: {fp16}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
