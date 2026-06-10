#include "display.h"

#include "log.h"

#ifdef WITH_SS928_SDK

#include <string.h>
#include <unistd.h>

#include "ot_buffer.h"
#include "ot_common.h"
#include "ot_common_hdmi.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_common_vo.h"
#include "ot_type.h"
#include "ss_mpi_hdmi.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_vo.h"

#define DISPLAY_VO_DEV   0
#define DISPLAY_VO_LAYER 0
#define DISPLAY_VO_CHN   0
#define DISPLAY_PIX_CLK_KHZ 49000 /* Waveshare 1024x600@60 原生像素时钟 49 MHz */

static int g_display_inited = 0;

/* 启动前清理可能残留的 VO/HDMI 状态（上次异常退出）；全部尽力而为，不检查返回值。 */
static void display_clear_residual(void)
{
    (void)ss_mpi_hdmi_stop(OT_HDMI_ID_0);
    (void)ss_mpi_hdmi_close(OT_HDMI_ID_0);
    (void)ss_mpi_hdmi_deinit();
    (void)ss_mpi_vo_disable_chn(DISPLAY_VO_LAYER, DISPLAY_VO_CHN);
    (void)ss_mpi_vo_disable_video_layer(DISPLAY_VO_LAYER);
    (void)ss_mpi_vo_disable(DISPLAY_VO_DEV);
}

static int display_start_vo(void)
{
    /* Waveshare 7inch HDMI LCD (C) EDID 原生时序，等价于
     * Modeline "1024x600_60.00" 49.00 1024 1072 1168 1312 600 603 613 624 -hsync +vsync。
     * 注意 hbb/vbb 按 SDK sample 约定 = 同步脉冲 + 后廊。 */
    ot_vo_sync_info sync = {
        .syncm = TD_FALSE,
        .iop = TD_TRUE,
        .intfb = 1,
        .vact = DISPLAY_HEIGHT,
        .vbb = 21,
        .vfb = 3,
        .hact = DISPLAY_WIDTH,
        .hbb = 240,
        .hfb = 48,
        .hmid = 1,
        .bvact = 1,
        .bvbb = 1,
        .bvfb = 1,
        .hpw = 96,
        .vpw = 10,
        .idv = TD_FALSE,
        .ihs = TD_TRUE, /* 负 HSync */
        .ivs = TD_FALSE,
    };
    ot_vo_user_sync_info user_sync = {
        .user_sync_attr = {
            .clk_src = OT_VO_CLK_SRC_PLL,
            .vo_pll = {
                .fb_div = 49,
                .frac = 0,
                .ref_div = 1,
                .post_div1 = 6,
                .post_div2 = 4,
            },
        },
        .pre_div = 1,
        .dev_div = 1,
        .clk_reverse_en = TD_FALSE,
    };
    ot_vo_pub_attr pub = {
        .bg_color = 0x000000,
        .intf_type = OT_VO_INTF_HDMI,
        .intf_sync = OT_VO_OUT_USER,
        .sync_info = sync,
    };
    ot_vo_video_layer_attr layer = {
        .display_rect = {0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT},
        .img_size = {DISPLAY_WIDTH, DISPLAY_HEIGHT},
        .display_frame_rate = DISPLAY_FRAME_RATE,
        .pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420,
        .double_frame_en = TD_FALSE,
        .cluster_mode_en = TD_FALSE,
        .dst_dynamic_range = OT_DYNAMIC_RANGE_SDR8,
        .display_buf_len = 3,
        .partition_mode = OT_VO_PARTITION_MODE_SINGLE,
        .compress_mode = OT_COMPRESS_MODE_NONE,
    };
    ot_vo_chn_attr chn = {
        .priority = 0,
        .rect = {0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT},
        .deflicker_en = TD_FALSE,
    };

    CHECK_RET_GOTO(ss_mpi_vo_set_pub_attr(DISPLAY_VO_DEV, &pub), fail);
    CHECK_RET_GOTO(ss_mpi_vo_set_user_sync_info(DISPLAY_VO_DEV, &user_sync), fail);
    CHECK_RET_GOTO(ss_mpi_vo_set_dev_frame_rate(DISPLAY_VO_DEV, DISPLAY_FRAME_RATE), fail);
    CHECK_RET_GOTO(ss_mpi_vo_enable(DISPLAY_VO_DEV), fail);

    CHECK_RET_GOTO(ss_mpi_vo_set_video_layer_attr(DISPLAY_VO_LAYER, &layer), fail_dev);
    CHECK_RET_GOTO(ss_mpi_vo_enable_video_layer(DISPLAY_VO_LAYER), fail_dev);

    CHECK_RET_GOTO(ss_mpi_vo_set_chn_attr(DISPLAY_VO_LAYER, DISPLAY_VO_CHN, &chn), fail_layer);
    CHECK_RET_GOTO(ss_mpi_vo_enable_chn(DISPLAY_VO_LAYER, DISPLAY_VO_CHN), fail_layer);
    return 0;

fail_layer:
    CHECK_RET(ss_mpi_vo_disable_video_layer(DISPLAY_VO_LAYER));
fail_dev:
    CHECK_RET(ss_mpi_vo_disable(DISPLAY_VO_DEV));
fail:
    return -1;
}

