#!/usr/bin/env bash
set -euo pipefail

# Remove sandbox overrides
rm -rf /etc/systemd/system/socchina-auth.service.d

# Simplify the auth service - no strict sandbox (needs /tmp socket access)
cat > /etc/systemd/system/socchina-auth.service << 'EOF'
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
EOF

systemctl daemon-reload
systemctl restart socchina-auth
sleep 3

echo "=== TEST ==="
curl -s -c /tmp/tc -X POST -d pass=socchina2026 http://127.0.0.1:8080/login -o /dev/null -w "login:%{http_code}\n"
curl -s -b /tmp/tc http://127.0.0.1:8080/api/v1/status 2>&1 | head -c 500
echo ""
echo "DONE"
