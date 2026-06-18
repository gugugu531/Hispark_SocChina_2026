#!/usr/bin/env bash
# 在目标板上运行生产主程序或指定板端程序。
# 用法：[BOARD=hispark-remote] [DEST=/root/socchina-2026] scripts/run_board.sh [程序 [参数...]]
#   默认运行 socchina_app（相机→ISP+Gamma→HDMI 实时曝光校正，Ctrl-C 退出）。
#   例：scripts/run_board.sh test_infer --iters 30
#
# 自动设置 LD_LIBRARY_PATH 含 /opt/lib/npu（libascendcl 等 NPU 运行时）。
# 注意：起链前确保板上无其他媒体进程争用 SYS/VB/VI/VPSS/VO（见 docs/board-operations.md）。
set -euo pipefail

if [[ -z "${BOARD:-}" ]]; then
    echo "[run] 请设置 BOARD=hispark-remote；电脑网线直连时可设 BOARD=root@192.168.1.168" >&2
    exit 2
fi
DEST="${DEST:-/root/socchina-2026}"
PROG="${1:-socchina_app}"
shift || true

echo "[run] ${BOARD}:${DEST}/${PROG} $*"
# shellcheck disable=SC2029
ssh -t "${BOARD}" "cd '${DEST}' && LD_LIBRARY_PATH=/opt/lib/npu:\${LD_LIBRARY_PATH:-} ./${PROG} $*"
