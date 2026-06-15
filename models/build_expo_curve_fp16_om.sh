#!/bin/bash
# ATC: FP16 ONNX → FP16 OM（NCHW 路径，本轮不挂 AIPP —— §6 点1 只测模型本体）。
#
# 以 experiments/zero-dce-npu-om/scripts/build_zero_dce_lite_fp16_om.sh 为模板，改动:
#   * 输入布局 NHWC/ND → **NCHW**（AIPP 需 NCHW；本轮虽不挂 AIPP 也按部署布局导出）。
#   * input_shape 改为 1,3,H,W（NCHW）。
#   * 不绑定 conda 环境名、不写死个人绝对路径 —— CANN 环境经环境变量传入（development-guide §3.4）。
#
# 用法:
#   ATC_SETENV=/path/to/CANN/.../bin/setenv.bash \
#   [SOC_VERSION=OPTG] [INPUT_NAME=input] \
#     models/build_expo_curve_fp16_om.sh <fp16.onnx> <out_dir> <out_name> [WIDTH] [HEIGHT]
#
# 示例（本机本地 toolkit；路径按各自机器经环境变量给）:
#   ATC_SETENV=tools/local/Ascend/ascend-toolkit/5.20.t6.2.b060/x86_64-linux/bin/setenv.bash \
#     models/build_expo_curve_fp16_om.sh \
#       models/weights/expo_curve_1024x576_fp16.onnx models/weights expo_curve_1024x576_fp16 1024 576
set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "Usage: $0 <fp16.onnx> <out_dir> <out_name> [WIDTH=1024] [HEIGHT=576]" >&2
    exit 2
fi

FP16_ONNX="$1"
OUT_DIR="$2"
OUT_NAME="$3"
WIDTH="${4:-1024}"
HEIGHT="${5:-576}"

SOC_VERSION="${SOC_VERSION:-OPTG}"
INPUT_NAME="${INPUT_NAME:-input}"

mkdir -p "${OUT_DIR}"
OM_PREFIX="${OUT_DIR}/${OUT_NAME}"

# CANN 环境：若 atc 已在 PATH 直接用；否则 source ATC_SETENV（不写死任何机器路径）
if ! command -v atc >/dev/null 2>&1; then
    if [ -z "${ATC_SETENV:-}" ]; then
        echo "ERROR: atc 不在 PATH，且未设置 ATC_SETENV（CANN setenv.bash 路径）" >&2
        echo "       见 development-guide.md §3.2/§3.4：CANN 工具包外部安装，路径经环境变量传入。" >&2
        exit 3
    fi
    # shellcheck disable=SC1090
    unset PYTHONHOME
    export PYTHONPATH="${PYTHONPATH:-}"
    set +eu  # CANN setenv.bash 引用未定义变量、内部 grep 退出非零；临时放开 errexit/nounset
    source "${ATC_SETENV}"
    set -eu
fi

# 可选 AIPP：设 AIPP_CFG=<aipp.cfg> 则把预处理（NV21→RGB→/255）融入 OM 前端（§6 点 2）。
# 默认不挂 AIPP（§6 点 1 只测模型本体）。
AIPP_ARGS=()
if [ -n "${AIPP_CFG:-}" ]; then
    [ -f "${AIPP_CFG}" ] || { echo "ERROR: AIPP_CFG 不存在: ${AIPP_CFG}" >&2; exit 4; }
    AIPP_ARGS=(--insert_op_conf="${AIPP_CFG}")
    echo ">>> 挂载 AIPP: ${AIPP_CFG}"
fi

echo ">>> atc: ${FP16_ONNX} → ${OM_PREFIX}.om  (NCHW, FP16, soc=${SOC_VERSION}, aipp=${AIPP_CFG:-none})"
atc --model="${FP16_ONNX}" \
    --framework=5 \
    --output="${OM_PREFIX}" \
    --input_format=NCHW \
    --input_shape="${INPUT_NAME}:1,3,${HEIGHT},${WIDTH}" \
    --soc_version="${SOC_VERSION}" \
    --output_type=FP16 \
    "${AIPP_ARGS[@]}"

# fusion_result.json 落在 OM 旁，供算子探测（步骤 4）
if [ -f fusion_result.json ]; then
    mv -f fusion_result.json "${OM_PREFIX}.fusion_result.json"
fi

echo "${OM_PREFIX}.om"
