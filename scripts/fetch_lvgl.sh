#!/usr/bin/env sh
# 拉取固定版本 LVGL 到 board/sim/vendor/lvgl（外部依赖,不入库）。
# 用法: scripts/fetch_lvgl.sh [tag]
set -e
TAG="${1:-v9.5.0}"
DEST="$(cd "$(dirname "$0")/.." && pwd)/board/sim/vendor/lvgl"
if [ -d "$DEST/.git" ]; then
  echo "LVGL 已存在: $DEST ($(git -C "$DEST" describe --tags 2>/dev/null || echo unknown))"
  exit 0
fi
echo "clone LVGL $TAG -> $DEST"
git clone --depth 1 --branch "$TAG" https://github.com/lvgl/lvgl.git "$DEST"
echo "done: $(git -C "$DEST" describe --tags 2>/dev/null || echo "$TAG")"
