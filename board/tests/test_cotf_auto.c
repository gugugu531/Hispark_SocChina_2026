/* test_cotf_auto — CoTF 场景自适应曝光校正（端侧实时，触硬件）。
 *
 * 闭环：相机 → ISP → VPSS → HDMI（30fps），低频控制环读 AE 直方图统计 → control_decide
 * 判决曝光模式（提亮/压高光/双向/旁路）→ 经 ISP 色调块施加（零 NPU、全分辨率硬件内联）。
 * 仅在场景变化时按 control_should_refresh_lut 的迟滞/限流策略刷新，把"出参数"移出每帧关键路径
 * ——即 architecture.md §2「场景自适应控制大脑」的真实接线。
 *
 * 施加块（--block，默认 gamma）：
 *   gamma：原生 **Gamma** 1D 色调表（《ISP 图像调优指南》§4.11，1025 节点）——曝光/色调用例的首选，
 *          与 CLUT 3D mesh 几何无关、最干净；
 *   clut： CLUT 3D-LUT 的几何无关 tone（读默认表→输出值叠曲线），用于对照。
 *
 * 这是 CoTF 路线的**模型无关版整链**：把"NN 出 LUT"暂以"AE 统计→规则判决"替代,验证整链实时校正。
 * 交互：触摸屏点击 toggle 自动校正 开/关（关 = 原始相机图，便于 A/B 对比）。
 * 用法：./test_cotf_auto [秒数] [--block gamma|clut] [--sensor 0|1] [--touch <dev>]
 *                        [--strength <0..1>] [--poll <帧>] [--dumpdir <目录>]
 */

#include "capture.h"
#include "control.h"
#include "display.h"
#include "isp.h"
#include "log.h"
#include "vpss.h"

#ifndef WITH_SS928_SDK

int main(void)
{
    LOG_WARN("test_cotf_auto skipped: built without SS928 SDK (host build)");
    return 0;
}

#else /* WITH_SS928_SDK */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <linux/input.h>

#include "ot_buffer.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_isp_define.h"
#include "ot_type.h"
#include "sample_comm.h"
#include "ss_mpi_sys.h"

#define CTRL_WARMUP_FRAMES 30 /* AE 收敛前不读统计/不刷 LUT（约 1s @30fps） */

typedef enum { BLOCK_GAMMA = 0, BLOCK_CLUT } tone_block_t;

/* 按所选 ISP 块施加 tone（两块都自带 BYPASS 处理）。 */
static int apply_tone(tone_block_t block, const unsigned int *base_lut, isp_tone_t tone, float s)
{
    return (block == BLOCK_GAMMA) ? isp_gamma_apply_tone(tone, s)
                                  : isp_clut_apply_tone(base_lut, OT_ISP_CLUT_LUT_LENGTH, tone, s);
}

