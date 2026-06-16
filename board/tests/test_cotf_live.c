/* test_cotf_live — CoTF 路线端侧实时演示（触硬件，交叉编译后上板运行）。
 *
 * 链路：OS08A20 → VI(online) → ISP(+CLUT 3D-LUT 全分辨率硬件施加) → VPSS chn0 1024x600 NV21
 *       → display 零拷贝送 HDMI。CLUT 由 cotf_clut.bin（5508 节点 u32，
 *       models/tools/cotf_make_demo_lut.py 经 cotf_lut_pack 打包）加载。
 *
 * 交互：监听 Waveshare 触摸屏 /dev/input/event0，每次手指点击翻转 CLUT：
 *   OFF = 原始相机图（ISP 直出）；ON = CoTF 全分辨率校正（ISP 硬件三线性施加，零 NPU）。
 * CLUT 施加在 ISP 内联、不进 NPU，显示吞吐仍是纯硬件链 30fps。
 *
 * 用法：./test_cotf_live [秒数] [--lut <bin>] [--sensor 0|1] [--touch <dev>]
 *                        [--auto <秒>] [--dumpdir <目录>]
 *   默认 600s、lut=/root/socchina-2026/cotf_clut.bin、sensor=默认、touch=/dev/input/event0
 *   --auto <秒>    每隔 N 秒自动翻转一次 CLUT（无人点屏也能演示/取证）
 *   --dumpdir <目录>  每次翻转后把下一帧 NV21 落盘到 <目录>/clut_{on,off}_<seq>.nv21
 * 前置：板上接 OS08A20，HDMI 面板已接，无其他媒体进程争用 SYS/VB/VI/VPSS/VO。
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
    LOG_WARN("test_cotf_live skipped: built without SS928 SDK (host build)");
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

#include <math.h>

#include "ot_buffer.h"
#include "ot_common_isp.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_isp_define.h"
#include "ot_type.h"
#include "sample_comm.h"
#include "ss_mpi_isp.h"
#include "ss_mpi_sys.h"

#define CLUT_UNITY_GAIN 1024u /* gain 12bit, 1024≈1x（见 isp.h） */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 读 5508 个小端 u32 LUT 系数。 */
static int load_clut_bin(const char *path, unsigned int *lut, unsigned n)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        LOG_ERR("clut bin: cannot open %s (%s)", path, strerror(errno));
        return -1;
    }
    size_t got = fread(lut, sizeof(unsigned int), n, fp);
    fclose(fp);
    if (got != n) {
        LOG_ERR("clut bin: read %zu/%u nodes", got, n);
        return -1;
    }
    return 0;
}

/* 读回硬件当前 CLUT 系数（SDK 默认表，作为正确几何的基线）。 */
static int clut_read_coeff(unsigned int *lut, unsigned n)
{
    static ot_isp_clut_lut cl;
    if (ss_mpi_isp_get_clut_coeff(0, &cl) != TD_SUCCESS) {
        LOG_ERR("get_clut_coeff failed");
        return -1;
    }
    memcpy(lut, cl.lut, n * sizeof(unsigned int));
    return 0;
}

/* 打印 CLUT 默认表诊断：是否像恒等（节点输出≈坐标渐变）。 */
static void clut_probe(const unsigned int *lut, unsigned n)
{
    unsigned i, nz = 0, eq = 0;
    unsigned rmin = 1023, rmax = 0;
    for (i = 0; i < n; i++) {
        unsigned r = (lut[i] >> 20) & 0x3FF, g = (lut[i] >> 10) & 0x3FF, b = lut[i] & 0x3FF;
        if (lut[i]) nz++;
        if (r == g && g == b) eq++;
        if (r < rmin) rmin = r;
        if (r > rmax) rmax = r;
    }
    LOG_INFO("[probe] nodes=%u nonzero=%u (R==G==B)=%u Rrange=[%u,%u]", n, nz, eq, rmin, rmax);
    for (i = 0; i < n; i += n / 8) {
        LOG_INFO("[probe] node[%u]=%#010x -> R=%u G=%u B=%u", i, lut[i],
                 (lut[i] >> 20) & 0x3FF, (lut[i] >> 10) & 0x3FF, lut[i] & 0x3FF);
    }
}

