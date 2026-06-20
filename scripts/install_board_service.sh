#!/usr/bin/env bash
# 安装/更新板端 RTSP 自启动服务。运行配置仅在首次安装时创建，后续更新不会覆盖。
# 用法: BOARD=hispark-remote scripts/install_board_service.sh
set -euo pipefail

readonly REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly BUILD_DIR="${REPO_ROOT}/build"
readonly DEST="${DEST:-/root/socchina-2026}"
readonly STAGE="/tmp/socchina-service-install"
readonly DEFAULT_MODEL="${REPO_ROOT}/models/weights/cotf_paramnet_lcdp_rtx4060/cotf_paramnet_256x144_lcdp_best_e0167_fp16_aipp.om"
readonly MODEL_FILE="${MODEL_FILE:-${DEFAULT_MODEL}}"

if [[ -z "${BOARD:-}" ]]; then
    echo "[install-service] 请设置 BOARD=hispark-remote；电脑直连可设 BOARD=root@192.168.1.168" >&2
    exit 2
fi
if [[ ! -x "${BUILD_DIR}/socchina_app" ]]; then
    echo "[install-service] 未找到 ${BUILD_DIR}/socchina_app，请先运行 scripts/build_board.sh" >&2
    exit 1
fi

files=(
    "${BUILD_DIR}/socchina_app"
    "${REPO_ROOT}/deploy/systemd/socchina-stream.service"
    "${REPO_ROOT}/deploy/systemd/runtime.conf"
    "${REPO_ROOT}/scripts/board/socchina-start"
    "${REPO_ROOT}/scripts/board/socchina-display"
    "${MODEL_FILE}"
)
for file in "${files[@]}"; do
    [[ -f "${file}" ]] || {
        echo "[install-service] 缺少文件: ${file}" >&2
        exit 1
    }
done

echo "[install-service] 上传安装文件到 ${BOARD}:${STAGE}"
ssh "${BOARD}" "rm -rf '${STAGE}' && mkdir -p '${STAGE}'"
scp "${files[@]}" "${BOARD}:${STAGE}/"

echo "[install-service] 安装并启用 socchina-stream.service"
# shellcheck disable=SC2029
ssh "${BOARD}" "set -eu
    systemctl stop socchina-stream.service 2>/dev/null || true
    install -d -m 0755 '${DEST}' /etc/socchina /usr/local/sbin
    install -m 0755 '${STAGE}/socchina_app' '${DEST}/socchina_app'
    install -m 0755 '${STAGE}/socchina-start' /usr/local/sbin/socchina-start
    install -m 0755 '${STAGE}/socchina-display' /usr/local/sbin/socchina-display
    install -m 0644 '${STAGE}/$(basename "${MODEL_FILE}")' '${DEST}/cotf_paramnet_256x144_lcdp_best_e0167_fp16_aipp.om'
    install -m 0644 '${STAGE}/socchina-stream.service' /etc/systemd/system/socchina-stream.service
    if [ ! -e /etc/socchina/runtime.conf ]; then
        install -m 0644 '${STAGE}/runtime.conf' /etc/socchina/runtime.conf
    fi
    systemctl daemon-reload
    systemctl enable --now socchina-stream.service
    rm -rf '${STAGE}'
    systemctl --no-pager --full status socchina-stream.service
"

echo "[install-service] 完成。配置: /etc/socchina/runtime.conf"
echo "[install-service] HDMI: ssh ${BOARD} socchina-display on|off|status"
