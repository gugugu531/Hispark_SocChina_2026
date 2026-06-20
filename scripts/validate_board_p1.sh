#!/usr/bin/env bash
# 上传当前构建与正式模型，并在板端执行 P1 稳定性验收。
# 用法：BOARD=hispark-remote scripts/validate_board_p1.sh [持续秒数]
set -euo pipefail

readonly REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly BUILD_DIR="${REPO_ROOT}/build"
readonly RUN_DIR="${RUN_DIR:-/root/socchina-p1-validate}"
readonly DURATION="${1:-600}"
readonly MODEL="${MODEL_FILE:-${REPO_ROOT}/models/weights/cotf_paramnet_lcdp_rtx4060/cotf_paramnet_256x144_lcdp_best_e0167_fp16_aipp.om}"

if [[ -z "${BOARD:-}" ]]; then
    echo "[p1] set BOARD to an SSH alias or board target" >&2
    exit 2
fi
scp_board_target() {
    local target="${BOARD}"
    local user=""
    local host="${target}"
    if [[ "${target}" == *@* ]]; then
        user="${target%%@*}@"
        host="${target#*@}"
    fi
    if [[ "${host}" == *:* && "${host}" != \[*\] ]]; then
        printf '%s[%s]' "${user}" "${host}"
    else
        printf '%s%s' "${user}" "${host}"
    fi
}
readonly SCP_BOARD="$(scp_board_target)"
for file in "${BUILD_DIR}/socchina_app" "${MODEL}" \
    "${REPO_ROOT}/scripts/board/socchina-p1-validate"; do
    [[ -f "${file}" ]] || {
        echo "[p1] missing file: ${file}" >&2
        exit 1
    }
done

echo "[p1] upload validation assets to ${BOARD}:${RUN_DIR}"
ssh "${BOARD}" "mkdir -p '${RUN_DIR}'"
scp "${BUILD_DIR}/socchina_app" "${MODEL}" \
    "${REPO_ROOT}/scripts/board/socchina-p1-validate" "${SCP_BOARD}:${RUN_DIR}/"
ssh -t "${BOARD}" \
    "chmod +x '${RUN_DIR}/socchina_app' '${RUN_DIR}/socchina-p1-validate' && \
     SOCCHINA_P1_DIR='${RUN_DIR}' '${RUN_DIR}/socchina-p1-validate' '${DURATION}'"