/* 配置 HDMI（init/open/CSC/attr），但不启动 PHY 输出（hdmi_start 由 display_init 在
 * 黑场帧稳定后再调）。 */
static int display_config_hdmi(void)
{
    ot_vo_hdmi_param hdmi_param;
    ot_hdmi_attr attr;

    CHECK_RET_GOTO(ss_mpi_hdmi_init(), fail);
    CHECK_RET_GOTO(ss_mpi_hdmi_open(OT_HDMI_ID_0), fail_hdmi_init);

    /* 面板为 RGB 接口的类 DVI 屏，VO 输出走 RGB full 色域。 */
    memset(&hdmi_param, 0, sizeof(hdmi_param));
    CHECK_RET_GOTO(ss_mpi_vo_get_hdmi_param(DISPLAY_VO_DEV, &hdmi_param), fail_hdmi_open);
    hdmi_param.csc.csc_matrix = OT_VO_CSC_MATRIX_BT709FULL_TO_RGBFULL;
    CHECK_RET_GOTO(ss_mpi_vo_set_hdmi_param(DISPLAY_VO_DEV, &hdmi_param), fail_hdmi_open);

    /* DVI 模式（hdmi_en=FALSE）+ 音频禁用 + 用户自定义 VESA 时序。 */
    memset(&attr, 0, sizeof(attr));
    CHECK_RET_GOTO(ss_mpi_hdmi_get_attr(OT_HDMI_ID_0, &attr), fail_hdmi_open);
    attr.hdmi_en = TD_FALSE;
    attr.video_format = OT_HDMI_VIDEO_FORMAT_VESA_CUSTOMER_DEFINE;
    attr.deep_color_mode = OT_HDMI_DEEP_COLOR_24BIT;
    attr.audio_en = TD_FALSE;
    attr.auth_mode_en = TD_FALSE;
    attr.deep_color_adapt_en = TD_TRUE;
    attr.pix_clk = DISPLAY_PIX_CLK_KHZ;
    CHECK_RET_GOTO(ss_mpi_hdmi_set_attr(OT_HDMI_ID_0, &attr), fail_hdmi_open);
    return 0;

fail_hdmi_open:
    CHECK_RET(ss_mpi_hdmi_close(OT_HDMI_ID_0));
fail_hdmi_init:
    CHECK_RET(ss_mpi_hdmi_deinit());
fail:
    return -1;
}

/* 向 VO 通道送一帧全黑 NV21（full-range 黑：Y=0，VU=128）并等其上屏。
 * 目的：PHY 使能前让通道里已有稳定黑场，面板锁定瞬间不见杂线/垃圾内容。
 * 需要公共 VB 池有一块 ≥1024x600 NV21 的空闲块（SYS/VB 由上层先起，见头文件约定）。 */
static int display_show_black(void)
{
    ot_pic_buf_attr buf_attr = {
        .width = DISPLAY_WIDTH,
        .height = DISPLAY_HEIGHT,
        .align = 0,
        .bit_width = OT_DATA_BIT_WIDTH_8,
        .pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420,
        .compress_mode = OT_COMPRESS_MODE_NONE,
    };
    ot_vb_calc_cfg cfg = {0};
    ot_video_frame_info info;
    ot_video_frame *vf = &info.video_frame;
    ot_vb_blk blk;
    td_phys_addr_t phys;
    td_u8 *virt;
    int rc = -1;

    ot_common_get_pic_buf_cfg(&buf_attr, &cfg);
    blk = ss_mpi_vb_get_blk(OT_VB_INVALID_POOL_ID, cfg.vb_size, TD_NULL);
    if (blk == OT_VB_INVALID_HANDLE) {
        LOG_WARN("display_show_black: no VB block (%u bytes) in common pool", cfg.vb_size);
        return -1;
    }
    phys = ss_mpi_vb_handle_to_phys_addr(blk);
    virt = (td_u8 *)ss_mpi_sys_mmap(phys, cfg.vb_size);
    if (phys == 0 || virt == TD_NULL) {
        LOG_ERR("display_show_black: vb handle_to_phys/mmap failed");
        goto release;
    }

    memset(virt + cfg.head_size, 0x00, cfg.main_y_size);                       /* Y */
    memset(virt + cfg.head_size + cfg.main_y_size, 0x80, cfg.main_size - cfg.main_y_size); /* VU */

    memset(&info, 0, sizeof(info));
    info.mod_id = OT_ID_USER;
    info.pool_id = ss_mpi_vb_handle_to_pool_id(blk);
    vf->width = DISPLAY_WIDTH;
    vf->height = DISPLAY_HEIGHT;
    vf->field = OT_VIDEO_FIELD_FRAME;
    vf->pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    vf->video_format = OT_VIDEO_FORMAT_LINEAR;
    vf->compress_mode = OT_COMPRESS_MODE_NONE;
    vf->dynamic_range = OT_DYNAMIC_RANGE_SDR8;
    vf->color_gamut = OT_COLOR_GAMUT_BT601;
    vf->header_stride[0] = cfg.head_stride;
    vf->header_stride[1] = cfg.head_stride;
    vf->header_phys_addr[0] = phys;
    vf->header_phys_addr[1] = vf->header_phys_addr[0] + cfg.head_y_size;
    vf->header_virt_addr[0] = virt;
    vf->header_virt_addr[1] = (td_u8 *)vf->header_virt_addr[0] + cfg.head_y_size;
    vf->stride[0] = cfg.main_stride;
    vf->stride[1] = cfg.main_stride;
    vf->phys_addr[0] = vf->header_phys_addr[0] + cfg.head_size;
    vf->phys_addr[1] = vf->phys_addr[0] + cfg.main_y_size;
    vf->virt_addr[0] = (td_u8 *)vf->header_virt_addr[0] + cfg.head_size;
    vf->virt_addr[1] = (td_u8 *)vf->virt_addr[0] + cfg.main_y_size;

    CHECK_RET_GOTO(ss_mpi_vo_send_frame(DISPLAY_VO_LAYER, DISPLAY_VO_CHN, &info, -1), unmap);
    usleep(2 * 1000000 / DISPLAY_FRAME_RATE); /* 等 2 个 vsync 上屏 */
    rc = 0;

unmap:
    CHECK_RET(ss_mpi_sys_munmap(virt, cfg.vb_size));
release:
    CHECK_RET(ss_mpi_vb_release_blk(blk));
    return rc;
}

int display_init(void)
{
    if (g_display_inited) {
        LOG_WARN("display already inited");
        return 0;
    }

    display_clear_residual();

    if (display_start_vo() != 0) {
        return -1;
    }
    if (display_config_hdmi() != 0) {
        goto fail_vo;
    }

    /* 黑场预帧：先让 VO 通道稳定输出黑场，再使能 PHY，避免面板锁定瞬间
     * 显示无信号样杂线（板端实测现象）。失败仅告警，退化为直接启动。 */
    if (display_show_black() != 0) {
        LOG_WARN("black pre-frame failed, starting HDMI output anyway");
    }

    if (ss_mpi_hdmi_start(OT_HDMI_ID_0) != TD_SUCCESS) {
        LOG_ERR("ss_mpi_hdmi_start failed");
        CHECK_RET(ss_mpi_hdmi_close(OT_HDMI_ID_0));
        CHECK_RET(ss_mpi_hdmi_deinit());
        goto fail_vo;
    }

    g_display_inited = 1;
    LOG_INFO("display up: HDMI0 DVI %ux%u@%u, pix_clk=%d kHz", DISPLAY_WIDTH, DISPLAY_HEIGHT,
             DISPLAY_FRAME_RATE, DISPLAY_PIX_CLK_KHZ);
    return 0;

fail_vo:
    CHECK_RET(ss_mpi_vo_disable_chn(DISPLAY_VO_LAYER, DISPLAY_VO_CHN));
    CHECK_RET(ss_mpi_vo_disable_video_layer(DISPLAY_VO_LAYER));
    CHECK_RET(ss_mpi_vo_disable(DISPLAY_VO_DEV));
    return -1;
}

int display_send_frame(const void *frame_info, int timeout_ms)
{
    td_s32 ret;

    if (!g_display_inited || frame_info == NULL) {
        LOG_ERR("display_send_frame: inited=%d frame=%p", g_display_inited, frame_info);
        return -1;
    }
    ret = ss_mpi_vo_send_frame(DISPLAY_VO_LAYER, DISPLAY_VO_CHN,
                               (const ot_video_frame_info *)frame_info, timeout_ms);
    if (ret != TD_SUCCESS) {
        LOG_ERR("ss_mpi_vo_send_frame failed: %#x", (unsigned)ret);
        return -1;
    }
    return 0;
}

int display_deinit(void)
{
    int rc = 0;

    if (!g_display_inited) {
        return 0;
    }

    /* 创建逆序：HDMI → VO chn → VO layer → VO dev。失败记录但继续释放。 */
    CHECK_RET(ss_mpi_hdmi_stop(OT_HDMI_ID_0));
    CHECK_RET(ss_mpi_hdmi_close(OT_HDMI_ID_0));
    CHECK_RET(ss_mpi_hdmi_deinit());
    CHECK_RET(ss_mpi_vo_disable_chn(DISPLAY_VO_LAYER, DISPLAY_VO_CHN));
    CHECK_RET(ss_mpi_vo_disable_video_layer(DISPLAY_VO_LAYER));
    CHECK_RET(ss_mpi_vo_disable(DISPLAY_VO_DEV));

    g_display_inited = 0;
    LOG_INFO("display down");
    return rc;
}

#else /* !WITH_SS928_SDK */

/* SDK-free 构建桩：保证 ENABLE_SDK=OFF 时库仍可编译链接。 */

int display_init(void)
{
    LOG_ERR("display_init: built without SS928 SDK");
    return -1;
}

int display_send_frame(const void *frame_info, int timeout_ms)
{
    (void)frame_info;
    (void)timeout_ms;
    LOG_ERR("display_send_frame: built without SS928 SDK");
    return -1;
}

int display_deinit(void)
{
    return 0;
}

#endif /* WITH_SS928_SDK */
