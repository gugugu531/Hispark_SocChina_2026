cmake_minimum_required(VERSION 3.16)
project(socchina2026_board LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()
add_compile_options(-Wall -Wextra)
enable_testing()

# 海思 SS928 SDK（外部依赖，可选）。设 -DENABLE_SDK=ON -DSS928_SDK_ROOT=<路径> 接入硬件。
option(ENABLE_SDK "链接海思 SS928 SDK（需 SS928_SDK_ROOT）" OFF)

# 库：除 main.c 外的所有源码（供主程序与测试共用）
file(GLOB LIB_SRC CONFIGURE_DEPENDS src/*.c)
list(REMOVE_ITEM LIB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c")
add_library(socchina STATIC ${LIB_SRC})
target_include_directories(socchina PUBLIC include)

if(ENABLE_SDK)
    if(NOT SS928_SDK_ROOT)
        message(FATAL_ERROR "ENABLE_SDK=ON 时必须设置 -DSS928_SDK_ROOT=<SDK 根目录>")
    endif()
    # 兼容两种 SDK 目录布局：<root>/include[/hisilicon] 与 <root>/lib[/linux/hisilicon]
    set(SDK_INC_DIRS "${SS928_SDK_ROOT}/include")
    if(EXISTS "${SS928_SDK_ROOT}/include/hisilicon")
        list(APPEND SDK_INC_DIRS "${SS928_SDK_ROOT}/include/hisilicon")
    endif()
    set(SDK_LIB_DIRS "${SS928_SDK_ROOT}/lib")
    if(EXISTS "${SS928_SDK_ROOT}/lib/linux/hisilicon")
        list(APPEND SDK_LIB_DIRS "${SS928_SDK_ROOT}/lib/linux/hisilicon")
    endif()
    target_include_directories(socchina PUBLIC ${SDK_INC_DIRS})
    target_link_directories(socchina PUBLIC ${SDK_LIB_DIRS})
    target_compile_definitions(socchina PUBLIC WITH_SS928_SDK=1)
    # 静态链接 MPP 库（与厂商 sample 一致），模块增多按需在 group 内追加。
    # 音频 VQE/AAC 库为 libss_mpi.a 内音频适配代码的被动依赖，本应用不使用音频。
    target_link_libraries(socchina PUBLIC
        -Wl,--start-group -l:libss_mpi.a -l:libss_hdmi.a -l:libsecurec.a
        -l:libss_voice_engine.a -l:libss_upvqe.a -l:libss_dnvqe.a
        -l:libaac_comm.a -l:libaac_enc.a -l:libaac_dec.a -l:libaac_sbr_enc.a -l:libaac_sbr_dec.a
        -Wl,--end-group
        pthread m dl)
endif()

# 主程序
add_executable(socchina_app src/main.c)
target_link_libraries(socchina_app PRIVATE socchina)
install(TARGETS socchina_app RUNTIME DESTINATION bin)

# 测试：tests/test_*.c 各编一个可执行并注册 ctest
file(GLOB TEST_SRC CONFIGURE_DEPENDS tests/test_*.c)
foreach(_t ${TEST_SRC})
    get_filename_component(_name ${_t} NAME_WE)
    add_executable(${_name} ${_t})
    target_link_libraries(${_name} PRIVATE socchina)
    add_test(NAME ${_name} COMMAND ${_name})
endforeach()
