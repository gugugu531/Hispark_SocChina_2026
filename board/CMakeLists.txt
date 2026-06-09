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

# ---- 海思 SS928 SDK（外部依赖，可选）----
# 设置 -DENABLE_SDK=ON -DSS928_SDK_ROOT=<SDK路径> 后可链接 MPP/ISP/IVE/VGS/ACL 等库。
# 未启用时仍可构建最小程序，便于先验证工具链与构建闭环（新成员无需 SDK 即可上手）。
option(ENABLE_SDK "链接海思 SS928 SDK（需 SS928_SDK_ROOT）" OFF)

add_executable(socchina_app
    src/main.c
)
target_include_directories(socchina_app PRIVATE include)

if(ENABLE_SDK)
    if(NOT SS928_SDK_ROOT)
        message(FATAL_ERROR "ENABLE_SDK=ON 时必须设置 -DSS928_SDK_ROOT=<SDK 根目录>")
    endif()
    message(STATUS "链接 SS928 SDK: ${SS928_SDK_ROOT}")
    target_include_directories(socchina_app PRIVATE "${SS928_SDK_ROOT}/include")
    target_link_directories(socchina_app PRIVATE "${SS928_SDK_ROOT}/lib")
    target_compile_definitions(socchina_app PRIVATE WITH_SS928_SDK=1)
    # 各模块接入硬件时在此追加具体库，例如：
    # target_link_libraries(socchina_app PRIVATE ss_mpi securec ...)
endif()

install(TARGETS socchina_app RUNTIME DESTINATION bin)
