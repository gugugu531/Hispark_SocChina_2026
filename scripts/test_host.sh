#!/usr/bin/env bash
# 主机单元测试入口：用本机原生编译器构建 SDK-free 模块及其测试，并用 ctest 运行。
# 仅覆盖纯逻辑模块（如 control）；触 SDK/硬件 的模块需交叉编译后部署到板端手动测。
# 用法：scripts/test_host.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-host"

# 不使用交叉 toolchain file -> 用本机原生编译器
cmake -S "${REPO_ROOT}/board" -B "${BUILD_DIR}" -DBOARD_HOST_TESTS=ON
cmake --build "${BUILD_DIR}" -j"$(nproc)"
ctest --test-dir "${BUILD_DIR}" --output-on-failure
