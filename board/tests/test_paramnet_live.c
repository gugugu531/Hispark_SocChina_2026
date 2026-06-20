/* test_paramnet_live — 正式 LCDP param-net 持续预览。
 *
 * 主链：OS08A20 → ISP(+当前 CLUT) → VPSS chn0 1024x600 → HDMI。
 * 控制旁路：VPSS chn2 256x144 NV21 → AIPP → param-net → 17v2 安全桥 → ISP CLUT。
 *
 * 默认每 30 帧（约 1 秒）刷新一次完整 RGB 3D-LUT，以 25% 强度混合 identity。触摸屏点击
 * 在 MODEL ON 与原图(CLUT OFF)之间切换。SIGINT/SIGTERM 或时限结束时关闭 CLUT 并逆序清理。
 */

#include "capture.h"
#include "display.h"
#include "infer.h"
#include "isp.h"
#include "log.h"
#include "lut_bridge.h"
#include "vpss.h"

#ifndef WITH_SS928_SDK

int main(void)
{
    LOG_WARN("test_paramnet_live skipped: built without SS928 SDK");
    return 0;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ot_buffer.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_type.h"
#include "sample_comm.h"
#include "ss_mpi_sys.h"

#define WARMUP_FRAMES   30u

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int touch_pressed(int fd)
{
    struct input_event event;
    ssize_t got;
    int pressed = 0;
    while ((got = read(fd, &event, sizeof(event))) == (ssize_t)sizeof(event)) {
        if (event.type == EV_KEY && event.code == BTN_TOUCH && event.value == 1) {
            pressed = 1;
        }
    }
    return pressed;
}

static int dump_nv21_frame(const ot_video_frame_info *frame, const char *path)
{
    const ot_video_frame *vf = &frame->video_frame;
    td_u32 map_size = vf->stride[0] * vf->height * 3 / 2;
    td_u8 *virt = (td_u8 *)ss_mpi_sys_mmap(vf->phys_addr[0], map_size);
    FILE *fp;
    td_u32 row;
    if (virt == TD_NULL) return -1;
    fp = fopen(path, "wb");
    if (fp == NULL) {
        (void)ss_mpi_sys_munmap(virt, map_size);
        return -1;
    }
    for (row = 0; row < vf->height; row++)
        (void)fwrite(virt + (size_t)row * vf->stride[0], 1, vf->width, fp);
    for (row = 0; row < vf->height / 2; row++)
        (void)fwrite(virt + (size_t)vf->stride[0] * vf->height +
                         (size_t)row * vf->stride[1],
                     1, vf->width, fp);
    fclose(fp);
    (void)ss_mpi_sys_munmap(virt, map_size);
    LOG_INFO("dump: %ux%u NV21 -> %s", vf->width, vf->height, path);
    return 0;
}

/* 检查模型输出并统计相对 identity 的变化；正式打包由 lut_bridge 完成。 */
static int inspect_lut(const float *raw, float *raw_min, float *raw_max, float *raw_mean,
                       float *mean_delta)
{
    unsigned c, r, g, b;
    size_t count = 0;
    double sum = 0.0, delta = 0.0;
    float lo = raw[0], hi = raw[0];

    for (c = 0; c < 3; c++) {
        for (r = 0; r < PIPELINE_COTF_LUT_DIM; r++) {
            for (g = 0; g < PIPELINE_COTF_LUT_DIM; g++) {
                for (b = 0; b < PIPELINE_COTF_LUT_DIM; b++) {
                    size_t index =
                        (((size_t)c * PIPELINE_COTF_LUT_DIM + r) * PIPELINE_COTF_LUT_DIM + g) *
                            PIPELINE_COTF_LUT_DIM +
                        b;
                    float value = raw[index];
                    float identity = (c == 0) ? (float)r / 16.0f
                                              : ((c == 1) ? (float)g / 16.0f
                                                          : (float)b / 16.0f);
                    if (!isfinite(value)) {
                        return -1;
                    }
                    if (value < lo) lo = value;
                    if (value > hi) hi = value;
                    if (value < 0.0f) value = 0.0f;
                    if (value > 1.0f) value = 1.0f;
                    sum += value;
                    delta += fabsf(value - identity);
                    count++;
                }
            }
        }
    }
    /* 明显失控的输出拒绝写入；保留上一版 LUT。 */
    if (lo < -0.05f || hi > 1.05f) {
        return -1;
    }
    *raw_min = lo;
    *raw_max = hi;
    *raw_mean = (float)(sum / count);
    *mean_delta = (float)(delta / count);
    return 0;
}

int main(int argc, char **argv)
{
    int run_sec = 86400;
    int sensor = CAPTURE_SENSOR_INDEX_DEFAULT;
    unsigned refresh_frames = 30;
    float strength = 0.25f;
    const char *touch_dev = "/dev/input/event0";
    const char *dump_dir = NULL;
    const char *model = "/root/socchina-2026/cotf_paramnet_256x144_lcdp_best_e0167_fp16_aipp.om";
    capture_cfg_t cap_cfg;
    int rc = 1, sys_up = 0, cap_up = 0, vpss_up = 0, bound = 0, display_up = 0;
    int infer_up = 0, touch_fd = -1, model_on = 1, have_lut = 0;
    int dump_phase = 0, dump_countdown = 0, dump_saved_lut = 0;
    unsigned in_w = 0, in_h = 0, frames = 0, infer_ok = 0, infer_fail = 0, updates = 0;
    long started, last_log;
    float exec_sum = 0.0f, exec_max = 0.0f;
    static float raw_lut[PIPELINE_COTF_LUT_FLOAT_COUNT];
    static unsigned int packed_lut[PIPELINE_ISP_CLUT_NODE_COUNT];
    lut_bridge_cfg_t bridge_cfg;
    ot_vb_cfg vb_cfg = {0};
    ot_pic_buf_attr buf_attr = {0};
    ot_vb_calc_cfg calc_cfg = {0};
    ot_video_frame_info display_frame, control_frame;
    vpss_chn_cfg_t chn_cfg[VPSS_CHN_COUNT] = {
        {1, DISPLAY_WIDTH, DISPLAY_HEIGHT, 2},
        {0, 0, 0, 0},
        {1, PIPELINE_CONTROL_WIDTH, PIPELINE_CONTROL_HEIGHT, 2},
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model = argv[++i];
        } else if (strcmp(argv[i], "--sensor") == 0 && i + 1 < argc) {
            sensor = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--refresh") == 0 && i + 1 < argc) {
            refresh_frames = (unsigned)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--strength") == 0 && i + 1 < argc) {
            strength = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--touch") == 0 && i + 1 < argc) {
            touch_dev = argv[++i];
        } else if (strcmp(argv[i], "--dumpdir") == 0 && i + 1 < argc) {
            dump_dir = argv[++i];
        } else {
            run_sec = atoi(argv[i]);
        }
    }
    if (refresh_frames == 0) refresh_frames = 30;
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;
    cap_cfg.sensor_index = sensor;
    cap_cfg.mode = CAPTURE_MODE_LINEAR;
    lut_bridge_default_cfg(&bridge_cfg);
    bridge_cfg.strength = strength;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (capture_query_in_size(&in_w, &in_h) != 0) {
        return 1;
    }
    LOG_INFO("test_paramnet_live: %ds model=%s sensor=%d strength=%.2f refresh=%u",
             run_sec, model, sensor, strength, refresh_frames);

    buf_attr.width = in_w;
    buf_attr.height = in_h;
    buf_attr.align = OT_DEFAULT_ALIGN;
    buf_attr.bit_width = OT_DATA_BIT_WIDTH_8;
    buf_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    buf_attr.compress_mode = OT_COMPRESS_MODE_SEG;
    ot_common_get_pic_buf_cfg(&buf_attr, &calc_cfg);
    vb_cfg.max_pool_cnt = 2;
    vb_cfg.common_pool[0].blk_size = calc_cfg.vb_size;
    vb_cfg.common_pool[0].blk_cnt = 12;
    CHECK_RET_GOTO(sample_comm_sys_init_with_vb_supplement(&vb_cfg, OT_VB_SUPPLEMENT_BNR_MOT_MASK),
                   cleanup);
    sys_up = 1;
    if (capture_init(&cap_cfg) != 0) goto cleanup;
    cap_up = 1;
    if (vpss_init(0, in_w, in_h, chn_cfg) != 0) goto cleanup;
    vpss_up = 1;
    if (capture_bind_vpss(0) != 0) goto cleanup;
    bound = 1;
    if (display_init() != 0) goto cleanup;
    display_up = 1;
    {
        infer_cfg_t cfg = {
            model, 0, PIPELINE_CONTROL_WIDTH, PIPELINE_CONTROL_HEIGHT,
            PIPELINE_COTF_LUT_DIM, PIPELINE_INPUT_COPY,
        };
        if (infer_init(&cfg) != 0) goto cleanup;
    }
    infer_up = 1;
    (void)isp_set_clut(ISP_BLOCK_OFF, 1024, 1024, 1024);

    touch_fd = open(touch_dev, O_RDONLY | O_NONBLOCK);
    if (touch_fd >= 0) {
        LOG_INFO("==> 触摸屏切换 [MODEL ON <-> 原图]；当前等待首个安全 LUT");
    } else {
        LOG_WARN("touch unavailable: %s (%s)", touch_dev, strerror(errno));
    }

    started = now_ms();
    last_log = started;
    while (!g_stop && now_ms() - started < (long)run_sec * 1000L) {
        if (touch_fd >= 0 && touch_pressed(touch_fd)) {
            model_on = !model_on;
            if (model_on && have_lut) {
                (void)isp_set_clut(ISP_BLOCK_AUTO, 1024, 1024, 1024);
            } else {
                (void)isp_set_clut(ISP_BLOCK_OFF, 1024, 1024, 1024);
            }
            LOG_INFO(">>> %s", model_on ? "MODEL ON" : "MODEL OFF：原始相机图");
        }

        if (vpss_get_frame(0, PIPELINE_VPSS_CHN_DISPLAY, &display_frame, 1000) != 0) {
            goto cleanup;
        }
        if (display_send_frame(&display_frame, -1) != 0) {
            (void)vpss_release_frame(0, PIPELINE_VPSS_CHN_DISPLAY, &display_frame);
            goto cleanup;
        }
        if (dump_countdown > 0 && --dump_countdown == 0 && dump_dir != NULL) {
            char path[256];
            if (dump_phase == 1) {
                snprintf(path, sizeof(path), "%s/model_on_before.nv21", dump_dir);
                (void)dump_nv21_frame(&display_frame, path);
                (void)isp_set_clut(ISP_BLOCK_OFF, 1024, 1024, 1024);
                dump_phase = 2;
                dump_countdown = 3;
            } else if (dump_phase == 2) {
                snprintf(path, sizeof(path), "%s/model_off.nv21", dump_dir);
                (void)dump_nv21_frame(&display_frame, path);
                (void)isp_set_clut(ISP_BLOCK_AUTO, 1024, 1024, 1024);
                dump_phase = 3;
                dump_countdown = 3;
            } else if (dump_phase == 3) {
                snprintf(path, sizeof(path), "%s/model_on_after.nv21", dump_dir);
                (void)dump_nv21_frame(&display_frame, path);
                dump_phase = 4;
                LOG_INFO("dump sequence complete");
            }
        }
        (void)vpss_release_frame(0, PIPELINE_VPSS_CHN_DISPLAY, &display_frame);
        frames++;

        if (frames >= WARMUP_FRAMES && frames % refresh_frames == 0) {
            infer_timing_t timing = {0};
            float raw_min, raw_max, raw_mean, mean_delta;
            if (vpss_get_frame(0, PIPELINE_VPSS_CHN_CONTROL, &control_frame, 1000) != 0) {
                infer_fail++;
                continue;
            }
            if (infer_run_nv21(&control_frame, raw_lut, PIPELINE_COTF_LUT_FLOAT_COUNT,
                               &timing) == 0 &&
                inspect_lut(raw_lut, &raw_min, &raw_max, &raw_mean, &mean_delta) == 0 &&
                lut_bridge_pack(&bridge_cfg, raw_lut, PIPELINE_COTF_LUT_FLOAT_COUNT,
                                packed_lut, PIPELINE_ISP_CLUT_NODE_COUNT) == 0 &&
                isp_load_clut_lut(packed_lut, PIPELINE_ISP_CLUT_NODE_COUNT) == 0) {
                have_lut = 1;
                infer_ok++;
                updates++;
                exec_sum += timing.execute_ms;
                if (timing.execute_ms > exec_max) exec_max = timing.execute_ms;
                if (model_on) {
                    (void)isp_set_clut(ISP_BLOCK_AUTO, 1024, 1024, 1024);
                }
                if (dump_dir != NULL && !dump_saved_lut) {
                    char raw_path[256], packed_path[256];
                    FILE *fp;
                    snprintf(raw_path, sizeof(raw_path), "%s/model_raw.f32", dump_dir);
                    snprintf(packed_path, sizeof(packed_path), "%s/model_packed.u32", dump_dir);
                    fp = fopen(raw_path, "wb");
                    if (fp != NULL) {
                        (void)fwrite(raw_lut, sizeof(float), PIPELINE_COTF_LUT_FLOAT_COUNT, fp);
                        fclose(fp);
                    }
                    fp = fopen(packed_path, "wb");
                    if (fp != NULL) {
                        (void)fwrite(packed_lut, sizeof(unsigned int),
                                     PIPELINE_ISP_CLUT_NODE_COUNT, fp);
                        fclose(fp);
                    }
                    dump_saved_lut = 1;
                    dump_phase = 1;
                    dump_countdown = 3;
                }
                LOG_INFO("[model] update=%u exec=%.2fms raw[min=%.3f max=%.3f mean=%.3f] "
                         "identity_delta=%.3f strength=%.2f",
                         updates, timing.execute_ms, raw_min, raw_max, raw_mean, mean_delta,
                         strength);
            } else {
                infer_fail++;
                LOG_WARN("[model] inference/safety/CLUT update rejected; keeping previous LUT");
            }
            (void)vpss_release_frame(0, PIPELINE_VPSS_CHN_CONTROL, &control_frame);
        }

        if (now_ms() - last_log >= 2000) {
            LOG_INFO("frames=%u fps=%.1f model=%s updates=%u infer_ok=%u fail=%u exec_avg=%.2fms "
                     "exec_max=%.2fms",
                     frames, frames * 1000.0 / (now_ms() - started),
                     (model_on && have_lut) ? "ON" : "OFF", updates, infer_ok, infer_fail,
                     infer_ok ? exec_sum / infer_ok : 0.0f, exec_max);
            last_log = now_ms();
        }
    }
    rc = 0;

cleanup:
    if (cap_up) (void)isp_set_clut(ISP_BLOCK_OFF, 1024, 1024, 1024);
    if (touch_fd >= 0) close(touch_fd);
    if (infer_up) (void)infer_deinit();
    if (display_up) CHECK_RET(display_deinit());
    if (bound) CHECK_RET(capture_unbind_vpss(0));
    if (vpss_up) CHECK_RET(vpss_deinit(0));
    if (cap_up) CHECK_RET(capture_deinit());
    if (sys_up) sample_comm_sys_exit();
    LOG_INFO("test_paramnet_live exit rc=%d frames=%u updates=%u", rc, frames, updates);
    return rc;
}

#endif