/* 把一帧 NV21 按行落盘（去 stride 填充），用于 before/after 取证。 */
static int dump_nv21_frame(const ot_video_frame_info *frame, const char *path)
{
    const ot_video_frame *vf = &frame->video_frame;
    td_u32 row, map_size = vf->stride[0] * vf->height * 3 / 2;
    td_u8 *virt = (td_u8 *)ss_mpi_sys_mmap(vf->phys_addr[0], map_size);
    FILE *fp;
    if (virt == TD_NULL) {
        return -1;
    }
    fp = fopen(path, "wb");
    if (fp == NULL) {
        CHECK_RET(ss_mpi_sys_munmap(virt, map_size));
        return -1;
    }
    for (row = 0; row < vf->height; row++) {
        fwrite(virt + (size_t)row * vf->stride[0], 1, vf->width, fp);
    }
    for (row = 0; row < vf->height / 2; row++) {
        fwrite(virt + (size_t)vf->stride[0] * vf->height + (size_t)row * vf->stride[1], 1,
               vf->width, fp);
    }
    LOG_INFO("dump: %ux%u NV21 -> %s", vf->width, vf->height, path);
    fclose(fp);
    CHECK_RET(ss_mpi_sys_munmap(virt, map_size));
    return 0;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int touch_pressed(int tfd)
{
    struct input_event ev;
    int pressed = 0;
    ssize_t r;
    while ((r = read(tfd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 1) {
            pressed = 1;
        }
    }
    (void)r;
    return pressed;
}

static isp_tone_t mode_to_tone(expo_mode_t m)
{
    switch (m) {
        case EXPO_MODE_BRIGHTEN: return ISP_TONE_BRIGHTEN;
        case EXPO_MODE_COMPRESS: return ISP_TONE_COMPRESS;
        case EXPO_MODE_BIDIR:    return ISP_TONE_BIDIR;
        case EXPO_MODE_BYPASS:
        default:                 return ISP_TONE_BYPASS;
    }
}

static const char *mode_name(expo_mode_t m)
{
    switch (m) {
        case EXPO_MODE_BRIGHTEN: return "BRIGHTEN(提亮暗部)";
        case EXPO_MODE_COMPRESS: return "COMPRESS(压高光)";
        case EXPO_MODE_BIDIR:    return "BIDIR(双向压缩)";
        default:                 return "BYPASS(正常)";
    }
}

/* 干净 ASCII 标签（用于落盘文件名）。 */
static const char *mode_tag(expo_mode_t m)
{
    switch (m) {
        case EXPO_MODE_BRIGHTEN: return "brighten";
        case EXPO_MODE_COMPRESS: return "compress";
        case EXPO_MODE_BIDIR:    return "bidir";
        default:                 return "bypass";
    }
}

int main(int argc, char **argv)
{
    int run_sec = 600;
    capture_cfg_t cap_cfg = {CAPTURE_SENSOR_INDEX_DEFAULT, CAPTURE_MODE_LINEAR};
    const char *touch_dev = "/dev/input/event0";
    const char *dump_dir = NULL;
    tone_block_t block = BLOCK_GAMMA; /* 默认走原生 Gamma（推荐路径） */
    float strength = 0.7f;
    unsigned poll = 5; /* 每 5 帧读一次 AE 统计评估刷新 */
    int dump_seq = 0, dump_pending = 0;
    int rc = 1;
    int sys_up = 0, cap_up = 0, vpss_up = 0, bound = 0, disp_up = 0;
    int tfd = -1;
    unsigned in_w = 0, in_h = 0;
    int frames = 0, auto_on = 1;
    long t0, t_last_log;
    static unsigned int base_lut[OT_ISP_CLUT_LUT_LENGTH]; /* 硬件默认表基线（缓存一次） */
    luma_stats_t prev_stats, cur_stats;
    int have_prev = 0;
    unsigned last_refresh_frame = 0;
    expo_mode_t cur_mode = EXPO_MODE_BYPASS;
    ot_vb_cfg vb_cfg = {0};
    ot_pic_buf_attr buf_attr = {0};
    ot_vb_calc_cfg calc_cfg = {0};
    ot_video_frame_info frame;
    vpss_chn_cfg_t chn_cfg[VPSS_CHN_COUNT] = {
        {1, DISPLAY_WIDTH, DISPLAY_HEIGHT, 2},
        {0, 0, 0, 0},
        {0, 0, 0, 0},
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sensor") == 0 && i + 1 < argc) {
            cap_cfg.sensor_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--touch") == 0 && i + 1 < argc) {
            touch_dev = argv[++i];
        } else if (strcmp(argv[i], "--strength") == 0 && i + 1 < argc) {
            strength = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--poll") == 0 && i + 1 < argc) {
            poll = (unsigned)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dumpdir") == 0 && i + 1 < argc) {
            dump_dir = argv[++i];
        } else if (strcmp(argv[i], "--block") == 0 && i + 1 < argc) {
            block = (strcmp(argv[++i], "clut") == 0) ? BLOCK_CLUT : BLOCK_GAMMA;
        } else {
            run_sec = atoi(argv[i]);
        }
    }

    if (capture_query_in_size(&in_w, &in_h) != 0) {
        return 1;
    }
    LOG_INFO("test_cotf_auto: %ds block=%s sensor=%d in=%ux%u strength=%.2f poll=%u touch=%s dump=%s",
             run_sec, (block == BLOCK_GAMMA) ? "gamma" : "clut", cap_cfg.sensor_index, in_w, in_h,
             strength, poll, touch_dev, dump_dir ? dump_dir : "off");

    buf_attr.width = in_w;
    buf_attr.height = in_h;
    buf_attr.align = OT_DEFAULT_ALIGN;
    buf_attr.bit_width = OT_DATA_BIT_WIDTH_8;
    buf_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    buf_attr.compress_mode = OT_COMPRESS_MODE_SEG;
    ot_common_get_pic_buf_cfg(&buf_attr, &calc_cfg);
    vb_cfg.max_pool_cnt = 2;
    vb_cfg.common_pool[0].blk_size = calc_cfg.vb_size;
    vb_cfg.common_pool[0].blk_cnt = 10;
    CHECK_RET_GOTO(sample_comm_sys_init_with_vb_supplement(&vb_cfg, OT_VB_SUPPLEMENT_BNR_MOT_MASK),
                   cleanup);
    sys_up = 1;

    if (capture_init(&cap_cfg) != 0) {
        goto cleanup;
    }
    cap_up = 1;

    /* CLUT 块需要默认表基线（Gamma 块内部自缓存默认 Gamma，不需要）。 */
    if (block == BLOCK_CLUT && isp_clut_get_coeff(base_lut, OT_ISP_CLUT_LUT_LENGTH) != 0) {
        goto cleanup;
    }
    (void)apply_tone(block, base_lut, ISP_TONE_BYPASS, strength); /* 初始 = 原图（bypass） */

    if (vpss_init(0, in_w, in_h, chn_cfg) != 0) {
        goto cleanup;
    }
    vpss_up = 1;
    if (capture_bind_vpss(0) != 0) {
        goto cleanup;
    }
    bound = 1;
    if (display_init() != 0) {
        goto cleanup;
    }
    disp_up = 1;

    tfd = open(touch_dev, O_RDONLY | O_NONBLOCK);
    if (tfd < 0) {
        LOG_WARN("touch: open %s failed (%s) — 仍出图，无法点屏切换", touch_dev, strerror(errno));
    } else {
        LOG_INFO("==> 触摸屏幕切换 自动曝光校正 [ON 场景自适应 <-> OFF 原图]");
    }

    if (dump_dir != NULL) {
        dump_pending = 6; /* 先落一帧 bypass 基线（off）作 before */
    }
    t0 = now_ms();
    t_last_log = t0;
    while (now_ms() - t0 < run_sec * 1000L) {
        if (tfd >= 0 && touch_pressed(tfd)) {
            auto_on = !auto_on;
            if (!auto_on) {
                cur_mode = EXPO_MODE_BYPASS;
                (void)apply_tone(block, base_lut, ISP_TONE_BYPASS, strength); /* 关 = 原图 */
                LOG_INFO(">>> AUTO OFF (原始相机图)");
            } else {
                have_prev = 0; /* 重新立即刷新 */
                LOG_INFO(">>> AUTO ON (场景自适应曝光校正)");
            }
            if (dump_dir != NULL) {
                dump_pending = 6; /* 等 ~6 帧让生效再落盘取证 */
            }
        }

        if (vpss_get_frame(0, 0, &frame, 1000) != 0) {
            goto cleanup;
        }
        if (display_send_frame(&frame, -1) != 0) {
            (void)vpss_release_frame(0, 0, &frame);
            goto cleanup;
        }
        if (dump_pending > 0 && --dump_pending == 0) {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s_%s_%d.nv21", dump_dir,
                     (block == BLOCK_GAMMA) ? "gamma" : "clut",
                     auto_on ? mode_tag(cur_mode) : "off", dump_seq++);
            (void)dump_nv21_frame(&frame, path);
        }
        (void)vpss_release_frame(0, 0, &frame);
        frames++;

        /* 低频控制环（AE 收敛后才读统计）：读 AE 统计 → 判决 → 按迟滞/限流刷新 CLUT tone。 */
        if (auto_on && frames > CTRL_WARMUP_FRAMES && (unsigned)frames % poll == 0) {
            if (isp_get_luma_stats(&cur_stats) == 0) {
                if (control_should_refresh_lut(have_prev ? &prev_stats : NULL, &cur_stats,
                                               (unsigned)frames - last_refresh_frame)) {
                    cur_mode = control_decide(&cur_stats);
                    (void)apply_tone(block, base_lut, mode_to_tone(cur_mode), strength);
                    prev_stats = cur_stats;
                    have_prev = 1;
                    last_refresh_frame = (unsigned)frames;
                    if (dump_dir != NULL) {
                        dump_pending = 6;
                    }
                    LOG_INFO("[ctrl] refresh -> %s (luma=%.1f low=%.1f%% high=%.1f%%)",
                             mode_name(cur_mode), cur_stats.mean_luma, cur_stats.clip_low_pct,
                             cur_stats.clip_high_pct);
                }
            }
        }
        if (now_ms() - t_last_log >= 2000) {
            LOG_INFO("frames=%d fps=%.1f auto=%s mode=%s", frames,
                     frames * 1000.0 / (now_ms() - t0), auto_on ? "ON" : "OFF",
                     mode_name(cur_mode));
            t_last_log = now_ms();
        }
    }
    rc = 0;
    LOG_INFO("test_cotf_auto PASS: %d frames in %ds (%.1f fps)", frames, run_sec,
             frames * 1000.0 / (now_ms() - t0));

cleanup:
    if (tfd >= 0) {
        close(tfd);
    }
    if (cap_up) {
        (void)apply_tone(block, base_lut, ISP_TONE_BYPASS, strength); /* 退出前还原 */
    }
    if (disp_up) {
        CHECK_RET(display_deinit());
    }
    if (bound) {
        CHECK_RET(capture_unbind_vpss(0));
    }
    if (vpss_up) {
        CHECK_RET(vpss_deinit(0));
    }
    if (cap_up) {
        CHECK_RET(capture_deinit());
    }
    if (sys_up) {
        sample_comm_sys_exit();
    }
    return rc;
}

#endif /* WITH_SS928_SDK */
