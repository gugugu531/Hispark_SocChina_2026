#!/usr/bin/env bash
# 部署 Web 控制台到目标板。
# 用法：[BOARD=hispark-remote] scripts/install_board_web.sh
#
# 先运行 scripts/build_web.sh 出产物。
# 部署内容：
#   - Go 二进制 → /usr/local/bin/
#   - 配置文件   → /etc/socchina/
#   - 静态页面   → /usr/share/socchina-web/
#   - systemd 服务文件
#   - 非特权用户 socchina-web（如不存在则创建）
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/web"

if [[ -z "${BOARD:-}" ]]; then
    echo "[install-web] 请设置 BOARD=hispark-remote；电脑网线直连时可设 BOARD=root@192.168.1.168" >&2
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
SCP_BOARD="$(scp_board_target)"

echo "[install-web] 目标：${BOARD}"

# ---- 板端目录 ----
ssh "${BOARD}" "mkdir -p /etc/socchina /usr/share/socchina-web /usr/local/bin /var/lib/socchina/config"

# ---- 二进制（如果存在） ----
if [ -x "${BUILD_DIR}/socchina-web" ]; then
    echo "[install-web] 传输 socchina-web..."
    scp "${BUILD_DIR}/socchina-web" "${SCP_BOARD}:/usr/local/bin/"
    ssh "${BOARD}" "chmod 755 /usr/local/bin/socchina-web"
fi
if [ -x "${BUILD_DIR}/socchina-admin" ]; then
    echo "[install-web] 传输 socchina-admin..."
    scp "${BUILD_DIR}/socchina-admin" "${SCP_BOARD}:/usr/local/bin/"
    ssh "${BOARD}" "chmod 755 /usr/local/bin/socchina-admin"
fi

# ---- 配置文件 ----
echo "[install-web] 传输配置文件..."
if [ -f "${BUILD_DIR}/web.conf" ]; then
    scp "${BUILD_DIR}/web.conf" "${SCP_BOARD}:/etc/socchina/"
else
    scp "${REPO_ROOT}/deploy/web.conf" "${SCP_BOARD}:/etc/socchina/"
fi
if [ -f "${REPO_ROOT}/deploy/mediamtx.yml" ]; then
    scp "${REPO_ROOT}/deploy/mediamtx.yml" "${SCP_BOARD}:/etc/socchina/"
fi

# ---- hls.js（如不存在则下载） ----
VENDOR_DIR="${REPO_ROOT}/web/ui/vendor"
HLS_JS="${VENDOR_DIR}/hls.min.js"
if [ ! -f "${HLS_JS}" ]; then
    echo "[install-web] 下载 hls.js..."
    curl -sL -o "${HLS_JS}" "https://cdn.jsdelivr.net/npm/hls.js@1.5/dist/hls.min.js" || {
        echo "[install-web] 警告: hls.js 下载失败；HLS 播放将不可用。可手动下载后放入 ${VENDOR_DIR}"
    }
fi

# ---- 静态资源 ----
if [ -d "${REPO_ROOT}/web/ui" ]; then
    echo "[install-web] 传输静态资源..."
    ssh "${BOARD}" "mkdir -p /usr/share/socchina-web/assets"
    if ls "${REPO_ROOT}/web/ui/"*.html >/dev/null 2>&1; then
        scp "${REPO_ROOT}/web/ui/"*.html "${SCP_BOARD}:/usr/share/socchina-web/"
    fi
    if ls "${REPO_ROOT}/web/ui/"*.js >/dev/null 2>&1; then
        scp "${REPO_ROOT}/web/ui/"*.js "${SCP_BOARD}:/usr/share/socchina-web/"
    fi
    if ls "${REPO_ROOT}/web/ui/"*.css >/dev/null 2>&1; then
        scp "${REPO_ROOT}/web/ui/"*.css "${SCP_BOARD}:/usr/share/socchina-web/"
    fi
    if [ -d "${REPO_ROOT}/web/ui/vendor" ] && ls "${REPO_ROOT}/web/ui/vendor/"* >/dev/null 2>&1; then
        scp -r "${REPO_ROOT}/web/ui/vendor/"* "${SCP_BOARD}:/usr/share/socchina-web/assets/"
    fi
fi

# ---- 非特权用户 ----
echo "[install-web] 检查用户 socchina-web..."
ssh "${BOARD}" "id socchina-web >/dev/null 2>&1 || useradd -r -s /usr/sbin/nologin -d /var/lib/socchina socchina-web"
ssh "${BOARD}" "getent group socchina >/dev/null 2>&1 || groupadd -f socchina"
ssh "${BOARD}" "usermod -a -G socchina socchina-web 2>/dev/null || true"

# ---- systemd 服务 ----
echo "[install-web] 安装 systemd 服务..."
scp "${REPO_ROOT}/deploy/systemd/socchina-mediamtx.service" "${SCP_BOARD}:/etc/systemd/system/"
scp "${REPO_ROOT}/deploy/systemd/socchina-web.service" "${SCP_BOARD}:/etc/systemd/system/"
scp "${REPO_ROOT}/deploy/systemd/socchina-admin.service" "${SCP_BOARD}:/etc/systemd/system/"
ssh "${BOARD}" "systemctl daemon-reload"

echo "[install-web] 完成。"
echo ""
echo "启用服务："
echo "  ssh ${BOARD} systemctl enable --now socchina-mediamtx.service"
echo "  ssh ${BOARD} systemctl enable --now socchina-web.service"
echo "  ssh ${BOARD} systemctl enable --now socchina-admin.service"
echo ""
echo "验证：scripts/validate_board_web.sh"
