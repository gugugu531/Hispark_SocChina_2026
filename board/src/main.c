#include "control.h"
#include "log.h"
#include "version.h"

/*
 * 板端应用入口（生产主程序）。
 *
 * 架构（architecture.md §2）：把模型/控制移出每帧关键路径，分两条线程：
 *   - 显示线程（主线程，~30fps）：相机 → VI → ISP → VPSS chn0 → display(VO/HDMI) 全分辨率直通。
 *   - 控制线程（低频）：读 ISP AE 统计 → control_decide 判决曝光模式 → 按迟滞/限流刷新 ISP
 *     色调块（Gamma，CoTF 曝光施加的首选原生块，零 NPU、与每帧解耦）。
 *
 * 这是 CoTF 路线"低分辨率出参数 + 硬件全分辨率施加"的产品形态：当前控制大脑是规则判决
 * （control_decide），后续可在控制线程内换成 infer.c 的 NN 输出（缩略图→NPU→色调曲线）。
 * SDK-free 构建（无硬件）退化为骨架自检。
 */

#ifdef WITH_SS928_SDK

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "capture.h"
#include "display.h"
#include "isp.h"
#include "vpss.h"

#include "ot_buffer.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_type.h"
#include "sample_comm.h"

#define APP_CTRL_PERIOD_MS  100u /* 控制环周期（~10Hz，低频；刷新由迟滞/限流再收敛） */
#define APP_CTRL_WARMUP_MS  1000u /* AE 收敛前不读统计 */
#define APP_TONE_STRENGTH   0.7f

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static isp_tone_t mode_to_tone(expo_mode_t m)
{
    switch (m) {
        case EXPO_MODE_BRIGHTEN: return ISP_TONE_BRIGHTEN;
        case EXPO_MODE_COMPRESS: return ISP_TONE_COMPRESS;
        case EXPO_MODE_BIDIR:    return ISP_TONE_BIDIR;
        default:                 return ISP_TONE_BYPASS;
    }
}

static const char *mode_name(expo_mode_t m)
{
    switch (m) {
        case EXPO_MODE_BRIGHTEN: return "BRIGHTEN";
        case EXPO_MODE_COMPRESS: return "COMPRESS";
        case EXPO_MODE_BIDIR:    return "BIDIR";
        default:                 return "BYPASS";
    }
}

/*
 * 控制线程：低频读 AE 统计 → 判决 → 刷新 ISP Gamma 色调。
 * 只触 ISP 参数面（统计读 + Gamma 写），不借用 VPSS 帧，与显示线程无共享帧、无锁需求。
 */
static void *control_worker(void *arg)
{
    float strength = *(const float *)arg;
    luma_stats_t prev, cur;
    int have_prev = 0;
    unsigned ticks = 0, last_refresh = 0;

    (void)isp_gamma_apply_tone(ISP_TONE_BYPASS, strength); /* 缓存默认 Gamma + 初始原图 */
    usleep(APP_CTRL_WARMUP_MS * 1000);                     /* 等 AE 收敛 */

    while (!g_stop) {
        usleep(APP_CTRL_PERIOD_MS * 1000);
        ticks++;
        if (isp_get_luma_stats(&cur) != 0) {
            continue;
        }
        if (control_should_refresh_lut(have_prev ? &prev : TD_NULL, &cur, ticks - last_refresh)) {
            expo_mode_t mode = control_decide(&cur);
            (void)isp_gamma_apply_tone(mode_to_tone(mode), strength);
            prev = cur;
            have_prev = 1;
            last_refresh = ticks;
            LOG_INFO("[ctrl] -> %s (luma=%.1f low=%.1f%% high=%.1f%%)", mode_name(mode),
                     cur.mean_luma, cur.clip_low_pct, cur.clip_high_pct);
        }
    }
    (void)isp_gamma_apply_tone(ISP_TONE_BYPASS, strength); /* 退出前还原默认 Gamma */
    return TD_NULL;
}

int main(int argc, char **argv)
{
    capture_cfg_t cap_cfg = {CAPTURE_SENSOR_INDEX_DEFAULT, CAPTURE_MODE_LINEAR};
    float strength = APP_TONE_STRENGTH;
    int rc = 1;
    int sys_up = 0, cap_up = 0, vpss_up = 0, bound = 0, disp_up = 0, ctrl_up = 0;
    unsigned in_w = 0, in_h = 0;
    long t0, t_last_log;
    uint64_t frames = 0;
    pthread_t ctrl_tid;
    ot_vb_cfg vb_cfg = {0};
    ot_pic_buf_attr buf_attr = {0};
    ot_vb_calc_cfg calc_cfg = {0};
    ot_video_frame_info frame;
    vpss_chn_cfg_t chn_cfg[VPSS_CHN_COUNT] = {
        {1, DISPLAY_WIDTH, DISPLAY_HEIGHT, 2}, /* chn0：显示底图 */
        {0, 0, 0, 0},
        {0, 0, 0, 0},
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sensor") == 0 && i + 1 < argc) {
            cap_cfg.sensor_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--strength") == 0 && i + 1 < argc) {
            strength = (float)atof(argv[++i]);
        }
    }

    LOG_INFO("%s v%s — CoTF 实时曝光校正（相机→ISP+Gamma→HDMI，控制线程低频刷新，零 NPU）",
             SOCCHINA_APP_NAME, SOCCHINA_APP_VERSION);

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    if (capture_query_in_size(&in_w, &in_h) != 0) {
        return 1;
    }

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

    if (pthread_create(&ctrl_tid, TD_NULL, control_worker, &strength) != 0) {
        LOG_ERR("control thread create failed");
        goto cleanup;
    }
    ctrl_up = 1;
    LOG_INFO("running: 显示线程 30fps + 控制线程 %ums；Ctrl-C 优雅退出", APP_CTRL_PERIOD_MS);

    /* 显示线程（主）：全分辨率相机帧直通到 HDMI，~30fps。 */
    t0 = now_ms();
    t_last_log = t0;
    while (!g_stop) {
        if (vpss_get_frame(0, 0, &frame, PIPELINE_FRAME_TIMEOUT_MS) != 0) {
            LOG_WARN("vpss_get_frame timeout/err");
            continue;
        }
        if (display_send_frame(&frame, -1) != 0) {
            (void)vpss_release_frame(0, 0, &frame);
            goto cleanup_running;
        }
        (void)vpss_release_frame(0, 0, &frame);
        frames++;
        if (now_ms() - t_last_log >= 5000) {
            LOG_INFO("display %llu frames, %.1f fps", (unsigned long long)frames,
                     frames * 1000.0 / (now_ms() - t0));
            t_last_log = now_ms();
        }
    }
    rc = 0;

cleanup_running:
    g_stop = 1;
cleanup:
    if (ctrl_up) {
        (void)pthread_join(ctrl_tid, TD_NULL);
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
    LOG_INFO("%s exit (%llu frames displayed)", SOCCHINA_APP_NAME, (unsigned long long)frames);
    return rc;
}

#else /* !WITH_SS928_SDK */

/* SDK-free（无硬件）：仅验证构建闭环与控制逻辑，不起数据通路。 */
int main(void)
{
    luma_stats_t demo = {30.0f, 0.1f, 25.0f};
    LOG_INFO("%s v%s — board app skeleton (no SS928 SDK)", SOCCHINA_APP_NAME, SOCCHINA_APP_VERSION);
    LOG_INFO("control_decide(demo) = %d", control_decide(&demo));
    return 0;
}

#endif /* WITH_SS928_SDK */
