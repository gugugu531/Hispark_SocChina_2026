# 主机 LVGL 模拟器构建(仅 PC,用于预览 board/ui)。
# 用法:
#   scripts/fetch_lvgl.sh
#   cmake -S board/sim -B board/sim/build && cmake --build board/sim/build -j
#   ./board/sim/build/socchina_sim
cmake_minimum_required(VERSION 3.13)
project(socchina_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

set(LVGL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/lvgl)
if(NOT EXISTS ${LVGL_DIR}/lvgl.h)
    message(FATAL_ERROR "LVGL 未拉取:先运行 scripts/fetch_lvgl.sh")
endif()

# LVGL 源码:排除所有 drivers 后只加回 SDL(除 egl,避免 GL/EGL 外部依赖)。
# 其余 src 各文件均由 lv_conf 的 #if 守卫,特性关闭时编译为空,无外部依赖。
file(GLOB_RECURSE LVGL_SRC ${LVGL_DIR}/src/*.c)
list(FILTER LVGL_SRC EXCLUDE REGEX "/src/drivers/")
file(GLOB SDL_SRC ${LVGL_DIR}/src/drivers/sdl/*.c)
list(FILTER SDL_SRC EXCLUDE REGEX "lv_sdl_egl")
list(APPEND LVGL_SRC ${SDL_SRC})

add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC
    ${LVGL_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}      # lv_conf.h (LV_CONF_INCLUDE_SIMPLE)
    ${SDL2_INCLUDE_DIRS})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES} m)

add_executable(socchina_sim
    main_sim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/ui_lvgl.c)
target_include_directories(socchina_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../ui)
target_link_libraries(socchina_sim PRIVATE lvgl)
