#!/usr/bin/env bash
# 在目标板上安装 MediaMTX + 更新 socchina-start + 更新 runtime.conf
# 用法：BOARD=sealbird bash scripts/install_mediamtx.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [[ -z "${BOARD:-}" ]]; then
    echo "[install-mediamtx] 请设置 BOARD=sealbird" >&2
    exit 2
fi

MTX_DIR=/opt/socchina/mediamtx
SYSTEMD_DIR=/etc/systemd/system
CONF_DIR=/etc/socchina

echo "[install-mediamtx] 创建目录 ${MTX_DIR} ..."
ssh "${BOARD}" "mkdir -p '${MTX_DIR}' '${CONF_DIR}'"

echo "[install-mediamtx] 传输 mediamtx 二进制（~60MB）..."
scp /tmp/mediamtx-dl/mediamtx "${BOARD}:${MTX_DIR}/mediamtx"
ssh "${BOARD}" "chmod +x '${MTX_DIR}/mediamtx'"

echo "[install-mediamtx] 传输 mediamtx.yml ..."
scp "${REPO_ROOT}/deploy/mediamtx/mediamtx.yml" "${BOARD}:${MTX_DIR}/mediamtx.yml"

echo "[install-mediamtx] 传输 systemd service ..."
scp "${REPO_ROOT}/deploy/systemd/socchina-mediamtx.service" "${BOARD}:${SYSTEMD_DIR}/socchina-mediamtx.service"

echo "[install-mediamtx] 更新 runtime.conf ..."
# 只在文件不存在时安装；若已存在则比较差异后手动合并（避免覆盖板端调参）
if ssh "${BOARD}" "[ ! -f '${CONF_DIR}/runtime.conf' ]"; then
    scp "${REPO_ROOT}/deploy/systemd/runtime.conf" "${BOARD}:${CONF_DIR}/runtime.conf"
    echo "[install-mediamtx] runtime.conf 首次安装完成"
else
    echo "[install-mediamtx] runtime.conf 已存在，跳过覆盖。如需更新请手动 scp:"
    echo "  scp deploy/systemd/runtime.conf ${BOARD}:${CONF_DIR}/runtime.conf"
fi

echo "[install-mediamtx] 更新 socchina-start ..."
scp "${REPO_ROOT}/deploy/socchina-start" "${BOARD}:/usr/local/sbin/socchina-start"
ssh "${BOARD}" "chmod +x /usr/local/sbin/socchina-start"

echo "[install-mediamtx] reload systemd ..."
ssh "${BOARD}" "systemctl daemon-reload && systemctl enable socchina-mediamtx.service"

echo "[install-mediamtx] 完成！"
echo ""
echo "启动步骤："
echo "  1. systemctl restart socchina-stream    # 重启主程序（用新 runtime.conf 的 8555+loopback）"
echo "  2. systemctl start socchina-mediamtx    # 启动 MediaMTX"
echo ""
echo "验证："
echo "  VLC:     rtsp://192.168.1.168:8554/live"
echo "  HLS:     http://192.168.1.168:8888/live"
echo "  WebRTC:  http://192.168.1.168:8889/live"
