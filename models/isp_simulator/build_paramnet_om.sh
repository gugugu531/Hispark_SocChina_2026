#!/usr/bin/env bash
# ParamNet ONNX → OM(AIPP，部署形态)。路线 B1 关口②：ATC→OM + 算子探测。
#
# 部署路径：VPSS chn2 缩略图(256x144 NV21) → AIPP → ParamNet OM → u。
# AIPP 在 OM 前端硬件完成 NV21→RGB→/255→fp16，实测使产物 0 Cast、全 AICore
# （无 AIPP 版有 2 个 FP32 输入/输出边界 Cast）。
#
# 前置环境（外部提供，不写死个人路径）：
#   ASCEND_TOOLKIT_HOME  CANN 5.x：指向 .../ascend-toolkit/<ver>/x86_64-linux
#                        （脚本 source 其 bin/setenv.bash 串联 compiler/runtime/opp/toolkit）。
#   ATC_PYTHON           ATC 的 TBE 编译器需要带依赖(numpy/decorator/sympy/cffi)的 python3；
#                        指向该解释器 bin 目录（如 conda `atc` 环境的 bin），脚本前置到 PATH。
#                        ⚠ 踩坑：用系统/base python3 会 "Failed to init tbe"（TBE 初始化失败）。
#   SOC_VERSION          默认 OPTG（SS928 NPU）。
#
# 用法：
#   ASCEND_TOOLKIT_HOME=/path/ascend-toolkit/5.20.x/x86_64-linux \
#   ATC_PYTHON=/path/miniconda3/envs/atc/bin \
#     models/isp_simulator/build_paramnet_om.sh
# 不用 set -u：CANN 的 setenv 脚本会引用未设变量（LD_LIBRARY_PATH/PYTHONPATH 等）。
set -eo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

: "${ASCEND_TOOLKIT_HOME:?需设 ASCEND_TOOLKIT_HOME 指向 ascend-toolkit/<ver>/x86_64-linux}"
SOC_VERSION="${SOC_VERSION:-OPTG}"
OUTDIR="${OUTDIR:-models/weights/paramnet}"
ONNX="${ONNX:-$OUTDIR/paramnet_256x144_fp32.onnx}"
AIPP_CFG="${AIPP_CFG:-models/configs/aipp_nv21_256x144.cfg}"

# CANN 环境（setenv 可能返回非零/引用未设变量，隔离其副作用）
# shellcheck source=/dev/null
set +e
source "$ASCEND_TOOLKIT_HOME/bin/setenv.bash" >/dev/null 2>&1
set -e
export PATH="$ASCEND_TOOLKIT_HOME/atc/bin:$PATH"
[ -n "${ATC_PYTHON:-}" ] && export PATH="$ATC_PYTHON:$PATH"

if [ ! -f "$ONNX" ]; then
    echo "缺 $ONNX：先在 torch 环境运行  python -m models.isp_simulator.paramnet export" >&2
    exit 1
fi

echo "[atc] ParamNet → OM  soc=$SOC_VERSION  aipp=$AIPP_CFG  python3=$(command -v python3)"
atc --model="$ONNX" --framework=5 --soc_version="$SOC_VERSION" \
    --input_format=NCHW --insert_op_conf="$AIPP_CFG" \
    --output="$OUTDIR/paramnet_256x144_aipp" --output_type=FP16

echo "[atc] 产物: $OUTDIR/paramnet_256x144_aipp.om"
echo "[atc] 算子探测（可选）: atc --mode=1 --om=<om> --json=<json> 后查 type 分布，确认 0 Cast / 无 AICPU"
