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

bin="${OUT}/test_lut_bridge"
cc -std=c11 -Wall -Wextra -I"${BOARD}/include" \
    "${BOARD}/tests/test_lut_bridge.c" "${BOARD}/src/lut_bridge.c" -lm -o "${bin}"
echo "== running test_lut_bridge =="
"${bin}" || fail=1

# 流水线配置、状态与退出码契约测试。
bin="${OUT}/test_pipeline_contract"
cc -std=c11 -Wall -Wextra -I"${BOARD}/include" \
    "${BOARD}/tests/test_pipeline_contract.c" "${BOARD}/src/pipeline.c" -o "${bin}"
echo "== running test_pipeline_contract =="
"${bin}" || fail=1

bin="${OUT}/test_rtsp_server"
cc -std=c11 -Wall -Wextra -pthread -I"${BOARD}/include" \
    "${BOARD}/tests/test_rtsp_server.c" "${BOARD}/src/rtsp_server.c" -o "${bin}"
echo "== running test_rtsp_server =="
"${bin}" || fail=1

exit "${fail}"
