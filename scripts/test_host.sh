#!/usr/bin/env bash
# 主机单元测试：用本机 cc 直接编译并运行 SDK-free 的纯逻辑单元测试。
# 触 SDK/硬件 的模块不在此列，需交叉编译后部署到板端手动测。
# 用法：scripts/test_host.sh
set -euo pipefail

BOARD="$(cd "$(dirname "${BASH_SOURCE[0]}")/../board" && pwd)"
OUT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build-host"
mkdir -p "${OUT}"

# SDK-free 单元：<名字> 对应 tests/test_<名字>.c + src/<名字>.c
HOST_UNITS=(control)

fail=0
for u in "${HOST_UNITS[@]}"; do
    bin="${OUT}/test_${u}"
    cc -std=c11 -Wall -Wextra -I"${BOARD}/include" \
        "${BOARD}/tests/test_${u}.c" "${BOARD}/src/${u}.c" -o "${bin}"
    echo "== running test_${u} =="
    "${bin}" || fail=1
done
exit "${fail}"
