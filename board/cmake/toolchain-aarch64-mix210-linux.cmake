# 交叉编译工具链文件：aarch64-mix210-linux（海思 SS928/SD3403 SDK 工具链）
# 用法：cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-mix210-linux.cmake ...
# 编译器需在 PATH 中（由 scripts/env.sh 加入），或用 -DCROSS_COMPILE_ROOT=<工具链根> 指定其 bin 目录。

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CROSS_PREFIX aarch64-mix210-linux)

# 定位工具链 bin 目录：优先 -DCROSS_COMPILE_ROOT，其次环境变量，否则从 PATH 查找
if(DEFINED CROSS_COMPILE_ROOT)
    set(_cross_bin "${CROSS_COMPILE_ROOT}/bin/")
elseif(DEFINED ENV{CROSS_COMPILE_ROOT})
    set(_cross_bin "$ENV{CROSS_COMPILE_ROOT}/bin/")
else()
    set(_cross_bin "")
endif()

set(CMAKE_C_COMPILER   "${_cross_bin}${CROSS_PREFIX}-gcc")
set(CMAKE_CXX_COMPILER "${_cross_bin}${CROSS_PREFIX}-g++")

# 交叉编译常规设置：程序在主机找，库/头在目标 sysroot 找
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
