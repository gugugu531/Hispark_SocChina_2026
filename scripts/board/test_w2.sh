#!/usr/bin/env bash
# W2 cold config smoke test — toggle HDMI on then off
set -euo pipefail

CFG='{"CONFIG_VERSION":"1","CAPTURE_MODE":"linear","TARGET_FPS":"30","BITRATE_KBPS":"3000","TONE_STRENGTH":"0.25","ENABLE_NN_CONTROL":"1","RTSP_BIND_ADDR":"127.0.0.1","RTSP_PORT":"8555","STREAM_PATH":"internal","SENSOR_INDEX":"1"}'

echo "=== 当前 HDMI ==="
grep ENABLE_HDMI /etc/socchina/runtime.conf

echo "=== HDMI=1 应用 ==="
curl -s -X POST -d "$(echo "$CFG" | python3 -c "import sys,json;d=json.load(sys.stdin);d['ENABLE_HDMI']='1';print(json.dumps(d))")" http://127.0.0.1:8080/api/v1/config/apply
echo ""
sleep 10
grep ENABLE_HDMI /etc/socchina/runtime.conf
systemctl status socchina-stream --no-pager | head -6

echo "=== HDMI=0 恢复 ==="
curl -s -X POST -d "$(echo "$CFG" | python3 -c "import sys,json;d=json.load(sys.stdin);d['ENABLE_HDMI']='0';print(json.dumps(d))")" http://127.0.0.1:8080/api/v1/config/apply
echo ""
sleep 10
grep ENABLE_HDMI /etc/socchina/runtime.conf
echo "=== DONE ==="
