"""导出 SCI 推理网络为干净静态 ONNX（FP32+FP16）——复用 ExpoCurveNet 导出的清理/校验逻辑。

仅作速率对照基线（SCI 单向提亮，见 networks/sci.py）。用法同 exporters/expo_curve_onnx.py。
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path

import onnx
import torch

from models.exporters.expo_curve_onnx import clean_graph, convert_to_fp16, export_fp32, save_clean
from models.networks.sci import build_model

MODELS_DIR = Path(__file__).resolve().parents[1]


def main() -> int:
    ap = argparse.ArgumentParser(description="Export SCI inference net to clean static ONNX.")
    ap.add_argument("--layers", type=int, default=3)
    ap.add_argument("--channels", type=int, default=3)
    ap.add_argument("--height", type=int, default=576)
    ap.add_argument("--width", type=int, default=1024)
    ap.add_argument("--opset", type=int, default=13)
    ap.add_argument("--in-name", default="input")
    ap.add_argument("--out-name", default="output")
    ap.add_argument("--out-dir", default=str(MODELS_DIR / "weights"))
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--tag", default="")
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    os.makedirs(args.out_dir, exist_ok=True)
    model = build_model(layers=args.layers, channels=args.channels)
    n_params = sum(p.numel() for p in model.parameters())
    stem = f"sci_{args.width}x{args.height}{args.tag}"
    fp32_path = os.path.join(args.out_dir, f"{stem}.onnx")
    fp16_path = os.path.join(args.out_dir, f"{stem}_fp16.onnx")

    print(f"SCI layers={args.layers} channels={args.channels} params={n_params} "
          f"in=1x3x{args.height}x{args.width} opset={args.opset}")

    export_fp32(model, args.height, args.width, args.in_name, args.out_name, fp32_path, args.opset)
    m32 = onnx.load(fp32_path)
    clean_graph(m32)
    ok32 = save_clean(m32, fp32_path, "FP32")

    m16 = onnx.load(fp32_path)
    convert_to_fp16(m16)
    ok16 = save_clean(m16, fp16_path, "FP16")
    print(f"FP16 ONNX: {fp16_path}")
    return 0 if (ok32 and ok16) else 1


if __name__ == "__main__":
    raise SystemExit(main())
