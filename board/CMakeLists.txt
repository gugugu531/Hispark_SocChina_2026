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
    if(EXISTS "${SS928_SDK_ROOT}/include/hisilicon/npu")
        list(APPEND SDK_INC_DIRS "${SS928_SDK_ROOT}/include/hisilicon/npu") # ACL（infer.c）
    endif()
    set(SDK_LIB_DIRS "${SS928_SDK_ROOT}/lib")
    if(EXISTS "${SS928_SDK_ROOT}/lib/linux/hisilicon")
        list(APPEND SDK_LIB_DIRS "${SS928_SDK_ROOT}/lib/linux/hisilicon")
    endif()
    if(EXISTS "${SS928_SDK_ROOT}/lib/linux/hisilicon/npu")
        list(APPEND SDK_LIB_DIRS "${SS928_SDK_ROOT}/lib/linux/hisilicon/npu") # libascendcl 等
    endif()
    target_include_directories(socchina PUBLIC ${SDK_INC_DIRS})
    target_link_directories(socchina PUBLIC ${SDK_LIB_DIRS})
    target_compile_definitions(socchina PUBLIC WITH_SS928_SDK=1)

    # 厂商 sample_comm 层（MIPI/sensor 注册/ISP 线程/VPSS 起链封装，capture/vpss 模块依赖）。
    # 要求 SS928_SDK_ROOT 指向含 src/common 的 MPP sample 包。
    set(VENDOR_COMM_DIR "${SS928_SDK_ROOT}/src/common")
    if(NOT EXISTS "${VENDOR_COMM_DIR}/sample_comm.h")
        message(FATAL_ERROR "未找到 ${VENDOR_COMM_DIR}/sample_comm.h：SS928_SDK_ROOT 需指向含 src/common 的 MPP sample 包")
    endif()
    add_library(vendor_comm STATIC
        "${VENDOR_COMM_DIR}/sample_comm_sys.c"
        "${VENDOR_COMM_DIR}/sample_comm_isp.c"
        "${VENDOR_COMM_DIR}/sample_comm_vi.c"
        "${VENDOR_COMM_DIR}/sample_comm_vpss.c")
    target_include_directories(vendor_comm PUBLIC "${VENDOR_COMM_DIR}" ${SDK_INC_DIRS})
    foreach(_d exp_inc osal/include)
        if(EXISTS "${SS928_SDK_ROOT}/include/hisilicon/${_d}")
            target_include_directories(vendor_comm PUBLIC "${SS928_SDK_ROOT}/include/hisilicon/${_d}")
        endif()
    endforeach()
    # 宏与传感器型号沿用已验证的相机点亮构建配方（OS08A20 8M30 12bit 线性，在 SENSOR0_TYPE 槽位）
    target_compile_definitions(vendor_comm PUBLIC
        ss928v100 OT_XXXX DISP_V2 OT_RELEASE VER_X=1 VER_Y=0 VER_Z=0 VER_P=0 VER_B=10
        USER_BIT_64 KERNEL_BIT_64 OT_ACODEC_TYPE_HDMI OT_ACODEC_TYPE_INNER
        OT_VQE_USE_STATIC_MODULE_REGISTER OT_AAC_USE_STATIC_MODULE_REGISTER OT_AAC_HAVE_SBR_LIB
        SENSOR0_TYPE=OV_OS08A20_MIPI_8M_30FPS_12BIT
        SENSOR1_TYPE=SONY_IMX347_SLAVE_MIPI_4M_30FPS_12BIT
        SENSOR2_TYPE=HISI_GS1860_MIPI_1M_30FPS_10BIT
        SENSOR3_TYPE=HISI_GS1860_MIPI_1M_30FPS_10BIT)
    target_compile_options(vendor_comm PRIVATE -w) # 厂商代码不套用本仓库告警等级

    # 静态链接 MPP 库（与厂商 sample 一致）。vendor_comm 须排在 group 之前。
    # ISP/3A/sensor 库为 capture 链依赖；音频 VQE/AAC 库为 libss_mpi.a 的被动依赖，本应用不使用音频。
    target_link_libraries(socchina PUBLIC
        vendor_comm
        -Wl,--start-group -l:libss_mpi.a -l:libss_hdmi.a -l:libsecurec.a
        -l:libss_ae.a -l:libss_isp.a -l:libot_isp.a -l:libss_awb.a -l:libss_dehaze.a
        -l:libss_extend_stats.a -l:libss_drc.a -l:libss_ldci.a -l:libss_crb.a -l:libss_bnr.a
        -l:libss_calcflicker.a -l:libss_ir_auto.a -l:libss_acs.a
        -l:libsns_os08a20.a -l:libsns_os05a10_2l_slave.a -l:libsns_imx347_slave.a
        -l:libsns_imx347_2l_slave.a -l:libsns_imx485.a -l:libsns_os04a10.a -l:libsns_os08b10.a
        -l:libsns_sc450ai.a -l:libsns_sc450ai_2l.a -l:libsns_gs1860.a
        -l:libss_voice_engine.a -l:libss_upvqe.a -l:libss_dnvqe.a
        -l:libaac_comm.a -l:libaac_enc.a -l:libaac_dec.a -l:libaac_sbr_enc.a -l:libaac_sbr_dec.a
        -Wl,--end-group
        # NPU ACL（infer.c 跑 CoTF param-net）：动态链 libascendcl，其传递 .so 依赖运行时由
        # 板端 /opt/lib/npu 解析，链接期放宽未定义符号。
        -lascendcl -Wl,--allow-shlib-undefined
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
