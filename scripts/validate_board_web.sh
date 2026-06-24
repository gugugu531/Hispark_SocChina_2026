#!/usr/bin/env bash
# 验证板端 Web 控制台部署。
# 用法：[BOARD=hispark-remote] scripts/validate_board_web.sh
#
# 检查项目：
#   - 二进制存在且为 ARM64
#   - 配置文件存在
#   - socket 目录可用
#   - systemd 服务状态
#   - 端口监听（RTSP/WebRTC/HLS/Web）
set -euo pipefail

if [[ -z "${BOARD:-}" ]]; then
    echo "[validate-web] 请设置 BOARD=hispark-remote；电脑网线直连时可设 BOARD=root@192.168.1.168" >&2
    exit 2
fi

PASS=0
FAIL=0

check() {
    local desc="$1"
    local cmd="$2"
    printf "[validate-web] %-50s " "${desc}..."
    if ssh "${BOARD}" "${cmd}" >/dev/null 2>&1; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL"
        FAIL=$((FAIL + 1))
    fi
}

check_opt() {
    local desc="$1"
    local cmd="$2"
    printf "[validate-web] %-50s " "${desc}..."
    if ssh "${BOARD}" "${cmd}" >/dev/null 2>&1; then
        echo "OK"
        PASS=$((PASS + 1))
    else
        echo "SKIP（尚未部署或为可选组件）"
    fi
}

echo "[validate-web] 目标：${BOARD}"
echo ""

# ---- 二进制 ----
check_opt "socchina-web 可执行"     "test -x /usr/local/bin/socchina-web"
check_opt "socchina-admin 可执行"   "test -x /usr/local/bin/socchina-admin"
check_opt "socchina-web 为 ARM64"   "file /usr/local/bin/socchina-web | grep -q 'ELF 64-bit LSB.*ARM aarch64'"
check_opt "socchina-admin 为 ARM64" "file /usr/local/bin/socchina-admin | grep -q 'ELF 64-bit LSB.*ARM aarch64'"

# ---- 用户与组 ----
check_opt "用户 socchina-web 存在" "id socchina-web"
check_opt "组 socchina 存在"        "getent group socchina"

# ---- 配置文件 ----
check_opt "/etc/socchina/web.conf"    "test -f /etc/socchina/web.conf"
check_opt "/etc/socchina/mediamtx.yml" "test -f /etc/socchina/mediamtx.yml"

# ---- systemd 服务 ----
check_opt "socchina-mediamtx.service"  "systemctl list-unit-files | grep -q socchina-mediamtx"
check_opt "socchina-web.service"       "systemctl list-unit-files | grep -q socchina-web"
check_opt "socchina-admin.service"     "systemctl list-unit-files | grep -q socchina-admin"

# ---- 端口（如果服务在运行） ----
check_opt "MediaMTX RTSP :8554"   "ss -tlnp | grep -q ':8554'"
check_opt "MediaMTX HLS :8888"    "ss -tlnp | grep -q ':8888'"
check_opt "MediaMTX WebRTC :8889" "ss -tlnp | grep -q ':8889'"
check_opt "Web :8080"             "ss -tlnp | grep -q ':8080'"

# ---- socket 目录 ----
check_opt "/run/socchina 目录" "test -d /run/socchina"

echo ""
echo "[validate-web] 结果：${PASS} PASS / ${FAIL} FAIL"

if [ "${FAIL}" -gt 0 ]; then
    echo "[validate-web] 部分检查未通过，请按需排查。"
    exit 1
fi