/* 对每个节点的输出值就地施加 gamma 提亮：out=1023*(out/1023)^gamma（gamma<1 提亮暗部）。
 * 这是逐节点的**输出值**点变换——与节点↔坐标映射无关：作用在硬件默认表(正确几何)之上，
 * 结果= gamma(默认表(input))，即在默认色调曲线上再叠一层提亮。绕开 mesh 标定。
 * 关键：相机 AE 在 CLUT **之前**测光（实测 luma 不随 CLUT 变），故该提亮不被 AE 抵消，toggle 一眼可辨。 */
static void clut_apply_tone(unsigned int *lut, unsigned n, double gamma)
{
    unsigned i, c;
    for (i = 0; i < n; i++) {
        unsigned ch[3] = {(lut[i] >> 20) & 0x3FF, (lut[i] >> 10) & 0x3FF, lut[i] & 0x3FF};
        unsigned out = 0;
        for (c = 0; c < 3; c++) {
            double y = pow((double)ch[c] / 1023.0, gamma) * 1023.0 + 0.5;
            unsigned q = (y > 1023.0) ? 1023u : (unsigned)y;
            out |= q << (20 - 10 * c);
        }
        lut[i] = out;
    }
}

/* 翻转 CLUT 开关。on=1 启用 CoTF 校正（unity gain），0 关闭（原图）。 */
static int clut_apply(int on)
{
    int rc = isp_set_clut(on ? ISP_BLOCK_AUTO : ISP_BLOCK_OFF,
                          CLUT_UNITY_GAIN, CLUT_UNITY_GAIN, CLUT_UNITY_GAIN);
    LOG_INFO(">>> CLUT %s  (%s)", on ? "ON" : "OFF",
             on ? "CoTF 校正：ISP 硬件全分辨率三线性施加" : "原始相机图：ISP 直出");
    return rc;
}

/* 把一帧 NV21（Y + 交织 VU）按行落盘，去除 stride 填充（与 test_capture 一致）。 */
static int dump_nv21_frame(const ot_video_frame_info *frame, const char *path)
{
    const ot_video_frame *vf = &frame->video_frame;
    td_u32 row;
    int rc = -1;
    FILE *fp = NULL;
    td_u32 map_size = vf->stride[0] * vf->height * 3 / 2;
    td_u8 *virt = (td_u8 *)ss_mpi_sys_mmap(vf->phys_addr[0], map_size);

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
    rc = 0;
    fclose(fp);
    CHECK_RET(ss_mpi_sys_munmap(virt, map_size));
    return rc;
}

/* 排空触摸事件队列；若期间有手指按下返回 1（需翻转）。 */
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

