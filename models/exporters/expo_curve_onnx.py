"""导出 ExpoCurveNet 为干净静态 ONNX（FP32 + FP16），供 ATC 转 FP16 OM。

产出（默认写到 ``--out-dir``，大件不入库，见 .gitignore）::

    expo_curve_<W>x<H>.onnx        FP32，供 onnxruntime/数值核对
    expo_curve_<W>x<H>_fp16.onnx   FP16，供 build_expo_curve_fp16_om.sh → ATC

严格满足步骤 2 约束（见任务说明 / development-guide.md §6）：
  * 输入 NCHW、opset 13、**单 opset 单输出**、静态 1x3x576x1024、无动态维。
  * 清理图：移除非默认 domain 的 opset_import、剥离 value_info。
  * onnx.checker 校验；打印完整算子直方图；断言上采样为 ConvTranspose、无 Resize、无红名单算子。

FP16 转换刻意"整图直转、不插 Cast"（手工把 FLOAT 初始化器/Constant/IO 直接改 FLOAT16），
对应 development-guide.md §6"FP32 会引入 Cast/AICPU"的规避思路 —— 与
``experiments/zero-dce-npu-om/scripts/build_zero_dce_lite_fp16_om.sh`` 的清理逻辑同源，
区别是本脚本走 NCHW 而非 NHWC/ND。
"""

from __future__ import annotations

import argparse
import os
from collections import Counter
from pathlib import Path

import numpy as np
import onnx
import torch
from onnx import TensorProto, numpy_helper

from models.networks.expo_curve import build_model

MODELS_DIR = Path(__file__).resolve().parents[1]

# NNN 红名单（会回退 AICPU/报错），导出态绝不允许出现，见 development-guide.md §6
RED_LIST = {
    "Resize",
    "Upsample",
    "LayerNormalization",
    "GroupNormalization",
    "InstanceNormalization",
    "Attention",
    "DFT",
    "STFT",
}


def export_fp32(model, h: int, w: int, in_name: str, out_name: str, path: str, opset: int) -> None:
    dummy = torch.rand(1, 3, h, w, dtype=torch.float32)
    torch.onnx.export(
        model,
        dummy,
        path,
        input_names=[in_name],
        output_names=[out_name],
        opset_version=opset,
        do_constant_folding=True,
        dynamic_axes=None,  # 静态尺寸，无动态维
    )


def clean_graph(m: onnx.ModelProto) -> None:
    """移除非默认 domain 的 opset_import，剥离 value_info（让下游重新推断）。"""
    for opset in list(m.opset_import):
        if opset.domain not in ("", "ai.onnx"):
            m.opset_import.remove(opset)
    del m.graph.value_info[:]


def _to_fp16_tensor(t: TensorProto) -> None:
    if t.data_type == TensorProto.FLOAT:
        arr = numpy_helper.to_array(t).astype(np.float16)
        t.CopyFrom(numpy_helper.from_array(arr, t.name))


def convert_to_fp16(m: onnx.ModelProto) -> None:
    """整图直转 FP16：初始化器 + Constant 节点张量 + 图 IO，全部 FLOAT→FLOAT16，不插 Cast。"""
    for init in m.graph.initializer:
        _to_fp16_tensor(init)
    for node in m.graph.node:
        if node.op_type == "Constant":
            for attr in node.attribute:
                if attr.name in ("value",) and attr.t.data_type == TensorProto.FLOAT:
                    _to_fp16_tensor(attr.t)
    for vi in list(m.graph.input) + list(m.graph.output):
        tt = vi.type.tensor_type
        if tt.elem_type == TensorProto.FLOAT:
            tt.elem_type = TensorProto.FLOAT16


def op_histogram(m: onnx.ModelProto) -> Counter:
    return Counter(node.op_type for node in m.graph.node)


