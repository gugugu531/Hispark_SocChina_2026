#!/usr/bin/env bash
# W4 systemd setup — stop manual processes, create services, enable auto-start
set -euo pipefail

# Stop manual processes
pkill -f socchina-auth 2>/dev/null || true
pkill -f "socchina-web -sock" 2>/dev/null || true
sleep 1

# Create systemd service for the legacy web console (port 8090)
cat > /etc/systemd/system/socchina-webapp.service << 'SVCEND'
[Unit]
Description=SocChina web console (legacy)
After=socchina-stream.service
Wants=socchina-stream.service

[Service]
Type=simple
ExecStart=/root/socchina-2026/socchina-web -sock /tmp/socchina-app-control.sock -addr :8090 -mtx http://127.0.0.1:8888
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
SVCEND

# Create systemd service for the auth proxy (port 8080)
cat > /etc/systemd/system/socchina-auth.service << 'SVCEND'
[Unit]
Description=SocChina auth proxy (W4)
After=socchina-webapp.service
Wants=socchina-webapp.service

[Service]
Type=simple
Environment=SOCCHINA_PASS=socchina2026
ExecStart=/usr/local/bin/socchina-auth -addr :8080 -backend http://127.0.0.1:8090
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
SVCEND

systemctl daemon-reload
systemctl enable --now socchina-webapp socchina-auth
sleep 3

echo "=== 服务状态 ==="
systemctl is-active socchina-stream socchina-mediamtx socchina-webapp socchina-auth socchina-admin
echo "=== 端口 ==="
ss -tlnp | grep -E "8080|8090|8554|8888"
echo "=== 认证测试 ==="
curl -s -o /dev/null -w "unauth: %{http_code}\n" http://127.0.0.1:8080/
curl -s -c /tmp/ck -X POST -d pass=socchina2026 http://127.0.0.1:8080/login -o /dev/null -w "login: %{http_code}\n"
curl -s -b /tmp/ck -o /dev/null -w "authed: %{http_code}\n" http://127.0.0.1:8080/
echo "=== DONE ==="
