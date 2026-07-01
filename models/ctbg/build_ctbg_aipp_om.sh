#!/bin/bash
# v9 6ch AIPP OM 构建脚本（NV21→RGB 硬件转换 + elementwise apply）
# 前置：已运行 export_ctbg_6ch.py 产出 ONNX
# 环境：conda atc（py3.9.2+protobuf3.13），CCE 编译器需在 PATH 或
#       /usr/local/HiAI/runtime/ccec_compiler/bin/ccec
# 已知限制：当前构建的 OM 在 Ascend310 上执行失败（aclError=507900），
#       疑似因 elementwise 图中缺 TransData NHWC↔NCHW 格式桥接。
#       v8 AIPP OM（含 ConvTranspose）可正常运行，待排查 ONNX 差异。
set -euo pipefail

ONNX_DIR="${1:-p1_bidir/runs/om_ctbg_6ch}"
OUT_DIR="$ONNX_DIR"
AIPP_CFG="${AIPP_CFG:-models/configs/aipp_nv21_1024x576.cfg}"
SOC_VERSION="${SOC_VERSION:-Ascend310}"

echo "=== v9 6ch AIPP OM 构建 ==="
atc --model="$ONNX_DIR/ctbg6ch_apply_1024x576.onnx" \
    --framework=5 \
    --output="$OUT_DIR/ctbg6ch_apply_aipp_1024x576" \
    --insert_op_conf="$AIPP_CFG" \
    --input_shape="in_full:1,3,576,1024;coeff_up:1,6,576,1024" \
    --input_fp16_nodes="coeff_up" \
    --soc_version="$SOC_VERSION" \
    --output_type=FP16

echo "=== 产出 ==="
ls -lh "$OUT_DIR"/ctbg6ch_apply_aipp_1024x576.om
