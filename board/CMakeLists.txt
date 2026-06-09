cmake_minimum_required(VERSION 3.16)
project(socchina2026_board LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

add_compile_options(-Wall -Wextra)
enable_testing()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/helpers.cmake)
set(BOARD_COMMON_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/common/include")

# 海思 SS928 SDK（外部依赖，可选）。设 -DENABLE_SDK=ON -DSS928_SDK_ROOT=<路径> 接入硬件。
option(ENABLE_SDK "链接海思 SS928 SDK（需 SS928_SDK_ROOT）" OFF)

# 仅构建主机可测的 SDK-free 模块及其单元测试（本机原生编译，配合 scripts/test_host.sh）。
option(BOARD_HOST_TESTS "主机单元测试模式：只构建 SDK-free 模块与其测试" OFF)

# 公共设施（header-only：日志/计时/错误检查/版本）
add_library(board_common INTERFACE)
target_include_directories(board_common INTERFACE "${BOARD_COMMON_INCLUDE}")

# ---- SDK-free 模块（主机可单元测试）----
add_subdirectory(modules/control)

# ---- 完整(交叉)构建：触 SDK/硬件 的模块 + 最终应用 ----
if(NOT BOARD_HOST_TESTS)
    # 各 owner 在此登记自己的硬件模块，例如：
    #   add_subdirectory(modules/capture)
    #   add_subdirectory(modules/isp)
    #   add_subdirectory(modules/infer)
    add_subdirectory(app)
endif()
