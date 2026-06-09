# 统一登记模块库与测试入口。
# 约定：每个模块目录含 include/ src/ tests/；模块编库(无 main)，tests/ 放测试入口(各自 main)。

# add_board_module(<name>)
#   收集当前目录 src/*.c|*.cpp 编成静态库 mod_<name>，公开本模块 include/ 与公共头。
function(add_board_module name)
    file(GLOB _srcs CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
    add_library(mod_${name} STATIC ${_srcs})
    target_include_directories(mod_${name} PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${BOARD_COMMON_INCLUDE}")
endfunction()

# add_board_test(<name> DEPS <libs...>)
#   tests/test_<name>.c -> 可执行 test_<name>，链接给定库，并注册到 ctest。
#   纯逻辑模块在主机原生构建(BOARD_HOST_TESTS)下可由 ctest 自动跑；
#   触 SDK/硬件 的模块交叉编译后部署到板端手动运行。
function(add_board_test name)
    cmake_parse_arguments(T "" "" "DEPS" ${ARGN})
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${name}.c")
    if(NOT EXISTS "${_src}")
        set(_src "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${name}.cpp")
    endif()
    add_executable(test_${name} "${_src}")
    target_link_libraries(test_${name} PRIVATE ${T_DEPS})
    add_test(NAME ${name} COMMAND test_${name})
endfunction()
