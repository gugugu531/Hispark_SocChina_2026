#!/usr/bin/env bash
# Add systemd sandbox to auth proxy service
set -euo pipefail

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

NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths=/tmp
PrivateTmp=yes
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
RestrictNamespaces=yes
LockPersonality=yes
RestrictRealtime=yes
RestrictSUIDSGID=yes
RemoveIPC=yes
SystemCallArchitectures=native
MemoryDenyWriteExecute=yes
ReadOnlyPaths=/usr/share/socchina-web

[Install]
WantedBy=multi-user.target
SVCEND

systemctl daemon-reload
systemctl restart socchina-auth
sleep 2
systemctl status socchina-auth | head -5
echo "=== TEST ==="
curl -s -c /tmp/tc -X POST -d pass=socchina2026 http://127.0.0.1:8080/login -o /dev/null -w "login:%{http_code}\n"
curl -s -b /tmp/tc -o /dev/null -w "dash:%{http_code}\n" http://127.0.0.1:8080/
echo "SANDBOX OK"
