#!/usr/bin/env bash
set -euo pipefail

echo "#!/usr/bin/env bash
echo ebaina" > /tmp/ap
chmod +x /tmp/ap
export SSH_ASKPASS=/tmp/ap DISPLAY=dummy:0
B="root@2001:250:4000:822c:c634:4d5d:56b1:a1da"

scp -o StrictHostKeyChecking=no ~/Hispark_SocChina_2026/deploy/mediamtx.yml "$B:/etc/socchina/"

setsid ssh "$B" << 'BOARDCMD'
systemctl stop socchina-mediamtx 2>/dev/null || true
sleep 1
systemctl start socchina-mediamtx
sleep 3
echo "=== MediaMTX status ==="
systemctl status socchina-mediamtx | head -12
echo "=== Ports ==="
ss -tlnp | grep -E '8554|8888|8889|8080' || echo "no mediamtx ports"
BOARDCMD
