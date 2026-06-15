"""导出 MSEC 类 U-Net 为干净静态 ONNX（FP32+FP16），复用 ExpoCurveNet 导出/清理逻辑。速率对照基线。"""

from __future__ import annotations

import argparse
import os
import sys

import onnx
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from msec_net import build_model  # noqa: E402
from export_expo_curve_onnx import clean_graph, convert_to_fp16, export_fp32, save_clean  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", type=int, default=24)
    ap.add_argument("--height", type=int, default=576)
    ap.add_argument("--width", type=int, default=1024)
    ap.add_argument("--opset", type=int, default=13)
    ap.add_argument("--out-dir", default=os.path.join(os.path.dirname(__file__), "weights"))
    ap.add_argument("--tag", default="")
    args = ap.parse_args()

    torch.manual_seed(0)
    os.makedirs(args.out_dir, exist_ok=True)
    model = build_model(base_ch=args.base)
    n = sum(p.numel() for p in model.parameters())
    stem = f"msec_{args.width}x{args.height}{args.tag}"
    fp32 = os.path.join(args.out_dir, f"{stem}.onnx")
    fp16 = os.path.join(args.out_dir, f"{stem}_fp16.onnx")
    print(f"MSEC-UNet base_ch={args.base} params={n} in=1x3x{args.height}x{args.width}")

    export_fp32(model, args.height, args.width, "input", "output", fp32, args.opset)
    m32 = onnx.load(fp32); clean_graph(m32); ok32 = save_clean(m32, fp32, "FP32")
    m16 = onnx.load(fp32); convert_to_fp16(m16); ok16 = save_clean(m16, fp16, "FP16")
    print(f"FP16 ONNX: {fp16}")
    return 0 if (ok32 and ok16) else 1


if __name__ == "__main__":
    raise SystemExit(main())