def report(m: onnx.ModelProto, tag: str) -> bool:
    """打印算子直方图并做硬约束断言。返回 True 表示通过。"""
    hist = op_histogram(m)
    print(f"\n[{tag}] 算子直方图 ({sum(hist.values())} 节点, {len(hist)} 种):")
    for op, n in sorted(hist.items()):
        print(f"    {op:18s} x{n}")

    red_hit = sorted(set(hist) & RED_LIST)
    has_convt = hist.get("ConvTranspose", 0) > 0
    has_resize = hist.get("Resize", 0) > 0
    has_relu = hist.get("Relu", 0) > 0

    print(f"\n[{tag}] 核对:")
    print(f"    上采样算子 ConvTranspose : {'✓ 有' if has_convt else '✗ 缺'} (x{hist.get('ConvTranspose', 0)})")
    print(f"    Resize/插值上采样        : {'✗ 存在(违规!)' if has_resize else '✓ 无'}")
    print(f"    红名单命中               : {'✗ ' + str(red_hit) if red_hit else '✓ 无'}")
    print(f"    Relu(非绿名单, 期望用Clip): {'⚠ 存在 x' + str(hist.get('Relu')) if has_relu else '✓ 无(用Clip)'}")

    ok = has_convt and not has_resize and not red_hit
    print(f"\n[{tag}] 结论: {'PASS ✅' if ok else 'FAIL ❌'}")
    return ok


def save_clean(m: onnx.ModelProto, path: str, tag: str) -> bool:
    onnx.checker.check_model(m)
    onnx.save(m, path)
    print(f"[{tag}] onnx.checker 通过, 已保存: {path}")
    return report(m, tag)


def main() -> int:
    ap = argparse.ArgumentParser(description="Export ExpoCurveNet to clean static ONNX (FP32+FP16).")
    ap.add_argument("--niter", type=int, default=8)
    ap.add_argument("--filters", type=int, default=16)
    ap.add_argument("--down", type=int, default=4)
    ap.add_argument("--height", type=int, default=576)
    ap.add_argument("--width", type=int, default=1024)
    ap.add_argument("--opset", type=int, default=13)
    ap.add_argument("--in-name", default="input")
    ap.add_argument("--out-name", default="output")
    ap.add_argument("--out-dir", default=str(MODELS_DIR / "weights"))
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--tag", default="", help="附加到文件名的标签（用于砍参数 sweep，不覆盖正式产物）")
    ap.add_argument("--shared", action="store_true", help="共享曲线变体（单套 3 通道复用 niter 次）")
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    os.makedirs(args.out_dir, exist_ok=True)

    model = build_model(niter=args.niter, filters=args.filters, down=args.down,
                        shared_curve=args.shared)
    n_params = sum(p.numel() for p in model.parameters())
    stem = f"expo_curve_{args.width}x{args.height}{args.tag}"
    fp32_path = os.path.join(args.out_dir, f"{stem}.onnx")
    fp16_path = os.path.join(args.out_dir, f"{stem}_fp16.onnx")

    print("=" * 72)
    print(f"ExpoCurveNet  niter={args.niter} filters={args.filters} down={args.down} "
          f"params={n_params}")
    print(f"输入(NCHW): {args.in_name} 1x3x{args.height}x{args.width}  opset={args.opset}")
    print("=" * 72)

    # 1) 导出 FP32 + 清理 + 校验 + 报告
    export_fp32(model, args.height, args.width, args.in_name, args.out_name, fp32_path, args.opset)
    m32 = onnx.load(fp32_path)
    clean_graph(m32)
    ok32 = save_clean(m32, fp32_path, "FP32")

    # 2) FP16 整图直转 + 校验 + 报告（供 ATC）
    m16 = onnx.load(fp32_path)
    convert_to_fp16(m16)
    ok16 = save_clean(m16, fp16_path, "FP16")

    print("\n" + "=" * 72)
    print("交付物:")
    print(f"    FP32 ONNX: {fp32_path}")
    print(f"    FP16 ONNX: {fp16_path}  ← 供 build_expo_curve_fp16_om.sh")
    print("=" * 72)
    return 0 if (ok32 and ok16) else 1


if __name__ == "__main__":
    raise SystemExit(main())
