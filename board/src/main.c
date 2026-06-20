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
 * 这是 CoTF 路线"低分辨率出参数 + 硬件全分辨率施加"的产品形态：规则 Gamma 负责曝光兜底，
 * 可选 param-net 从 chn2 缩略图预测 RGB LUT，经安全 bridge 后低频热刷 ISP CLUT。
 * SDK-free 构建（无硬件）退化为骨架自检。
 */

#ifdef WITH_SS928_SDK

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "capture.h"
#include "display.h"
#include "infer.h"
#include "isp.h"
#include "lut_bridge.h"
#include "stream.h"
#include "vpss.h"

#include "ot_buffer.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_type.h"
#include "sample_comm.h"

#define APP_CTRL_PERIOD_MS      100u  /* 控制环周期（~10Hz，低频；刷新由迟滞/限流再收敛） */
#define APP_CTRL_WARMUP_MS      1000u /* AE 收敛前不读统计 */
#define APP_TONE_STRENGTH       0.25f
#define APP_STREAM_BITRATE_KBPS 3000u
#define APP_RTSP_PORT           8554u
#define APP_RTSP_PATH           "live"
#define APP_CONTROL_TIMEOUT_MS  200
#define APP_NN_FAILURE_LIMIT    3u
#define APP_CLUT_UNITY_GAIN     1024u
#define APP_CLUT_MAX_STEP_10BIT 64u
#define APP_NN_HIGH_CLIP_GUARD  3.0f
#define APP_TIMING_SAMPLES      256u

static volatile sig_atomic_t g_stop = 0;
static atomic_uint_fast64_t g_stream_frames;
static atomic_uint_fast64_t g_stream_drops;

typedef struct {
    float strength;
    float high_clip_guard;
    int nn_enabled;
    int fatal_error;
    control_health_t health;
    uint64_t polls;
    uint64_t infer_runs;
    uint64_t lut_updates;
    uint64_t lut_failures;
    float infer_last_ms;
    float infer_max_ms;
    float infer_total_last_ms;
    float infer_total_max_ms;
    float transaction_last_ms;
    float transaction_max_ms;
    float infer_samples[APP_TIMING_SAMPLES];
    float transaction_samples[APP_TIMING_SAMPLES];
    unsigned timing_samples;
} app_control_ctx_t;

