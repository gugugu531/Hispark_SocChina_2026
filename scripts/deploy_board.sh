#!/usr/bin/env bash
# 部署板端产物到目标板。先 build_board.sh 出产物，再 scp 二进制 + 模型/LUT 到板上目录。
# 用法：[BOARD=hispark-remote] [DEST=/root/socchina-2026] scripts/deploy_board.sh [额外文件...]
#
# 默认部署：socchina_app + build/test_*（板端测试）。额外文件（OM/LUT/.bin）作参数追加。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
if [[ -z "${BOARD:-}" ]]; then
    echo "[deploy] 请设置 BOARD=hispark-remote；电脑网线直连时可设 BOARD=root@192.168.1.168" >&2
    exit 2
fi
DEST="${DEST:-/root/socchina-2026}"

if [ ! -x "${BUILD_DIR}/socchina_app" ]; then
    echo "[deploy] 未找到 ${BUILD_DIR}/socchina_app，请先运行 scripts/build_board.sh" >&2
    exit 1
fi

echo "[deploy] 目标：${BOARD}:${DEST}"
ssh "${BOARD}" "mkdir -p '${DEST}'"

# 主程序 + 所有板端测试可执行
FILES=("${BUILD_DIR}/socchina_app")
for t in "${BUILD_DIR}"/test_*; do
    [ -x "$t" ] && [ ! -d "$t" ] && FILES+=("$t")
done
# 额外资源（模型 OM / LUT bin 等）由参数指定
for extra in "$@"; do
    FILES+=("${extra}")
done

echo "[deploy] 传输 ${#FILES[@]} 个文件..."
scp "${FILES[@]}" "${BOARD}:${DEST}/"
echo "[deploy] 完成。板端运行见 scripts/run_board.sh"
