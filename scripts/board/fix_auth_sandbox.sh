#!/usr/bin/env bash
# Fix auth proxy sandbox to allow socket access
set -euo pipefail

mkdir -p /etc/systemd/system/socchina-auth.service.d
cat > /etc/systemd/system/socchina-auth.service.d/override.conf << 'EOF'
[Service]
ReadWritePaths=/tmp/socchina-app-control.sock
EOF
systemctl daemon-reload
systemctl restart socchina-auth
sleep 2
echo "=== TEST STATUS API ==="
curl -s -c /tmp/tc -X POST -d pass=socchina2026 http://127.0.0.1:8080/login -o /dev/null
curl -s -b /tmp/tc http://127.0.0.1:8080/api/v1/status | head -c 500
echo ""
echo "DONE"
