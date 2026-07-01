#!/bin/bash
# CTBG v9 6ch ONNX → OM 转换脚本
# 用法：cd models/ctbg && bash build_ctbg_6ch_om.sh [onnx_dir]
#
# 前置：已运行 export_ctbg_6ch.py 产出 ONNX
# 环境：ATC 用 conda atc（py3.9.2+protobuf3.13），
#       必须把 envs/atc/bin 放 PATH 头部（非交互 shell conda activate 失效）
set -euo pipefail

ONNX_DIR="${1:-p1_bidir/runs/om_ctbg_6ch}"
OUT_DIR="$ONNX_DIR"

EST_ONNX="$ONNX_DIR/ctbg6ch_estimator_256x144.onnx"
APP_ONNX="$ONNX_DIR/ctbg6ch_apply_1024x576.onnx"

ATC_BIN="${ATC_BIN:-atc}"
SOC_VERSION="${SOC_VERSION:-Ascend310}"

echo "=== CTBG v9 6ch ATC 转换 ==="

# estimator（单输入，无 AIPP）
echo "[1/2] estimator"
"$ATC_BIN" --model="$EST_ONNX" --output="$OUT_DIR/ctbg6ch_estimator_256x144" \
    --framework=5 --soc_version="$SOC_VERSION" \
    --input_format=ND --input_shape="in_low:1,3,144,256" \
    --output_type=FP16

# apply（双输入：全分辨率 RGB fp16 + 预上采样系数 fp16）
echo "[2/2] apply"
"$ATC_BIN" --model="$APP_ONNX" --output="$OUT_DIR/ctbg6ch_apply_1024x576" \
    --framework=5 --soc_version="$SOC_VERSION" \
    --input_format=ND --input_shape="in_full:1,3,576,1024;coeff_up:1,6,576,1024" \
    --input_fp16_nodes="in_full;coeff_up" \
    --output_type=FP16

echo ""
echo "=== 产出 ==="
ls -lh "$OUT_DIR"/ctbg6ch_estimator_256x144.om "$OUT_DIR"/ctbg6ch_apply_1024x576.om
echo ""
echo "板端部署：scp $OUT_DIR/ctbg6ch_*.om hispark-remote:/root/socchina-2026/"
