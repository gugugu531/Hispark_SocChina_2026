#!/usr/bin/env bash
# 统一环境入口：交叉工具链 + 主机依赖库。
# 用法：source scripts/env.sh
#
# 通过环境变量配置（不写死个人路径；各人按自己机器设置，建议放入 ~/.bashrc 或本地 .env）：
#   CROSS_COMPILE_ROOT  交叉工具链根目录（其下应有 bin/aarch64-mix210-linux-gcc）
#   ISL_LIB_DIR         提供 libisl.so.19 的目录（工具链前端依赖；若提示缺库时设置）
#   SS928_SDK_ROOT      海思 SS928 SDK 根目录（链接 MPP/ISP/IVE/VGS/ACL 时需要）

# 工具链加入 PATH
if [ -n "${CROSS_COMPILE_ROOT:-}" ] && [ -d "${CROSS_COMPILE_ROOT}/bin" ]; then
    export PATH="${CROSS_COMPILE_ROOT}/bin:${PATH}"
fi

# 工具链前端若提示缺 libisl.so.19，设置 ISL_LIB_DIR 指向其所在目录
if [ -n "${ISL_LIB_DIR:-}" ]; then
    export LD_LIBRARY_PATH="${ISL_LIB_DIR}:${LD_LIBRARY_PATH:-}"
fi

# 自检
if command -v aarch64-mix210-linux-gcc >/dev/null 2>&1; then
    echo "[env] 交叉编译器: $(command -v aarch64-mix210-linux-gcc)"
else
    echo "[env] 警告: 未找到 aarch64-mix210-linux-gcc；请设置 CROSS_COMPILE_ROOT 或将工具链 bin 加入 PATH" >&2
fi
if [ -n "${SS928_SDK_ROOT:-}" ]; then
    echo "[env] SS928_SDK_ROOT=${SS928_SDK_ROOT}"
fi

# 确保被 `set -e` 的脚本 source 时返回 0（末尾命令可能为假）
:
