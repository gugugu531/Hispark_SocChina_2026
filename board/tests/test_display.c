/* test_display — HDMI 显示驱动板端冒烟测试（触硬件，交叉编译后上板手动运行）。
 *
 * 验证：VO 视频层送帧路径（架构 §6 待验证点 5：VGS/VO 替代 GFBG 是否无 flicker）。
 * 行为：SYS/VB 起 → display_init → 交替送 NV21 彩条/灰阶渐变帧 → 创建逆序释放。
 *
 * 用法：./test_display [秒数]   （默认 10 秒；运行中观察面板画面与 /proc/umap/hdmi0）
 * 前置：板上无其他媒体进程占用 SYS/VB/VO（勿与厂商 sample_hdmi 并跑）。
 */

#include "display.h"
#include "log.h"

#ifndef WITH_SS928_SDK

int main(void)
{
    LOG_WARN("test_display skipped: built without SS928 SDK (host build)");
    return 0;
}

#else /* WITH_SS928_SDK */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ot_buffer.h"
#include "ot_common.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_type.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_vb.h"

typedef struct {
    ot_vb_blk blk;
    td_u8 *virt;
    ot_video_frame_info info;
} test_frame_t;

/* BT.601 limited-range YUV，8 色彩条（白黄青绿品红红蓝黑）。 */
static const td_u8 g_bar_y[8] = {235, 210, 170, 145, 106, 81, 41, 16};
static const td_u8 g_bar_u[8] = {128, 16, 166, 54, 202, 90, 240, 128};
static const td_u8 g_bar_v[8] = {128, 146, 16, 34, 222, 240, 110, 128};

/* NV21（YVU 半平面）填充：pattern 0=垂直彩条，1=水平灰阶渐变。 */
static void fill_nv21(td_u8 *y_plane, td_u8 *c_plane, td_u32 stride, td_u32 w, td_u32 h, int pattern)
{
    td_u32 x, row;

    for (row = 0; row < h; row++) {
        td_u8 *y = y_plane + (size_t)row * stride;
        for (x = 0; x < w; x++) {
            y[x] = (pattern == 0) ? g_bar_y[x * 8 / w] : (td_u8)(16 + x * 219 / (w - 1));
        }
    }
    for (row = 0; row < h / 2; row++) {
        td_u8 *c = c_plane + (size_t)row * stride;
        for (x = 0; x < w / 2; x++) {
            int bar = (int)(x * 2 * 8 / w);
            c[2 * x] = (pattern == 0) ? g_bar_v[bar] : 128;     /* V */
            c[2 * x + 1] = (pattern == 0) ? g_bar_u[bar] : 128; /* U */
        }
    }
}

/* 从公共 VB 池取块并构造 ot_video_frame_info（零拷贝用户帧）。 */
static int test_frame_create(const ot_vb_calc_cfg *cfg, int pattern, test_frame_t *frame)
{
    td_phys_addr_t phys;
    ot_video_frame *vf = &frame->info.video_frame;

    frame->blk = ss_mpi_vb_get_blk(OT_VB_INVALID_POOL_ID, cfg->vb_size, TD_NULL);
    if (frame->blk == OT_VB_INVALID_HANDLE) {
        LOG_ERR("ss_mpi_vb_get_blk(%u) failed", cfg->vb_size);
        return -1;
    }
    phys = ss_mpi_vb_handle_to_phys_addr(frame->blk);
    frame->virt = (td_u8 *)ss_mpi_sys_mmap(phys, cfg->vb_size);
    if (phys == 0 || frame->virt == TD_NULL) {
        LOG_ERR("vb handle_to_phys/mmap failed");
        CHECK_RET(ss_mpi_vb_release_blk(frame->blk));
        return -1;
    }

    memset(&frame->info, 0, sizeof(frame->info));
    frame->info.mod_id = OT_ID_USER;
    frame->info.pool_id = ss_mpi_vb_handle_to_pool_id(frame->blk);
    vf->width = DISPLAY_WIDTH;
    vf->height = DISPLAY_HEIGHT;
    vf->field = OT_VIDEO_FIELD_FRAME;
    vf->pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    vf->video_format = OT_VIDEO_FORMAT_LINEAR;
    vf->compress_mode = OT_COMPRESS_MODE_NONE;
    vf->dynamic_range = OT_DYNAMIC_RANGE_SDR8;
    vf->color_gamut = OT_COLOR_GAMUT_BT601;
    vf->header_stride[0] = cfg->head_stride;
    vf->header_stride[1] = cfg->head_stride;
    vf->header_phys_addr[0] = phys;
    vf->header_phys_addr[1] = vf->header_phys_addr[0] + cfg->head_y_size;
    vf->header_virt_addr[0] = frame->virt;
    vf->header_virt_addr[1] = (td_u8 *)vf->header_virt_addr[0] + cfg->head_y_size;
    vf->stride[0] = cfg->main_stride;
    vf->stride[1] = cfg->main_stride;
    vf->phys_addr[0] = vf->header_phys_addr[0] + cfg->head_size;
    vf->phys_addr[1] = vf->phys_addr[0] + cfg->main_y_size;
    vf->virt_addr[0] = (td_u8 *)vf->header_virt_addr[0] + cfg->head_size;
    vf->virt_addr[1] = (td_u8 *)vf->virt_addr[0] + cfg->main_y_size;

    fill_nv21((td_u8 *)vf->virt_addr[0], (td_u8 *)vf->virt_addr[1], cfg->main_stride, DISPLAY_WIDTH,
              DISPLAY_HEIGHT, pattern);
    return 0;
}

static void test_frame_destroy(const ot_vb_calc_cfg *cfg, test_frame_t *frame)
{
    if (frame->virt != TD_NULL) {
        CHECK_RET(ss_mpi_sys_munmap(frame->virt, cfg->vb_size));
        frame->virt = TD_NULL;
    }
    if (frame->blk != OT_VB_INVALID_HANDLE) {
        CHECK_RET(ss_mpi_vb_release_blk(frame->blk));
        frame->blk = OT_VB_INVALID_HANDLE;
    }
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
    int run_sec = (argc > 1) ? atoi(argv[1]) : 10;
    int rc = 1;
    int sent = 0;
    long t0, t_send;
    ot_pic_buf_attr buf_attr = {
        .width = DISPLAY_WIDTH,
        .height = DISPLAY_HEIGHT,
        .align = 0,
        .bit_width = OT_DATA_BIT_WIDTH_8,
        .pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420,
        .compress_mode = OT_COMPRESS_MODE_NONE,
    };
    ot_vb_calc_cfg calc_cfg = {0};
    ot_vb_cfg vb_cfg = {0};
    test_frame_t frames[2] = {{0}, {0}};

    ot_common_get_pic_buf_cfg(&buf_attr, &calc_cfg);
    LOG_INFO("test_display: %ds, NV21 %ux%u vb_size=%u stride=%u", run_sec, DISPLAY_WIDTH,
             DISPLAY_HEIGHT, calc_cfg.vb_size, calc_cfg.main_stride);

    /* SYS/VB 起（display 模块不管 SYS/VB）。先清残留状态再初始化。 */
    CHECK_RET(ss_mpi_sys_exit());
    CHECK_RET(ss_mpi_vb_exit());
    vb_cfg.max_pool_cnt = 1;
    vb_cfg.common_pool[0].blk_size = calc_cfg.vb_size;
    vb_cfg.common_pool[0].blk_cnt = 4;
    CHECK_RET_GOTO(ss_mpi_vb_set_cfg(&vb_cfg), out);
    CHECK_RET_GOTO(ss_mpi_vb_init(), out);
    CHECK_RET_GOTO(ss_mpi_sys_init(), out_vb);

    if (display_init() != 0) {
        goto out_sys;
    }
    if (test_frame_create(&calc_cfg, 0, &frames[0]) != 0 ||
        test_frame_create(&calc_cfg, 1, &frames[1]) != 0) {
        goto out_frames;
    }

    /* 每秒交替彩条/渐变；记录单次送帧耗时。 */
    t0 = now_ms();
    while (now_ms() - t0 < run_sec * 1000L) {
        t_send = now_ms();
        if (display_send_frame(&frames[(sent / 30) % 2].info, -1) != 0) {
            goto out_frames;
        }
        sent++;
        if (sent % 30 == 0) {
            LOG_INFO("sent %d frames, last send took %ld ms", sent, now_ms() - t_send);
        }
        usleep(33000); /* ~30fps 送帧节奏 */
    }
    rc = 0;
    LOG_INFO("test_display PASS: %d frames in %ds", sent, run_sec);

out_frames:
    test_frame_destroy(&calc_cfg, &frames[1]);
    test_frame_destroy(&calc_cfg, &frames[0]);
    CHECK_RET(display_deinit());
out_sys:
    CHECK_RET(ss_mpi_sys_exit());
out_vb:
    CHECK_RET(ss_mpi_vb_exit());
out:
    return rc;
}

#endif /* WITH_SS928_SDK */
