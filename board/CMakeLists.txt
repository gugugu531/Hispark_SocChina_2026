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
    target_include_directories(socchina PUBLIC "${SS928_SDK_ROOT}/include")
    target_link_directories(socchina PUBLIC "${SS928_SDK_ROOT}/lib")
    target_compile_definitions(socchina PUBLIC WITH_SS928_SDK=1)
    # 接入硬件时追加具体库，例如： target_link_libraries(socchina PUBLIC ss_mpi securec ...)
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
