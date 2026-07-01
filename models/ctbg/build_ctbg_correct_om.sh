#!/bin/bash
# 转正确版 OM(estimator raw + apply correct),板端实测。
# 复用已验证的 atc env + x86_64 setenv,与 build_ctbg_om.sh 一致。
set -euo pipefail
cd "$(dirname "$0")"

ONNX_DIR="p1_bidir/runs/om_prep_ctbg_correct"
OUT="p1_bidir/runs/om_ctbg_correct"; mkdir -p "$OUT"
SOC_VERSION="${SOC_VERSION:-OPTG}"
ATC_ENV=/home/alan/miniconda3/envs/atc
export PATH="${ATC_ENV}/bin:${PATH}"

if ! command -v atc >/dev/null 2>&1; then
    unset PYTHONHOME; export PYTHONPATH="${PYTHONPATH:-}"
    set +eu
    source /home/alan/Learning/Hispark/tools/local/Ascend/ascend-toolkit/5.20.t6.2.b060/x86_64-linux/bin/setenv.bash
    set -eu
fi

echo ">>> atc: estimator_raw"
atc --model="$ONNX_DIR/ctbg_estimator_raw_256x144.onnx" \
    --framework=5 \
    --output="$OUT/ctbg_estimator_raw_256x144" \
    --input_format=NCHW \
    --input_shape="in_low:1,3,144,256" \
    --input_fp16_nodes="in_low" \
    --soc_version="$SOC_VERSION" \
    --output_type=FP16
[ -f fusion_result.json ] && mv -f fusion_result.json "$OUT/ctbg_estimator_raw_256x144.fusion_result.json"

echo ""; echo ">>> atc: apply_correct_k4"
atc --model="$ONNX_DIR/ctbg_apply_correct_k4_1024x576.onnx" \
    --framework=5 \
    --output="$OUT/ctbg_apply_correct_k4_1024x576" \
    --input_format=NCHW \
    --input_shape="in_full:1,3,576,1024;raw_coeff:1,18,144,256" \
    --input_fp16_nodes="in_full;raw_coeff" \
    --soc_version="$SOC_VERSION" \
    --output_type=FP16
[ -f fusion_result.json ] && mv -f fusion_result.json "$OUT/ctbg_apply_correct_k4_1024x576.fusion_result.json"

echo ""; echo ">>> atc: apply_correct_k8"
atc --model="$ONNX_DIR/ctbg_apply_correct_k8_1024x576.onnx" \
    --framework=5 \
    --output="$OUT/ctbg_apply_correct_k8_1024x576" \
    --input_format=NCHW \
    --input_shape="in_full:1,3,576,1024;raw_coeff:1,18,144,256" \
    --input_fp16_nodes="in_full;raw_coeff" \
    --soc_version="$SOC_VERSION" \
    --output_type=FP16
[ -f fusion_result.json ] && mv -f fusion_result.json "$OUT/ctbg_apply_correct_k8_1024x576.fusion_result.json"

echo ""; echo ">>> atc: apply_twostage (方案C: ×2 bilinear raw→288 decode→×2 nearest 288→576)"
atc --model="$ONNX_DIR/ctbg_apply_twostage_1024x576.onnx" \
    --framework=5 \
    --output="$OUT/ctbg_apply_twostage_1024x576" \
    --input_format=NCHW \
    --input_shape="in_full:1,3,576,1024;raw_coeff:1,18,144,256" \
    --input_fp16_nodes="in_full;raw_coeff" \
    --soc_version="$SOC_VERSION" \
    --output_type=FP16
[ -f fusion_result.json ] && mv -f fusion_result.json "$OUT/ctbg_apply_twostage_1024x576.fusion_result.json"

echo ""; echo ">>> atc: apply_twostage_nn (双 nearest: ×2 nearest raw→288 decode→×2 nearest 288→576)"
atc --model="$ONNX_DIR/ctbg_apply_twostage_nn_1024x576.onnx" \
    --framework=5 \
    --output="$OUT/ctbg_apply_twostage_nn_1024x576" \
    --input_format=NCHW \
    --input_shape="in_full:1,3,576,1024;raw_coeff:1,18,144,256" \
    --input_fp16_nodes="in_full;raw_coeff" \
    --soc_version="$SOC_VERSION" \
    --output_type=FP16
[ -f fusion_result.json ] && mv -f fusion_result.json "$OUT/ctbg_apply_twostage_nn_1024x576.fusion_result.json"

# AIPP 版(NV21 first input, deploy 用)
AIPP_CFG="/home/alan/Learning/Hispark/Hispark_SocChina_2026/models/configs/aipp_nv21_1024x576.cfg"
echo ""; echo ">>> atc: apply_twostage_nn + AIPP(NV21→RGB fp16)"
atc --model="$ONNX_DIR/ctbg_apply_twostage_nn_1024x576.onnx" \
    --framework=5 \
    --output="$OUT/ctbg_apply_twostage_nn_aipp_1024x576" \
    --insert_op_conf="$AIPP_CFG" \
    --input_format=NCHW \
    --input_shape="in_full:1,3,576,1024;raw_coeff:1,18,144,256" \
    --input_fp16_nodes="raw_coeff" \
    --soc_version="$SOC_VERSION" \
    --output_type=FP16
[ -f fusion_result.json ] && mv -f fusion_result.json "$OUT/ctbg_apply_twostage_nn_aipp_1024x576.fusion_result.json"

echo ""; echo ">>> done:"
ls -lah "$OUT"/*.om
