add_executable(socchina_app src/main.c)
target_link_libraries(socchina_app PRIVATE board_common mod_control)

# 海思 SS928 SDK（可选）：接入硬件模块后启用。
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
