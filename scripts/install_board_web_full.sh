#!/usr/bin/env bash
# SocChina Web 控制台一键安装
# 用法：BOARD=hispark-remote scripts/install_board_web_full.sh
set -euo pipefail

if [[ -z "${BOARD:-}" ]]; then
    echo "请设置 BOARD=hispark-remote 或 BOARD=root@<ip>"
    exit 2
fi

echo "=== SocChina Web 控制台安装 ==="
echo "目标: ${BOARD}"
echo ""

# 1. Create directories
echo "[1/6] 创建目录..."
ssh "${BOARD}" "mkdir -p /etc/socchina /usr/share/socchina-web/vendor /var/lib/socchina/config /usr/local/bin /root/socchina-2026"

# 2. MediaMTX
echo "[2/6] MediaMTX..."
if ! ssh "${BOARD}" "test -x /usr/local/bin/mediamtx"; then
    echo "  下载 MediaMTX ARM64..."
    ssh "${BOARD}" "cd /tmp && wget -q https://github.com/bluenviron/mediamtx/releases/download/v1.11.3/mediamtx_v1.11.3_linux_arm64v8.tar.gz && tar xzf mediamtx_v1.11.3_linux_arm64v8.tar.gz && mv mediamtx /usr/local/bin/ && chmod 755 /usr/local/bin/mediamtx"
fi

# 3. hls.js
echo "[3/6] 前端依赖..."
ssh "${BOARD}" "cd /usr/share/socchina-web/vendor && test -f hls.min.js || wget -q -O hls.min.js https://cdn.jsdelivr.net/npm/hls.js@1.5/dist/hls.min.js"

# 4. Copy configs
echo "[4/6] 配置文件..."
scp deploy/systemd/runtime.conf "${BOARD}:/etc/socchina/" 2>/dev/null || true
scp deploy/mediamtx.yml "${BOARD}:/etc/socchina/" 2>/dev/null || true
scp deploy/web.conf "${BOARD}:/etc/socchina/" 2>/dev/null || true

# 5. Copy systemd services
echo "[5/6] systemd 服务..."
for svc in socchina-stream socchina-mediamtx socchina-webapp socchina-auth socchina-admin; do
    scp "deploy/systemd/${svc}.service" "${BOARD}:/etc/systemd/system/" 2>/dev/null || true
done
ssh "${BOARD}" "systemctl daemon-reload"

# 6. Enable and start
echo "[6/6] 启用服务..."
ssh "${BOARD}" "
systemctl enable socchina-stream socchina-mediamtx socchina-webapp socchina-auth socchina-admin 2>/dev/null
systemctl start socchina-stream
sleep 3
systemctl start socchina-mediamtx socchina-webapp socchina-auth socchina-admin 2>/dev/null
sleep 2
"

echo ""
echo "=== 安装完成 ==="
echo "Web:  http://<board>:8080/"
echo "密码: socchina2026"
echo ""
echo "验证: BOARD=${BOARD} scripts/validate_board_web.sh"
