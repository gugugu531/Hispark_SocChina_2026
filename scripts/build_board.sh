#!/usr/bin/env bash
# 板端应用统一构建入口。
# 用法：scripts/build_board.sh [Debug|Release]   （默认 Release）
# 环境变量见 scripts/env.sh：CROSS_COMPILE_ROOT / ISL_LIB_DIR / SS928_SDK_ROOT
#   - 未设置 SS928_SDK_ROOT 时，仅构建最小程序以验证工具链（不链接 SDK）。
#   - 设置 SS928_SDK_ROOT 后自动开启 ENABLE_SDK。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_TYPE="${1:-Release}"
BUILD_DIR="${REPO_ROOT}/build"

# shellcheck source=scripts/env.sh
source "${SCRIPT_DIR}/env.sh"

CMAKE_ARGS=(
    -S "${REPO_ROOT}/board"
    -B "${BUILD_DIR}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/board/cmake/toolchain-aarch64-mix210-linux.cmake"
)
if [ -n "${CROSS_COMPILE_ROOT:-}" ]; then
    CMAKE_ARGS+=( -DCROSS_COMPILE_ROOT="${CROSS_COMPILE_ROOT}" )
fi
if [ -n "${SS928_SDK_ROOT:-}" ]; then
    CMAKE_ARGS+=( -DENABLE_SDK=ON -DSS928_SDK_ROOT="${SS928_SDK_ROOT}" )
fi

echo "[build] 配置 (${BUILD_TYPE})..."
cmake "${CMAKE_ARGS[@]}"
echo "[build] 编译..."
cmake --build "${BUILD_DIR}" -j"$(nproc)"
echo "[build] 完成，产物目录：${BUILD_DIR}"
