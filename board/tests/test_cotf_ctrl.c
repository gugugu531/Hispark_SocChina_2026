/* test_cotf_ctrl — CoTF 场景自适应曝光校正「独立控制面」（触硬件）。
 *
 * 与厂商 sample_vio（或本仓库 capture 链）**并行**运行：本进程不碰采集/VPSS/VO，只在**正在运行**的
 * ISP pipe0 上做控制面动作——读 AE 统计 → control_decide → 几何无关 CLUT tone 施加，触摸 toggle。
 * 这是 architecture.md §2「场景自适应控制大脑」的进程级实现：控制面与数据通路解耦。
 *
 * 用途：当数据通路由别的进程（厂商 sample_vio）提供时，本程序把 CoTF 校正注入其 ISP CLUT。
 * 用法：./test_cotf_ctrl [秒数] [--strength <0..1>] [--touch <dev>] [--hz <控制环频率>] [--sysinit]
 * 前置：ISP pipe0 已在跑（先起 sample_vio 等相机预览）。
 */

#include "control.h"
#include "isp.h"
#include "log.h"

#ifndef WITH_SS928_SDK

int main(void)
{
    LOG_WARN("test_cotf_ctrl skipped: built without SS928 SDK (host build)");
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
#include <signal.h>
#include <linux/input.h>

#include "ot_isp_define.h"
#include "ot_type.h"
#include "ss_mpi_sys.h"

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

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
    while (read(tfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 1) {
            pressed = 1;
        }
    }
    return pressed;
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

int main(int argc, char **argv)
{
    int run_sec = 600;
    float strength = 0.7f;
    const char *touch_dev = "/dev/input/event0";
    int hz = 20, do_sysinit = 0, cycle = 0;
    static unsigned int base_lut[OT_ISP_CLUT_LUT_LENGTH];
    luma_stats_t prev_stats, cur_stats;
    int have_prev = 0, auto_on = 1, tfd = -1;
    unsigned tick = 0, last_refresh = 0;
    expo_mode_t cur_mode = EXPO_MODE_BYPASS;
    long t0, t_last_log;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--strength") == 0 && i + 1 < argc) strength = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--touch") == 0 && i + 1 < argc) touch_dev = argv[++i];
        else if (strcmp(argv[i], "--hz") == 0 && i + 1 < argc) hz = atoi(argv[++i]);
        else if (strcmp(argv[i], "--sysinit") == 0) do_sysinit = 1;
        else if (strcmp(argv[i], "--cycle") == 0) cycle = 1;
        else run_sec = atoi(argv[i]);
    }
    if (hz <= 0) hz = 20;
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    /* 默认不碰 sys（避免扰动占用数据通路的进程）；若 MPI 需要用户态 sys 上下文再开 --sysinit。 */
    if (do_sysinit) {
        if (ss_mpi_sys_init() != TD_SUCCESS) {
            LOG_WARN("ss_mpi_sys_init failed (可能已由数据通路进程初始化)");
        }
    }

    /* 读回运行中 ISP 的默认 CLUT 表作为几何正确基线。 */
    if (isp_clut_get_coeff(base_lut, OT_ISP_CLUT_LUT_LENGTH) != 0) {
        LOG_ERR("读 CLUT 系数失败：ISP pipe0 未在跑？先起 sample_vio 预览。可加 --sysinit 重试。");
        return 1;
    }
    LOG_INFO("test_cotf_ctrl: 控制面接管 ISP CLUT；%ds strength=%.2f hz=%d touch=%s",
             run_sec, strength, hz, touch_dev);
    (void)isp_set_clut(ISP_BLOCK_OFF, 1024, 1024, 1024);

    tfd = open(touch_dev, O_RDONLY | O_NONBLOCK);
    if (tfd < 0) {
        LOG_WARN("touch open %s 失败(%s)", touch_dev, strerror(errno));
    } else if (cycle) {
        LOG_INFO("==> 触摸屏幕循环切换 tone: BYPASS→BRIGHTEN→COMPRESS→BIDIR（手动选场景）");
    } else {
        LOG_INFO("==> 触摸屏幕切换 自动曝光校正 [ON 场景自适应 <-> OFF 原图]");
    }

    t0 = now_ms();
    t_last_log = t0;
    while (!g_stop && now_ms() - t0 < run_sec * 1000L) {
        int pressed = (tfd >= 0 && touch_pressed(tfd));
        if (cycle) {
            /* AE 统计被数据通路进程独占时（cross-process 0xa01c8045），用触摸手动循环各 tone 演示；
             * 同时每 3s 自动推进一次，便于无人值守自检/演示。 */
            static long last_adv = 0;
            if (last_adv == 0) last_adv = now_ms();
            if (pressed || now_ms() - last_adv >= 3000) {
                cur_mode = (expo_mode_t)((cur_mode + 1) % 4); /* BYPASS..BIDIR */
                int ar = isp_clut_apply_tone(base_lut, OT_ISP_CLUT_LUT_LENGTH,
                                             mode_to_tone(cur_mode), strength);
                LOG_INFO(">>> tone -> %s (apply rc=%d)", mode_name(cur_mode), ar);
                last_adv = now_ms();
            }
            tick++;
            if (now_ms() - t_last_log >= 2000) {
                LOG_INFO("tick=%u mode=%s (点屏循环)", tick, mode_name(cur_mode));
                t_last_log = now_ms();
            }
            usleep(1000000 / hz);
            continue;
        }
        if (pressed) {
            auto_on = !auto_on;
            if (!auto_on) {
                (void)isp_set_clut(ISP_BLOCK_OFF, 1024, 1024, 1024);
                LOG_INFO(">>> AUTO OFF (原图)");
            } else {
                have_prev = 0;
                LOG_INFO(">>> AUTO ON (场景自适应)");
            }
        }
        if (auto_on && isp_get_luma_stats(&cur_stats) == 0) {
            if (control_should_refresh_lut(have_prev ? &prev_stats : NULL, &cur_stats,
                                           tick - last_refresh)) {
                cur_mode = control_decide(&cur_stats);
                (void)isp_clut_apply_tone(base_lut, OT_ISP_CLUT_LUT_LENGTH,
                                          mode_to_tone(cur_mode), strength);
                prev_stats = cur_stats;
                have_prev = 1;
                last_refresh = tick;
                LOG_INFO("[ctrl] -> %s (luma=%.1f low=%.1f%% high=%.1f%%)", mode_name(cur_mode),
                         cur_stats.mean_luma, cur_stats.clip_low_pct, cur_stats.clip_high_pct);
            }
        }
        tick++;
        if (now_ms() - t_last_log >= 2000) {
            LOG_INFO("tick=%u auto=%s mode=%s", tick, auto_on ? "ON" : "OFF", mode_name(cur_mode));
            t_last_log = now_ms();
        }
        usleep(1000000 / hz);
    }
    (void)isp_set_clut(ISP_BLOCK_OFF, 1024, 1024, 1024);
    if (tfd >= 0) close(tfd);
    LOG_INFO("test_cotf_ctrl exit.");
    return 0;
}

#endif /* WITH_SS928_SDK */