static void on_sig(int s) {
    (void) s;
    g_stop = 1;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

static int compare_float(const void *a, const void *b)
{
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static float percentile95(const float *samples, unsigned count)
{
    float sorted[APP_TIMING_SAMPLES];
    unsigned index;

    if (samples == NULL || count == 0) {
        return 0.0f;
    }
    if (count > APP_TIMING_SAMPLES) {
        count = APP_TIMING_SAMPLES;
    }
    memcpy(sorted, samples, count * sizeof(sorted[0]));
    qsort(sorted, count, sizeof(sorted[0]), compare_float);
    index = (95u * count + 99u) / 100u;
    if (index == 0) {
        index = 1;
    }
    return sorted[index - 1];
}

static isp_tone_t mode_to_tone(expo_mode_t m) {
    switch (m) {
        case EXPO_MODE_BRIGHTEN:
            return ISP_TONE_BRIGHTEN;
        case EXPO_MODE_COMPRESS:
            return ISP_TONE_COMPRESS;
        case EXPO_MODE_BIDIR:
            return ISP_TONE_BIDIR;
        default:
            return ISP_TONE_BYPASS;
    }
}

static const char* mode_name(expo_mode_t m) {
    switch (m) {
        case EXPO_MODE_BRIGHTEN:
            return "BRIGHTEN";
        case EXPO_MODE_COMPRESS:
            return "COMPRESS";
        case EXPO_MODE_BIDIR:
            return "BIDIR";
        default:
            return "BYPASS";
    }
}

/*
 * 控制线程：低频读 AE 统计 → 判决 → 规则 Gamma；需要刷新时可额外执行
 * chn2→AIPP/NN→安全 bridge→CLUT。推理后立即归还借用帧，ISP 写入成功才提交版本。
 * 连续失败进入 sticky degraded，关闭 CLUT 并保留规则 Gamma；显示/串流继续。
 */
static void* control_worker(void* arg) {
    app_control_ctx_t *ctx = (app_control_ctx_t *)arg;
    luma_stats_t raw_stats, cur;
    control_feedback_t feedback;
    static float raw_lut[PIPELINE_COTF_LUT_FLOAT_COUNT];
    static uint32_t packed_lut[PIPELINE_ISP_CLUT_NODE_COUNT];
    static uint32_t previous_lut[PIPELINE_ISP_CLUT_NODE_COUNT];
    int have_previous_lut = 0;
    lut_bridge_cfg_t bridge_cfg;

    lut_bridge_default_cfg(&bridge_cfg);
    bridge_cfg.strength = ctx->strength;
    control_feedback_init(&feedback);
    (void)isp_set_clut(ISP_BLOCK_OFF, APP_CLUT_UNITY_GAIN, APP_CLUT_UNITY_GAIN,
                       APP_CLUT_UNITY_GAIN);
    (void) isp_gamma_apply_tone(ISP_TONE_BYPASS, ctx->strength); /* 缓存默认 Gamma */
    usleep(APP_CTRL_WARMUP_MS * 1000);                           /* 等 AE 收敛 */

    while (!g_stop) {
        expo_mode_t mode;
        int refresh;

        usleep(APP_CTRL_PERIOD_MS * 1000);
        ctx->polls++;
        if (isp_get_luma_stats(&raw_stats) != 0) {
            continue;
        }
        refresh = control_feedback_observe(&feedback, &raw_stats, &cur);
        if (!refresh) {
            continue;
        }
        mode = control_decide(&cur);

        /* Gamma/DRC/CLUT 按用途分流：当前模型只产 RGB 3D-LUT；曝光色调仍由规则 Gamma 负责。
         * DRC 保持 ISP 自动配置，避免用 RGB LUT 冒充动态范围局部参数。 */
        if (isp_gamma_apply_tone(mode_to_tone(mode), ctx->strength) != 0) {
            LOG_WARN("[ctrl] gamma update rejected; keeping previous ISP parameters");
            continue;
        }

        /* 已明显过曝时旁路 RGB LUT，避免模型继续推高 pre-CLUT 已裁剪区域。 */
        if (ctx->nn_enabled && !ctx->health.degraded &&
            cur.clip_high_pct > ctx->high_clip_guard) {
            (void)isp_set_clut(ISP_BLOCK_OFF, APP_CLUT_UNITY_GAIN, APP_CLUT_UNITY_GAIN,
                               APP_CLUT_UNITY_GAIN);
            have_previous_lut = 0;
            control_feedback_commit(&feedback);
            LOG_WARN("[ctrl] NN CLUT bypassed by high-clip guard (%.1f%% > %.1f%%); "
                     "fallback=Gamma/DRC",
                     cur.clip_high_pct, ctx->high_clip_guard);
            continue;
        }

        if (ctx->nn_enabled && !ctx->health.degraded) {
            ot_video_frame_info control_frame;
            infer_timing_t timing = {0};
            long transaction_start_us = now_us();
            int got_frame = 0;
            int infer_rc = PIPELINE_ERR_IO;
            int transaction_ok = 0;

            if (vpss_get_frame(PIPELINE_VPSS_GRP, PIPELINE_VPSS_CHN_CONTROL, &control_frame,
                               APP_CONTROL_TIMEOUT_MS) == 0) {
                got_frame = 1;
                infer_rc = infer_run_nv21(&control_frame, raw_lut,
                                          PIPELINE_COTF_LUT_FLOAT_COUNT, &timing);
                ctx->infer_runs++;
                ctx->infer_last_ms = timing.execute_ms;
                if (timing.execute_ms > ctx->infer_max_ms) {
                    ctx->infer_max_ms = timing.execute_ms;
                }
                ctx->infer_total_last_ms = timing.total_ms;
                if (timing.total_ms > ctx->infer_total_max_ms) {
                    ctx->infer_total_max_ms = timing.total_ms;
                }
                if (vpss_release_frame(PIPELINE_VPSS_GRP, PIPELINE_VPSS_CHN_CONTROL,
                                       &control_frame) != 0) {
                    LOG_ERR("[ctrl] failed to release chn2 frame");
                    infer_rc = PIPELINE_ERR_IO;
                }
                got_frame = 0;
            }
            if (got_frame) {
                (void)vpss_release_frame(PIPELINE_VPSS_GRP, PIPELINE_VPSS_CHN_CONTROL,
                                         &control_frame);
            }
            if (infer_rc == PIPELINE_ERR_RUNTIME) {
                LOG_ERR("[ctrl] fatal ACL runtime failure; stopping pipeline for clean recovery");
                ctx->fatal_error = 1;
                g_stop = 1;
                break;
            }
            if (infer_rc == PIPELINE_OK &&
                lut_bridge_pack(&bridge_cfg, raw_lut, PIPELINE_COTF_LUT_FLOAT_COUNT,
                                packed_lut, PIPELINE_ISP_CLUT_NODE_COUNT) == 0 &&
                (!have_previous_lut ||
                 lut_bridge_limit_packed_step(previous_lut, packed_lut,
                                              PIPELINE_ISP_CLUT_NODE_COUNT,
                                              APP_CLUT_MAX_STEP_10BIT) == 0) &&
                isp_load_clut_lut(packed_lut, PIPELINE_ISP_CLUT_NODE_COUNT) == 0 &&
                isp_set_clut(ISP_BLOCK_AUTO, APP_CLUT_UNITY_GAIN, APP_CLUT_UNITY_GAIN,
                             APP_CLUT_UNITY_GAIN) == 0) {
                transaction_ok = 1;
            }

            if (transaction_ok) {
                float transaction_ms = (now_us() - transaction_start_us) / 1000.0f;
                unsigned sample_index = ctx->timing_samples % APP_TIMING_SAMPLES;

                (void)control_health_record(&ctx->health, 1, APP_NN_FAILURE_LIMIT);
                ctx->lut_updates++;
                ctx->transaction_last_ms = transaction_ms;
                if (transaction_ms > ctx->transaction_max_ms) {
                    ctx->transaction_max_ms = transaction_ms;
                }
                ctx->infer_samples[sample_index] = timing.execute_ms;
                ctx->transaction_samples[sample_index] = transaction_ms;
                ctx->timing_samples++;
                memcpy(previous_lut, packed_lut, sizeof(previous_lut));
                have_previous_lut = 1;
                control_feedback_commit(&feedback);
                LOG_INFO("[ctrl] NN+CLUT v%llu -> %s luma=%.1f low=%.1f%% high=%.1f%% "
                         "exec=%.2fms infer_total=%.2fms transaction=%.2fms",
                         (unsigned long long)ctx->lut_updates, mode_name(mode), cur.mean_luma,
                         cur.clip_low_pct, cur.clip_high_pct, timing.execute_ms, timing.total_ms,
                         transaction_ms);
                continue;
            }

            ctx->lut_failures++;
            if (control_health_record(&ctx->health, 0, APP_NN_FAILURE_LIMIT)) {
                (void)isp_set_clut(ISP_BLOCK_OFF, APP_CLUT_UNITY_GAIN, APP_CLUT_UNITY_GAIN,
                                   APP_CLUT_UNITY_GAIN);
                LOG_ERR("[ctrl] NN control degraded after %u consecutive failures; "
                        "fallback=rule Gamma",
                        ctx->health.consecutive_failures);
                control_feedback_commit(&feedback);
            } else {
                LOG_WARN("[ctrl] NN refresh failed (%u/%u); keeping previous CLUT",
                         ctx->health.consecutive_failures, APP_NN_FAILURE_LIMIT);
            }
            continue;
        }

        /* 无模型或已降级：稳定规则 Gamma 路径。 */
        {
            control_feedback_commit(&feedback);
            LOG_INFO("[ctrl] Gamma%s -> %s (luma=%.1f low=%.1f%% high=%.1f%%)",
                     ctx->health.degraded ? " [DEGRADED]" : "", mode_name(mode), cur.mean_luma,
                     cur.clip_low_pct, cur.clip_high_pct);
        }
    }
    (void)isp_set_clut(ISP_BLOCK_OFF, APP_CLUT_UNITY_GAIN, APP_CLUT_UNITY_GAIN,
                       APP_CLUT_UNITY_GAIN);
    (void) isp_gamma_apply_tone(ISP_TONE_BYPASS, ctx->strength); /* 退出前还原默认 Gamma */
    return TD_NULL;
}

static void* stream_worker(void* arg) {
    ot_video_frame_info frame;
    (void) arg;

    while (!g_stop) {
        if (vpss_get_frame(PIPELINE_VPSS_GRP, PIPELINE_VPSS_CHN_STREAM, &frame, 200) != 0) {
            atomic_fetch_add(&g_stream_drops, 1);
            continue;
        }
        if (stream_send_frame(&frame, 0) == 0) {
            atomic_fetch_add(&g_stream_frames, 1);
        } else {
            atomic_fetch_add(&g_stream_drops, 1);
        }
        if (vpss_release_frame(PIPELINE_VPSS_GRP, PIPELINE_VPSS_CHN_STREAM, &frame) != 0) {
            atomic_fetch_add(&g_stream_drops, 1);
        }
    }
    return TD_NULL;
}

int main(int argc, char** argv) {
    capture_cfg_t cap_cfg = {CAPTURE_SENSOR_INDEX_DEFAULT, CAPTURE_MODE_LINEAR};
    stream_cfg_t stream_cfg = {
        PIPELINE_STREAM_WIDTH,   PIPELINE_STREAM_HEIGHT, PIPELINE_TARGET_FPS,
        APP_STREAM_BITRATE_KBPS, APP_RTSP_PORT,          APP_RTSP_PATH,
    };
    float strength = APP_TONE_STRENGTH;
    float high_clip_guard = APP_NN_HIGH_CLIP_GUARD;
    const char *model_path = NULL;
    int enable_display = 1;
    int enable_stream = 0;
    int enable_nn = 1;
    int rc = 1;
    int sys_up = 0, cap_up = 0, vpss_up = 0, bound = 0, disp_up = 0, stream_up = 0;
    int infer_up = 0;
    int ctrl_up = 0, stream_thread_up = 0;
    unsigned in_w = 0, in_h = 0;
    long t0 = 0, t_last_log;
    long run_elapsed_ms = 0;
    uint64_t frames = 0;
    pthread_t ctrl_tid, stream_tid;
    app_control_ctx_t control_ctx = {0};
    ot_vb_cfg vb_cfg = {0};
    ot_pic_buf_attr buf_attr = {0};
    ot_vb_calc_cfg calc_cfg = {0};
    ot_video_frame_info frame;
    vpss_chn_cfg_t chn_cfg[VPSS_CHN_COUNT] = {
        {1, DISPLAY_WIDTH, DISPLAY_HEIGHT, 2}, /* chn0：显示底图 */
        {0, PIPELINE_STREAM_WIDTH, PIPELINE_STREAM_HEIGHT, 2},
        {0, 0, 0, 0},
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sensor") == 0 && i + 1 < argc) {
            cap_cfg.sensor_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--strength") == 0 && i + 1 < argc) {
            strength = (float) atof(argv[++i]);
        } else if (strcmp(argv[i], "--stream") == 0) {
            enable_stream = 1;
        } else if (strcmp(argv[i], "--no-display") == 0) {
            enable_display = 0;
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            stream_cfg.bitrate_kbps = (unsigned) strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--rtsp-port") == 0 && i + 1 < argc) {
            stream_cfg.rtsp_port = (unsigned) strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--stream-path") == 0 && i + 1 < argc) {
            stream_cfg.stream_path = argv[++i];
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_path = argv[++i];
        } else if (strcmp(argv[i], "--no-nn") == 0) {
            enable_nn = 0;
        } else if (strcmp(argv[i], "--nn-high-clip") == 0 && i + 1 < argc) {
            high_clip_guard = (float)atof(argv[++i]);
        }
    }
    if (high_clip_guard < 0.0f) {
        high_clip_guard = 0.0f;
    }
    if (high_clip_guard > 100.0f) {
        high_clip_guard = 100.0f;
    }
    if (model_path == NULL) {
        enable_nn = 0;
    }
    chn_cfg[PIPELINE_VPSS_CHN_DISPLAY].enable = enable_display;
    chn_cfg[PIPELINE_VPSS_CHN_STREAM].enable = enable_stream;
    chn_cfg[PIPELINE_VPSS_CHN_CONTROL] =
        (vpss_chn_cfg_t){enable_nn, PIPELINE_CONTROL_WIDTH, PIPELINE_CONTROL_HEIGHT, 1};
    control_ctx.strength = strength;
    control_ctx.high_clip_guard = high_clip_guard;
    control_ctx.nn_enabled = enable_nn;
    control_health_init(&control_ctx.health);

    LOG_INFO("%s v%s — CoTF realtime exposure correction (display=%s stream=%s nn=%s)",
             SOCCHINA_APP_NAME, SOCCHINA_APP_VERSION, enable_display ? "on" : "off",
             enable_stream ? "on" : "off", enable_nn ? model_path : "rule-gamma");

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
    vb_cfg.common_pool[0].blk_cnt = 10 + (enable_stream ? 2 : 0) + (enable_nn ? 2 : 0);
    CHECK_RET_GOTO(sample_comm_sys_init_with_vb_supplement(&vb_cfg, OT_VB_SUPPLEMENT_BNR_MOT_MASK), cleanup);
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
    if (enable_display) {
        if (display_init() != 0) {
            goto cleanup;
        }
        disp_up = 1;
    }
    if (enable_stream) {
        if (stream_init(&stream_cfg) != 0) {
            goto cleanup;
        }
        stream_up = 1;
    }
    if (enable_nn) {
        infer_cfg_t infer_cfg = {
            model_path, 0, PIPELINE_CONTROL_WIDTH, PIPELINE_CONTROL_HEIGHT,
            PIPELINE_COTF_LUT_DIM, PIPELINE_INPUT_COPY,
        };
        if (infer_init(&infer_cfg) != 0) {
            LOG_ERR("NN init failed; continuing in PIPELINE_DEGRADED with rule Gamma");
            control_ctx.nn_enabled = 0;
            control_health_init(&control_ctx.health);
            control_ctx.health.degraded = 1;
        } else {
            infer_up = 1;
        }
    }

    if (pthread_create(&ctrl_tid, TD_NULL, control_worker, &control_ctx) != 0) {
        LOG_ERR("control thread create failed");
        goto cleanup;
    }
    ctrl_up = 1;
    if (enable_stream) {
        if (pthread_create(&stream_tid, TD_NULL, stream_worker, TD_NULL) != 0) {
            LOG_ERR("stream thread create failed");
            goto cleanup_running;
        }
        stream_thread_up = 1;
    }
    LOG_INFO("running: control %ums%s%s; Ctrl-C to stop", APP_CTRL_PERIOD_MS,
             enable_display ? " + display 30fps" : "", enable_stream ? " + H.264/RTSP" : "");

    /* 显示线程（主）：全分辨率相机帧直通到 HDMI，~30fps。 */
    t0 = now_ms();
    t_last_log = t0;
    while (!g_stop) {
        if (!enable_display) {
            usleep(100000);
            if (now_ms() - t_last_log >= 5000) {
                if (enable_stream) {
                    LOG_INFO("stream %llu frames, %llu drops",
                             (unsigned long long) atomic_load(&g_stream_frames),
                             (unsigned long long) atomic_load(&g_stream_drops));
                }
                t_last_log = now_ms();
            }
            continue;
        }
        if (vpss_get_frame(0, 0, &frame, PIPELINE_FRAME_TIMEOUT_MS) != 0) {
            LOG_WARN("vpss_get_frame timeout/err");
            continue;
        }
        if (display_send_frame(&frame, -1) != 0) {
            (void) vpss_release_frame(0, 0, &frame);
            goto cleanup_running;
        }
        (void) vpss_release_frame(0, 0, &frame);
        frames++;
        if (now_ms() - t_last_log >= 5000) {
            LOG_INFO("display %llu frames, %.1f fps", (unsigned long long) frames,
                     frames * 1000.0 / (now_ms() - t0));
            if (enable_stream) {
                LOG_INFO("stream %llu frames, %llu drops", (unsigned long long) atomic_load(&g_stream_frames),
                         (unsigned long long) atomic_load(&g_stream_drops));
            }
            t_last_log = now_ms();
        }
    }
    run_elapsed_ms = now_ms() - t0;
    rc = control_ctx.fatal_error ? 1 : 0;

cleanup_running:
    g_stop = 1;
cleanup:
    if (stream_thread_up) {
        (void) pthread_join(stream_tid, TD_NULL);
    }
    if (ctrl_up) {
        (void) pthread_join(ctrl_tid, TD_NULL);
    }
    if (infer_up) {
        CHECK_RET(infer_deinit());
    }
    if (stream_up) {
        CHECK_RET(stream_deinit());
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
    {
        unsigned sample_count =
            control_ctx.timing_samples < APP_TIMING_SAMPLES ? control_ctx.timing_samples
                                                            : APP_TIMING_SAMPLES;
        long elapsed_ms = run_elapsed_ms;
        double display_fps = (elapsed_ms > 0) ? frames * 1000.0 / elapsed_ms : 0.0;
        LOG_INFO("%s exit (display=%llu/%.2ffps stream=%llu drops=%llu; ctrl polls=%llu "
                 "infer=%llu lut=%llu fail=%llu state=%s infer_p95=%.2fms "
                 "transaction_p95=%.2fms infer_max=%.2fms infer_total_max=%.2fms "
                 "transaction_max=%.2fms samples=%u)",
                 SOCCHINA_APP_NAME, (unsigned long long)frames, display_fps,
                 (unsigned long long)atomic_load(&g_stream_frames),
                 (unsigned long long)atomic_load(&g_stream_drops),
                 (unsigned long long)control_ctx.polls,
                 (unsigned long long)control_ctx.infer_runs,
                 (unsigned long long)control_ctx.lut_updates,
                 (unsigned long long)control_ctx.lut_failures,
                 control_ctx.health.degraded ? "DEGRADED" : "STOPPED",
                 percentile95(control_ctx.infer_samples, sample_count),
                 percentile95(control_ctx.transaction_samples, sample_count),
                 control_ctx.infer_max_ms, control_ctx.infer_total_max_ms,
                 control_ctx.transaction_max_ms, sample_count);
    }
    return rc;
}

#else /* !WITH_SS928_SDK */

/* SDK-free（无硬件）：仅验证构建闭环与控制逻辑，不起数据通路。 */
int main(void) {
    luma_stats_t demo = {30.0f, 0.1f, 25.0f};
    LOG_INFO("%s v%s — board app skeleton (no SS928 SDK)", SOCCHINA_APP_NAME, SOCCHINA_APP_VERSION);
    LOG_INFO("control_decide(demo) = %d", control_decide(&demo));
    return 0;
}

#endif /* WITH_SS928_SDK */
