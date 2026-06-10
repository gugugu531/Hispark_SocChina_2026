#include "capture.h"

#include "log.h"

#ifdef WITH_SS928_SDK

#include <stdlib.h>

#include "sample_comm.h"
#include "ss_mpi_isp.h"

#define CAPTURE_VI_PIPE 0
#define CAPTURE_VI_CHN  0

static sample_vi_cfg g_vi_cfg;
static int g_capture_inited = 0;

/* 使能 sensor 时钟（厂商 bspmm 工具写时钟寄存器；与已验证的相机点亮流程一致，
 * 不配置会导致 VPSS 取帧超时）。sensor1 与 sensor0 的时钟寄存器不同。 */
static void capture_enable_sensor_clock(int sensor_index)
{
    const char *cmd = (sensor_index == 1)
        ? "command -v bspmm >/dev/null 2>&1 && bspmm 0x11018460 0x4001 >/dev/null 2>&1"
        : "command -v bspmm >/dev/null 2>&1 && bspmm 0x11018440 0x4001 >/dev/null 2>&1";
    if (system(cmd) != 0) {
        LOG_WARN("sensor clock setup via bspmm failed (sensor_index=%d)", sensor_index);
    }
}

/* 按板上传感器位生成 VI 配置：index 1 = sensor1/J4（bus7、clk/rst 源 1、VI dev2）。 */
static void capture_build_vi_cfg(sample_sns_type sns_type, int sensor_index, sample_vi_cfg *vi_cfg)
{
    const ot_vi_dev vi_dev = (sensor_index == 1) ? 2 : 0;
    const ot_vi_pipe vi_pipe = CAPTURE_VI_PIPE;

    sample_comm_vi_get_default_vi_cfg(sns_type, vi_cfg);
    vi_cfg->mipi_info.divide_mode = LANE_DIVIDE_MODE_1;
    if (sensor_index == 1) {
        vi_cfg->sns_info.bus_id = 7;
        vi_cfg->sns_info.sns_clk_src = 1;
        vi_cfg->sns_info.sns_rst_src = 1;
    } else {
        vi_cfg->sns_info.bus_id = 5;
        vi_cfg->sns_info.sns_clk_src = 0;
        vi_cfg->sns_info.sns_rst_src = 0;
    }
    sample_comm_vi_get_mipi_info_by_dev_id(sns_type, vi_dev, &vi_cfg->mipi_info);
    vi_cfg->dev_info.vi_dev = vi_dev;
    vi_cfg->bind_pipe.pipe_id[0] = vi_pipe;
    vi_cfg->grp_info.grp_num = 1;
    vi_cfg->grp_info.fusion_grp[0] = 0;
    vi_cfg->grp_info.fusion_grp_attr[0].pipe_id[0] = vi_pipe;
}

int capture_query_in_size(unsigned *width, unsigned *height)
{
    ot_size size = {0};

    if (width == NULL || height == NULL) {
        return -1;
    }
    sample_comm_vi_get_size_by_sns_type(SENSOR0_TYPE, &size);
    *width = size.width;
    *height = size.height;
    return 0;
}

int capture_init(int sensor_index)
{
    if (g_capture_inited) {
        LOG_WARN("capture already inited");
        return 0;
    }
    if (sensor_index != 0 && sensor_index != 1) {
        LOG_ERR("capture_init: invalid sensor_index %d", sensor_index);
        return -1;
    }

    capture_enable_sensor_clock(sensor_index);

    /* VI 在线直通 VPSS（零 DDR 往返），CPU 取帧统一走 VPSS 通道。 */
    CHECK_RET_GOTO(sample_comm_vi_set_vi_vpss_mode(OT_VI_ONLINE_VPSS_OFFLINE, OT_VI_VIDEO_MODE_NORM),
                   fail);

    capture_build_vi_cfg(SENSOR0_TYPE, sensor_index, &g_vi_cfg);

    /* 上次异常退出可能残留 ISP0 内存初始化状态；ISP 空闲时 exit 无害，
     * 可避免重启时 isp_mem_init 报 0xa01c800c / already inited。 */
    (void)ss_mpi_isp_exit(CAPTURE_VI_PIPE);
    if (sample_comm_vi_start_vi(&g_vi_cfg) != TD_SUCCESS) {
        LOG_ERR("sample_comm_vi_start_vi failed");
        (void)ss_mpi_isp_exit(CAPTURE_VI_PIPE);
        goto fail;
    }

    g_capture_inited = 1;
    LOG_INFO("capture up: OS08A20 sensor_index=%d (vi_dev=%d, linear mode)", sensor_index,
             (sensor_index == 1) ? 2 : 0);
    return 0;

fail:
    return -1;
}

int capture_bind_vpss(int vpss_grp)
{
    if (!g_capture_inited) {
        LOG_ERR("capture_bind_vpss: capture not inited");
        return -1;
    }
    CHECK_RET_GOTO(sample_comm_vi_bind_vpss(CAPTURE_VI_PIPE, CAPTURE_VI_CHN, vpss_grp, 0), fail);
    return 0;
fail:
    return -1;
}

int capture_unbind_vpss(int vpss_grp)
{
    CHECK_RET_GOTO(sample_comm_vi_un_bind_vpss(CAPTURE_VI_PIPE, CAPTURE_VI_CHN, vpss_grp, 0), fail);
    return 0;
fail:
    return -1;
}

int capture_deinit(void)
{
    if (!g_capture_inited) {
        return 0;
    }
    sample_comm_vi_stop_vi(&g_vi_cfg);
    g_capture_inited = 0;
    LOG_INFO("capture down");
    return 0;
}

#else /* !WITH_SS928_SDK */

/* SDK-free 构建桩：保证 ENABLE_SDK=OFF 时库仍可编译链接。 */

int capture_query_in_size(unsigned *width, unsigned *height)
{
    (void)width;
    (void)height;
    LOG_ERR("capture_query_in_size: built without SS928 SDK");
    return -1;
}

int capture_init(int sensor_index)
{
    (void)sensor_index;
    LOG_ERR("capture_init: built without SS928 SDK");
    return -1;
}

int capture_bind_vpss(int vpss_grp)
{
    (void)vpss_grp;
    return -1;
}

int capture_unbind_vpss(int vpss_grp)
{
    (void)vpss_grp;
    return -1;
}

int capture_deinit(void)
{
    return 0;
}

#endif /* WITH_SS928_SDK */