int main(int argc, char **argv)
{
    int run_sec = 600;
    capture_cfg_t cap_cfg = {CAPTURE_SENSOR_INDEX_DEFAULT, CAPTURE_MODE_LINEAR};
    const char *lut_path = "/root/socchina-2026/cotf_clut.bin";
    const char *touch_dev = "/dev/input/event0";
    const char *dump_dir = NULL;
    double tone_gamma = 0.0; /* >0 ⇒ 几何无关 tone 模式（读默认表就地重映射） */
    int do_probe = 0;
    unsigned lock_exp_us = 0, lock_again = 0; /* >0 ⇒ 锁定手动曝光，排除 AE 干扰 */
    int auto_sec = 0;
    int dump_seq = 0, dump_pending = 0;
    long t_last_auto = 0;
    int rc = 1;
    int sys_up = 0, cap_up = 0, vpss_up = 0, bound = 0, disp_up = 0;
    int tfd = -1;
    unsigned in_w = 0, in_h = 0;
    int frames = 0, clut_on = 0, toggles = 0;
    long t0, t_last_log;
    static unsigned int clut_lut[OT_ISP_CLUT_LUT_LENGTH];
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
        if (strcmp(argv[i], "--lut") == 0 && i + 1 < argc) {
            lut_path = argv[++i];
        } else if (strcmp(argv[i], "--sensor") == 0 && i + 1 < argc) {
            cap_cfg.sensor_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--touch") == 0 && i + 1 < argc) {
            touch_dev = argv[++i];
        } else if (strcmp(argv[i], "--auto") == 0 && i + 1 < argc) {
            auto_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dumpdir") == 0 && i + 1 < argc) {
            dump_dir = argv[++i];
        } else if (strcmp(argv[i], "--tone") == 0 && i + 1 < argc) {
            tone_gamma = atof(argv[++i]);
        } else if (strcmp(argv[i], "--probe") == 0) {
            do_probe = 1;
        } else if (strcmp(argv[i], "--lockexp") == 0 && i + 2 < argc) {
            lock_exp_us = (unsigned)atoi(argv[++i]);
            lock_again = (unsigned)atoi(argv[++i]);
        } else {
            run_sec = atoi(argv[i]);
        }
    }

    /* tone/probe 模式从硬件默认表派生（几何无关），不需要 .bin。 */
    if (tone_gamma <= 0.0 && !do_probe) {
        if (load_clut_bin(lut_path, clut_lut, OT_ISP_CLUT_LUT_LENGTH) != 0) {
            return 1;
        }
    }
    if (capture_query_in_size(&in_w, &in_h) != 0) {
        return 1;
    }
    LOG_INFO("test_cotf_live: %ds sensor=%d in=%ux%u lut=%s touch=%s", run_sec,
             cap_cfg.sensor_index, in_w, in_h, lut_path, touch_dev);

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

    /* 可选：锁定手动曝光，排除 AE 反馈把 LUT 提亮效果抵消（静态取证用）。 */
    if (lock_exp_us != 0 || lock_again != 0) {
        (void)isp_set_exposure_manual(lock_exp_us, lock_again);
        LOG_INFO("[lockexp] manual exposure %uus again=%u/1024 (AE off)", lock_exp_us, lock_again);
    }

    /* tone/probe：读回硬件默认 CLUT 表作为正确几何基线。 */
    if (tone_gamma > 0.0 || do_probe) {
        if (clut_read_coeff(clut_lut, OT_ISP_CLUT_LUT_LENGTH) != 0) {
            goto cleanup;
        }
        clut_probe(clut_lut, OT_ISP_CLUT_LUT_LENGTH);
        if (tone_gamma > 0.0) {
            clut_apply_tone(clut_lut, OT_ISP_CLUT_LUT_LENGTH, tone_gamma);
            LOG_INFO("[tone] applied gamma=%.2f in-place on default table (geometry-free)", tone_gamma);
        }
    }

    /* 把 CoTF LUT 灌进运行中的 ISP CLUT；初始关闭（先看原图）。 */
    if (isp_load_clut_lut(clut_lut, OT_ISP_CLUT_LUT_LENGTH) != 0) {
        goto cleanup;
    }
    (void)clut_apply(0);

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
        LOG_WARN("touch: open %s failed (%s) — 仍出图，但无法点屏切换", touch_dev, strerror(errno));
    } else {
        LOG_INFO("==> 触摸屏幕切换 [OFF 原图 <-> ON CoTF 校正]");
    }

    t0 = now_ms();
    t_last_log = t0;
    t_last_auto = t0;
    while (now_ms() - t0 < run_sec * 1000L) {
        int toggle_now = (tfd >= 0 && touch_pressed(tfd));
        if (auto_sec > 0 && now_ms() - t_last_auto >= auto_sec * 1000L) {
            toggle_now = 1;
            t_last_auto = now_ms();
        }
        if (toggle_now) {
            clut_on = !clut_on;
            toggles++;
            (void)clut_apply(clut_on);
            if (dump_dir != NULL) {
                dump_pending = 6; /* 等 ~6 帧让 CLUT 生效再落盘取证 */
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
            snprintf(path, sizeof(path), "%s/clut_%s_%d.nv21", dump_dir,
                     clut_on ? "on" : "off", dump_seq++);
            (void)dump_nv21_frame(&frame, path);
        }
        (void)vpss_release_frame(0, 0, &frame);
        frames++;
        if (now_ms() - t_last_log >= 2000) {
            luma_stats_t luma = {0};
            if (isp_get_luma_stats(&luma) == 0) {
                LOG_INFO("frames=%d fps=%.1f clut=%s toggles=%d | luma mean=%.1f low=%.1f%% high=%.1f%%",
                         frames, frames * 1000.0 / (now_ms() - t0), clut_on ? "ON" : "OFF",
                         toggles, luma.mean_luma, luma.clip_low_pct, luma.clip_high_pct);
            }
            t_last_log = now_ms();
        }
    }
    rc = 0;
    LOG_INFO("test_cotf_live PASS: %d frames in %ds (%.1f fps), %d toggles", frames, run_sec,
             frames * 1000.0 / (now_ms() - t0), toggles);

cleanup:
    if (tfd >= 0) {
        close(tfd);
    }
    (void)isp_set_clut(ISP_BLOCK_OFF, CLUT_UNITY_GAIN, CLUT_UNITY_GAIN, CLUT_UNITY_GAIN);
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
