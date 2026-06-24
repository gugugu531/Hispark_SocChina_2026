#!/usr/bin/env bash
# Web 控制台统一构建入口。
# 用法：scripts/build_web.sh [--release]
#
# 交叉编译 Go 二进制到 ARM64，产物放在 build/web/。
# 需要本地安装 Go 工具链（>=1.21）。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/web"

RELEASE=false
for arg in "$@"; do
    case "$arg" in
        --release) RELEASE=true ;;
    esac
done

LDFLAGS="-s -w"
if [ "${RELEASE}" = true ]; then
    LDFLAGS="${LDFLAGS} -X main.version=$(git -C "${REPO_ROOT}" describe --always --dirty 2>/dev/null || echo 'dev')"
fi

export GOOS=linux
export GOARCH=arm64
export CGO_ENABLED=0

echo "[build-web] 构建 socchina-web (ARM64)..."
mkdir -p "${BUILD_DIR}"
if [ -f "${REPO_ROOT}/web/backend/go.mod" ]; then
    (cd "${REPO_ROOT}/web/backend" && go build -ldflags "${LDFLAGS}" -o "${BUILD_DIR}/socchina-web" ./cmd/socchina-web/)
else
    echo "[build-web] 信息: web/backend/go.mod 不存在，跳过 socchina-web（尚未实现）"
fi

echo "[build-web] 构建 socchina-admin (ARM64)..."
if [ -f "${REPO_ROOT}/web/admin/go.mod" ]; then
    (cd "${REPO_ROOT}/web/admin" && go build -ldflags "${LDFLAGS}" -o "${BUILD_DIR}/socchina-admin" ./cmd/socchina-admin/)
else
    echo "[build-web] 信息: web/admin/go.mod 不存在，跳过 socchina-admin（尚未实现）"
fi

echo "[build-web] 构建 socchina-auth (ARM64)..."
if [ -f "${REPO_ROOT}/web/authproxy/go.mod" ]; then
    (cd "${REPO_ROOT}/web/authproxy" && go build -ldflags "${LDFLAGS}" -o "${BUILD_DIR}/socchina-auth" .)
else
    echo "[build-web] 信息: web/authproxy/go.mod 不存在，跳过 socchina-auth（尚未实现）"
fi

if [ -x "${BUILD_DIR}/socchina-web" ] || [ -x "${BUILD_DIR}/socchina-admin" ] || [ -x "${BUILD_DIR}/socchina-auth" ]; then
    file "${BUILD_DIR}"/* 2>/dev/null || true
    echo "[build-web] 完成，产物目录：${BUILD_DIR}"
else
    echo "[build-web] 无二进制产出（W1–W2 实现 Go 代码后将生成 ARM64 可执行文件）"
fi
