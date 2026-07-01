#include "control.h"
#include "log.h"
#include "menu_render.h"
#include "touch.h"
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

#include "app_control.h"
#include "capture.h"
#include "display.h"
#include "infer.h"
#include "isp.h"
#include "lut_bridge.h"
#include "stream.h"
#include "vpss.h"

#ifdef WITH_SS928_SDK
#include "ctbg_upsample.h"
#include "infer_ctbg.h"
#endif

#include "ot_buffer.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_type.h"
#include "sample_comm.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_vb.h"

#define APP_CTRL_PERIOD_MS      100u  /* 控制环周期（~10Hz，低频；刷新由迟滞/限流再收敛） */
#define APP_CTRL_WARMUP_MS      1000u /* AE 收敛前不读统计 */
#define APP_CONTROL_TIMEOUT_MS  200
#define APP_NN_FAILURE_LIMIT    3u
#define APP_CLUT_UNITY_GAIN     1024u
#define APP_CLUT_MAX_STEP_10BIT 64u
#define APP_NN_HIGH_CLIP_GUARD  3.0f
#define APP_TIMING_SAMPLES      256u

static volatile sig_atomic_t g_stop = 0;

/* ---- CTBG 全局（v9 6ch 双 OM 路线） ---- */
static volatile int g_ctbg_mode = 0;
static volatile int g_ctbg_writeback = 0;     /* --ctbg-writeback：启用写回（~23fps），默认仅诊断 */
static uint16_t *g_ctbg_coeff = NULL;        /* 预上采样 6ch 系数（full_h×full_w，供 v9 apply 用） */
static uint16_t *g_ctbg_coeff_raw = NULL;    /* estimator 原始输出 6ch（low_h×low_w fp16） */
static uint8_t  *g_ctbg_nv21_low = NULL;     /* estimator NV21 输入临时缓冲（256×144×3/2） */
static size_t     g_ctbg_coeff_sz = 0;       /* 预上采样系数大小（= apply OM 系数输入大小） */
static size_t     g_ctbg_coeff_raw_sz = 0;   /* estimator 输出大小（6ch raw） */
static pthread_mutex_t g_ctbg_coeff_mutex = PTHREAD_MUTEX_INITIALIZER;
static int        g_ctbg_coeff_ready = 0;    /* 首个系数已就绪 */
static atomic_int g_ctbg_scene_changed = 0;  /* 场景变化标志，stream 写回消费后清零 */

typedef struct {
    atomic_int state;
    atomic_uint_fast64_t display_frames;
    atomic_uint_fast64_t display_drops;
    atomic_uint_fast64_t stream_frames;
    atomic_uint_fast64_t stream_drops;
    atomic_uint_fast64_t control_polls;
    atomic_uint_fast64_t frame_timeouts;
    atomic_uint_fast64_t infer_runs;
    atomic_uint_fast64_t lut_requests;
    atomic_uint_fast64_t lut_updates;
    atomic_uint_fast64_t lut_failures;
    atomic_uint_fast64_t transient_errors;
    atomic_uint_fast64_t fatal_errors;
    atomic_uint_fast64_t degraded_events;
    float infer_last_ms;
    float infer_max_ms;
    float infer_total_last_ms;
    float infer_total_max_ms;
    float transaction_last_ms;
    float transaction_max_ms;
    float infer_samples[APP_TIMING_SAMPLES];
    float transaction_samples[APP_TIMING_SAMPLES];
    unsigned timing_samples;
} app_runtime_metrics_t;

typedef struct {
    float strength;
    float high_clip_guard;
    int nn_enabled;
    int enhancement_enabled;   /* 全局增强总开关（默认开） */
    int tone_enabled;          /* 规则 Gamma 色调开关（默认开） */
    int drc_mode;              /* 0 off / 1 auto / 2 manual（默认 auto） */
    int drc_strength;          /* 0..1023（manual 时生效） */
    int ldci_mode;             /* 0 off / 1 auto（默认 auto） */
    int fatal_error;
    control_health_t health;
    app_runtime_metrics_t *metrics;
} app_control_ctx_t;

static float percentile95(const float *samples, unsigned count);

static void metrics_set_state(app_runtime_metrics_t *metrics, pipeline_state_t state)
{
    atomic_store(&metrics->state, state);
    LOG_INFO("pipeline lifecycle state=%s", pipeline_state_name(state));
}

static void metrics_snapshot(const app_runtime_metrics_t *runtime, long elapsed_ms,
                             pipeline_metrics_t *out)
{
    unsigned sample_count;

    memset(out, 0, sizeof(*out));
    out->state = (pipeline_state_t)atomic_load(&runtime->state);
    out->display_frames = atomic_load(&runtime->display_frames);
    out->display_drops = atomic_load(&runtime->display_drops);
    out->stream_frames = atomic_load(&runtime->stream_frames);
    out->stream_drops = atomic_load(&runtime->stream_drops);
    out->control_polls = atomic_load(&runtime->control_polls);
    out->frame_timeouts = atomic_load(&runtime->frame_timeouts);
    out->infer_runs = atomic_load(&runtime->infer_runs);
    out->lut_requests = atomic_load(&runtime->lut_requests);
    out->lut_updates = atomic_load(&runtime->lut_updates);
    out->lut_failures = atomic_load(&runtime->lut_failures);
    out->transient_errors = atomic_load(&runtime->transient_errors);
    out->fatal_errors = atomic_load(&runtime->fatal_errors);
    out->degraded_events = atomic_load(&runtime->degraded_events);
    out->display_fps =
        (elapsed_ms > 0) ? (float)(out->display_frames * 1000.0 / elapsed_ms) : 0.0f;
    out->infer_last_ms = runtime->infer_last_ms;
    out->infer_max_ms = runtime->infer_max_ms;
    out->infer_total_last_ms = runtime->infer_total_last_ms;
    out->infer_total_max_ms = runtime->infer_total_max_ms;
    out->transaction_last_ms = runtime->transaction_last_ms;
    out->transaction_max_ms = runtime->transaction_max_ms;
    sample_count = runtime->timing_samples < APP_TIMING_SAMPLES ? runtime->timing_samples
                                                                : APP_TIMING_SAMPLES;
    out->timing_samples = sample_count;
    out->infer_p95_ms = percentile95(runtime->infer_samples, sample_count);
    out->transaction_p95_ms = percentile95(runtime->transaction_samples, sample_count);
}

/* ---- control socket 状态回调 ---- */

typedef struct {
    app_runtime_metrics_t *metrics;
    app_control_ctx_t     *ctrl;
    long                   t0;
} app_status_ctx_t;

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

static void ctrl_status_fn(void *opaque, char *buf, size_t buflen)
{
    app_status_ctx_t  *sctx = (app_status_ctx_t *)opaque;
    long               elapsed = now_ms() - sctx->t0;
    pipeline_metrics_t snap;

    metrics_snapshot(sctx->metrics, elapsed, &snap);
    /* 产出不含最外层花括号的成员列表（pipeline/processing/outputs），
     * 由 app_control 的 handle_client 拼进 {"id":N,"ok":true,<此处>}。
     * 键名与 web 控制台契约一致；额外字段（lut_updates 等）放进 pipeline 对象供调试，
     * JSON 消费方忽略未知字段。 */
    snprintf(buf, buflen,
             "\"pipeline\":{"
             "\"state\":\"%s\","
             "\"capture_mode\":\"%s\","
             "\"target_fps\":%u,"
             "\"display_fps\":%.2f,"
             "\"stream_drops\":%llu,"
             "\"timeouts\":%llu,"
             "\"transient_errors\":%llu,"
             "\"fatal_errors\":%llu,"
             "\"lut_updates\":%llu,"
             "\"lut_failures\":%llu"
             "},"
             "\"processing\":{"
             "\"nn_enabled\":%s,"
             "\"nn_degraded\":%s,"
             "\"enhancement_enabled\":%s,"
             "\"tone_enabled\":%s,"
             "\"tone_strength\":%.2f,"
             "\"high_clip_guard\":%.1f,"
             "\"drc_mode\":%d,"
             "\"ldci_mode\":%d,"
             "\"infer_p95_ms\":%.2f,"
             "\"transaction_p95_ms\":%.2f"
             "},"
             "\"outputs\":{"
             "\"hdmi\":%s,"
             "\"rtsp\":%s"
             "}",
             pipeline_state_name(snap.state),
             "linear", /* TODO: capture_mode 待从运行配置透传（沿用 PR#8 占位） */
             snap.display_fps > 0 ? 30u : 0u,
             snap.display_fps,
             (unsigned long long)snap.stream_drops,
             (unsigned long long)snap.frame_timeouts,
             (unsigned long long)snap.transient_errors,
             (unsigned long long)snap.fatal_errors,
             (unsigned long long)snap.lut_updates,
             (unsigned long long)snap.lut_failures,
             sctx->ctrl->nn_enabled ? "true" : "false",
             snap.degraded_events > 0 ? "true" : "false",
             sctx->ctrl->enhancement_enabled ? "true" : "false",
             sctx->ctrl->tone_enabled ? "true" : "false",
             (double)sctx->ctrl->strength,
             (double)sctx->ctrl->high_clip_guard,
             sctx->ctrl->drc_mode,
             sctx->ctrl->ldci_mode,
             snap.infer_p95_ms,
             snap.transaction_p95_ms,
             snap.display_frames > 0 ? "true" : "false",
             snap.stream_frames > 0 ? "true" : "false");
}

static void on_sig(int s) {
    (void) s;
    g_stop = 1;
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
    app_runtime_metrics_t *metrics = ctx->metrics;
    luma_stats_t raw_stats, cur;
    control_feedback_t feedback;
    static float raw_lut[PIPELINE_COTF_LUT_FLOAT_COUNT];
    static uint32_t packed_lut[PIPELINE_ISP_CLUT_NODE_COUNT];
    static uint32_t previous_lut[PIPELINE_ISP_CLUT_NODE_COUNT];
    int have_previous_lut = 0;
    int params_dirty = 0;  /* 手动改动强制立即刷新；NN 重开无帧时跨周期保留 */
    int nn_retry = 0;
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
        float effective_strength;
        int   effective_nn;

        /* 从控制 socket / 触摸菜单消费最新参数（last-write-wins）；
         * drain 返回 1 即"本周期有手动改动" → 置 params_dirty 强制立即刷新。 */
        {
            app_ctrl_params_t p;
            if (app_ctrl_drain(&p)) {
                if (p.has_tone_strength) {
                    ctx->strength      = p.tone_strength;
                    bridge_cfg.strength = p.tone_strength;
                }
                if (p.has_nn_clut_enabled)     ctx->nn_enabled          = p.nn_clut_enabled;
                if (p.has_high_clip_guard)     ctx->high_clip_guard     = p.high_clip_guard;
                if (p.has_enhancement_enabled) ctx->enhancement_enabled = p.enhancement_enabled;
                if (p.has_tone_enabled)        ctx->tone_enabled        = p.tone_enabled;
                if (p.has_drc_mode)            ctx->drc_mode            = p.drc_mode;
                if (p.has_drc_strength)        ctx->drc_strength        = p.drc_strength;
                if (p.has_ldci_mode)           ctx->ldci_mode           = p.ldci_mode;
                params_dirty = 1;
                LOG_INFO("[ctrl] params applied: strength=%.3f nn=%d guard=%.2f enh=%d tone=%d "
                         "drc=%d ldci=%d",
                         ctx->strength, ctx->nn_enabled, ctx->high_clip_guard,
                         ctx->enhancement_enabled, ctx->tone_enabled, ctx->drc_mode,
                         ctx->ldci_mode);
            }
        }

        usleep(APP_CTRL_PERIOD_MS * 1000);
        atomic_fetch_add(&metrics->control_polls, 1);
        if (isp_get_luma_stats(&raw_stats) != 0) {
            atomic_fetch_add(&metrics->transient_errors, 1);
            continue;
        }
        /* 手动改动或反馈环判定变化都触发刷新；稳定场景下手动滑块亦立即生效。 */
        refresh = params_dirty || control_feedback_observe(&feedback, &raw_stats, &cur);
        if (!refresh) {
            continue;
        }

        /* 全局增强总开关与色调开关：关闭时强制旁路对应处理。 */
        effective_strength = ctx->strength;
        effective_nn       = ctx->nn_enabled;
        if (!ctx->enhancement_enabled) { effective_strength = 0.0f; effective_nn = 0; }
        if (!ctx->tone_enabled)        { effective_strength = 0.0f; }

        mode = control_decide(&cur);
        /* 用户手动给了非零强度但自动判为 BYPASS 时，提升为 BRIGHTEN，
         * 否则正常光照下 tone_strength 滑块无可见效果。 */
        if (mode == EXPO_MODE_BYPASS && effective_strength > 0.0f) {
            mode = EXPO_MODE_BRIGHTEN;
        }

        /* NN 刚被重开但还没有可用 LUT 时，保留 params_dirty 重试，避免丢失"打开 NN"的意图。 */
        if (params_dirty && effective_nn && !have_previous_lut && nn_retry < 10) {
            nn_retry++;
        } else {
            params_dirty = 0;
            nn_retry = 0;
        }

        /* Gamma/DRC/CLUT 按用途分流：当前模型只产 RGB 3D-LUT；曝光色调由规则 Gamma 负责。
         * DRC/LDCI 保持 ISP 自动配置，避免用 RGB LUT 冒充动态范围局部参数。 */
        if (isp_gamma_apply_tone(mode_to_tone(mode), effective_strength) != 0) {
            atomic_fetch_add(&metrics->transient_errors, 1);
            LOG_WARN("[ctrl] gamma update rejected; keeping previous ISP parameters");
            continue;
        }

        /* 用户关闭 NN（或增强总开关）时清除上次残留的 CLUT，否则画面保持上次 AI 色彩。 */
        if (!effective_nn && have_previous_lut) {
            (void)isp_set_clut(ISP_BLOCK_OFF, APP_CLUT_UNITY_GAIN, APP_CLUT_UNITY_GAIN,
                               APP_CLUT_UNITY_GAIN);
            have_previous_lut = 0;
            LOG_INFO("[ctrl] NN CLUT disabled by user; ISP CLUT -> identity");
        }

        /* 已明显过曝时旁路 RGB LUT（CoTF 路径），避免模型继续推高 pre-CLUT 已裁剪区域。
         * CTBG 路径不适用此门控：estimator 感知全图双向曝光，高光场景也需要更新系数。 */
        if (!g_ctbg_mode && effective_nn && !ctx->health.degraded &&
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

        if (effective_nn && !ctx->health.degraded) {
            /* ---- CTBG estimator 路径（v9 6ch） ---- */
            if (g_ctbg_mode) {
                ot_video_frame_info control_frame;
                ctbg_timing_t ctbg_t = {0};
                long transaction_start_us = now_us();
                int got_frame = 0;
                int estimator_ok = 0;

                atomic_fetch_add(&metrics->lut_requests, 1);
                if (vpss_get_frame(PIPELINE_VPSS_GRP, PIPELINE_VPSS_CHN_CONTROL, &control_frame,
                                   APP_CONTROL_TIMEOUT_MS) == 0) {
                    got_frame = 1;

                    /* CPU NV21 → NCHW fp16 RGB（256×144 极小） */
                    {
                        unsigned cw = PIPELINE_CONTROL_WIDTH, ch = PIPELINE_CONTROL_HEIGHT;
                        size_t ysz = (size_t)cw * ch;
                        /* mmap VPSS 物理帧到用户空间 */
                        td_void *y_virt  = ss_mpi_sys_mmap(
                            control_frame.video_frame.phys_addr[0], ysz);
                        td_void *uv_virt = ss_mpi_sys_mmap(
                            control_frame.video_frame.phys_addr[1], ysz / 2);
                        if (g_ctbg_nv21_low && g_ctbg_coeff_raw && y_virt && uv_virt) {
                            memcpy(g_ctbg_nv21_low, y_virt, ysz);
                            memcpy(g_ctbg_nv21_low + ysz, uv_virt, ysz / 2);
                            ss_mpi_sys_munmap(y_virt, ysz);
                            ss_mpi_sys_munmap(uv_virt, ysz / 2);
                            /* NV21→RGB fp16，输出到 g_ctbg_coeff_raw（独立缓冲区） */
                            uint16_t *rgb_fp16 = g_ctbg_coeff_raw;
                            for (unsigned y = 0; y < ch; y++) {
                                for (unsigned x = 0; x < cw; x++) {
                                    int yi = y * cw + x;
                                    int uv_idx = (y / 2) * (cw / 2) * 2 + (x / 2) * 2;
                                    float Yv  = (float)g_ctbg_nv21_low[yi];
                                    float Uv  = (float)g_ctbg_nv21_low[ysz + uv_idx + 1] - 128.0f;
                                    float Vv  = (float)g_ctbg_nv21_low[ysz + uv_idx] - 128.0f;
                                    float Rf  = (Yv + 1.402f * Vv) / 255.0f;
                                    float Gf  = (Yv - 0.344f * Uv - 0.714f * Vv) / 255.0f;
                                    float Bf  = (Yv + 1.772f * Uv) / 255.0f;
                                    if (Rf < 0.0f) Rf = 0.0f; if (Rf > 1.0f) Rf = 1.0f;
                                    if (Gf < 0.0f) Gf = 0.0f; if (Gf > 1.0f) Gf = 1.0f;
                                    if (Bf < 0.0f) Bf = 0.0f; if (Bf > 1.0f) Bf = 1.0f;
                                    uint32_t ri; memcpy(&ri, &Rf, 4);
                                    uint32_t gi; memcpy(&gi, &Gf, 4);
                                    uint32_t bi; memcpy(&bi, &Bf, 4);
                                    rgb_fp16[0 * cw * ch + yi] = (uint16_t)(ri >> 16);
                                    rgb_fp16[1 * cw * ch + yi] = (uint16_t)(gi >> 16);
                                    rgb_fp16[2 * cw * ch + yi] = (uint16_t)(bi >> 16);
                                }
                            }
                            /* 运行 estimator */
                            if (ctbg_estimator_run(rgb_fp16, g_ctbg_coeff_raw,
                                                   &ctbg_t) == 0) {
                                estimator_ok = 1;
                                /* 预上采样 6ch raw 系数 → 全分辨率（v9 apply OM 无 ConvTranspose） */
                                ctbg_upsample_nearest(g_ctbg_coeff_raw, 6,
                                                      PIPELINE_CONTROL_WIDTH,
                                                      PIPELINE_CONTROL_HEIGHT,
                                                      PIPELINE_STREAM_WIDTH,
                                                      PIPELINE_STREAM_HEIGHT,
                                                      g_ctbg_coeff);
                                ctbg_upsample_flush(g_ctbg_coeff, g_ctbg_coeff_sz);
                                pthread_mutex_lock(&g_ctbg_coeff_mutex);
                                g_ctbg_coeff_ready = 1;
                                pthread_mutex_unlock(&g_ctbg_coeff_mutex);
                                atomic_store(&g_ctbg_scene_changed, 1);
                            }
                        }
                    }
                    if (vpss_release_frame(PIPELINE_VPSS_GRP, PIPELINE_VPSS_CHN_CONTROL,
                                           &control_frame) != 0) {
                        LOG_ERR("[ctrl] failed to release chn2 frame");
                        atomic_fetch_add(&metrics->transient_errors, 1);
                    }
                    got_frame = 0;
                } else {
                    atomic_fetch_add(&metrics->frame_timeouts, 1);
                }
                if (got_frame) {
                    (void)vpss_release_frame(PIPELINE_VPSS_GRP, PIPELINE_VPSS_CHN_CONTROL,
                                             &control_frame);
                }

                if (estimator_ok) {
                    float transaction_ms = (now_us() - transaction_start_us) / 1000.0f;
                    unsigned sample_index = metrics->timing_samples % APP_TIMING_SAMPLES;

                    (void)control_health_record(&ctx->health, 1, APP_NN_FAILURE_LIMIT);
                    atomic_fetch_add(&metrics->lut_updates, 1);
                    atomic_fetch_add(&metrics->infer_runs, 1);
                    metrics->infer_last_ms = ctbg_t.est_ms;
                    if (ctbg_t.est_ms > metrics->infer_max_ms) {
                        metrics->infer_max_ms = ctbg_t.est_ms;
                    }
                    metrics->transaction_last_ms = transaction_ms;
                    if (transaction_ms > metrics->transaction_max_ms) {
                        metrics->transaction_max_ms = transaction_ms;
                    }
                    metrics->infer_samples[sample_index] = ctbg_t.est_ms;
                    metrics->transaction_samples[sample_index] = transaction_ms;
                    metrics->timing_samples++;
                    control_feedback_commit(&feedback);
                    LOG_INFO("[ctrl] CTBG est v%llu -> %s luma=%.1f exec=%.2fms tx=%.2fms",
                             (unsigned long long)atomic_load(&metrics->lut_updates),
                             mode_name(mode), cur.mean_luma, ctbg_t.est_ms, transaction_ms);
                    continue;
                }

                /* estimator 失败：不降级（NPU 瞬态错误可恢复），保留上次系数继续 */
                atomic_fetch_add(&metrics->lut_failures, 1);
                atomic_fetch_add(&metrics->transient_errors, 1);
                LOG_WARN("[ctrl] CTBG estimator failed; keeping previous coefficients");
                continue;
            }

            /* ---- CoTF param-net + CLUT bridge 路径（原有） ---- */
            {
            ot_video_frame_info control_frame;
            infer_timing_t timing = {0};
            long transaction_start_us = now_us();
            int got_frame = 0;
            int infer_rc = PIPELINE_ERR_IO;
            int transaction_ok = 0;

            atomic_fetch_add(&metrics->lut_requests, 1);
            if (vpss_get_frame(PIPELINE_VPSS_GRP, PIPELINE_VPSS_CHN_CONTROL, &control_frame,
                               APP_CONTROL_TIMEOUT_MS) == 0) {
                got_frame = 1;
                infer_rc = infer_run_nv21(&control_frame, raw_lut,
                                          PIPELINE_COTF_LUT_FLOAT_COUNT, &timing);
                atomic_fetch_add(&metrics->infer_runs, 1);
                metrics->infer_last_ms = timing.execute_ms;
                if (timing.execute_ms > metrics->infer_max_ms) {
                    metrics->infer_max_ms = timing.execute_ms;
                }
                metrics->infer_total_last_ms = timing.total_ms;
                if (timing.total_ms > metrics->infer_total_max_ms) {
                    metrics->infer_total_max_ms = timing.total_ms;
                }
                if (vpss_release_frame(PIPELINE_VPSS_GRP, PIPELINE_VPSS_CHN_CONTROL,
                                       &control_frame) != 0) {
                    LOG_ERR("[ctrl] failed to release chn2 frame");
                    atomic_fetch_add(&metrics->transient_errors, 1);
                    infer_rc = PIPELINE_ERR_IO;
                }
                got_frame = 0;
            } else {
                atomic_fetch_add(&metrics->frame_timeouts, 1);
            }
            if (got_frame) {
                (void)vpss_release_frame(PIPELINE_VPSS_GRP, PIPELINE_VPSS_CHN_CONTROL,
                                         &control_frame);
            }
            if (infer_rc == PIPELINE_ERR_RUNTIME) {
                LOG_ERR("[ctrl] fatal ACL runtime failure; stopping pipeline for clean recovery");
                ctx->fatal_error = 1;
                atomic_fetch_add(&metrics->fatal_errors, 1);
                metrics_set_state(metrics, PIPELINE_STATE_FAILED);
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
                unsigned sample_index = metrics->timing_samples % APP_TIMING_SAMPLES;

                (void)control_health_record(&ctx->health, 1, APP_NN_FAILURE_LIMIT);
                atomic_fetch_add(&metrics->lut_updates, 1);
                metrics->transaction_last_ms = transaction_ms;
                if (transaction_ms > metrics->transaction_max_ms) {
                    metrics->transaction_max_ms = transaction_ms;
                }
                metrics->infer_samples[sample_index] = timing.execute_ms;
                metrics->transaction_samples[sample_index] = transaction_ms;
                metrics->timing_samples++;
                memcpy(previous_lut, packed_lut, sizeof(previous_lut));
                have_previous_lut = 1;
                control_feedback_commit(&feedback);
                LOG_INFO("[ctrl] NN+CLUT v%llu -> %s luma=%.1f low=%.1f%% high=%.1f%% "
                         "exec=%.2fms infer_total=%.2fms transaction=%.2fms",
                         (unsigned long long)atomic_load(&metrics->lut_updates), mode_name(mode),
                         cur.mean_luma,
                         cur.clip_low_pct, cur.clip_high_pct, timing.execute_ms, timing.total_ms,
                         transaction_ms);
                continue;
            }

            atomic_fetch_add(&metrics->lut_failures, 1);
            atomic_fetch_add(&metrics->transient_errors, 1);
            if (control_health_record(&ctx->health, 0, APP_NN_FAILURE_LIMIT)) {
                (void)isp_set_clut(ISP_BLOCK_OFF, APP_CLUT_UNITY_GAIN, APP_CLUT_UNITY_GAIN,
                                   APP_CLUT_UNITY_GAIN);
                atomic_fetch_add(&metrics->degraded_events, 1);
                metrics_set_state(metrics, PIPELINE_STATE_DEGRADED);
                LOG_ERR("[ctrl] NN control degraded after %u consecutive failures; "
                        "fallback=rule Gamma",
                        ctx->health.consecutive_failures);
                control_feedback_commit(&feedback);
            } else {
                LOG_WARN("[ctrl] NN refresh failed (%u/%u); keeping previous CLUT",
                         ctx->health.consecutive_failures, APP_NN_FAILURE_LIMIT);
            }
            continue;
            } /* end CoTF path */
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

/* 合并 YUV 贡献 LUT：单个 fp16 值 → R/G/B 三通道全部 Y/U/V 贡献，一次查表 9 字节。
 * 12-bit 量化 → 4096 项 × 9B = 36KB，装入 ARM A55 L1 cache（32-64KB）。
 * 索引：(fp16_value + 8) >> 4（round-to-nearest），量化误差 <0.02% for 8-bit YUV。 */
struct f16_all_yuv { uint8_t yr,yg,yb; int8_t ur,ug,ub,vr,vg,vb; };
static struct f16_all_yuv g_yuv_lut[4096];
static int g_yuv_lut_init = 0;

/* 8-bit→fp16 LUT：256 项 uint16_t，用于 NV21→RGB 快速转换输出 */
static uint16_t g_r8_to_f16[256];
static int      g_r8_f16_init = 0;

static void ctbg_init_r8_f16(void) {
    if (g_r8_f16_init) return;
    for (int i = 0; i < 256; i++) {
        float v = i / 255.0f;
        /* float→fp16（仅处理 [0,1] 范围，简化编码） */
        uint32_t bits; memcpy(&bits, &v, 4);
        int sign = (bits >> 31) & 1, exp = (bits >> 23) & 0xFF, mant = bits & 0x7FFFFF;
        uint16_t h;
        if (exp == 0) { h = 0; }
        else if (exp < 113) { h = 0; }  /* underflow */
        else if (exp > 142) { h = (uint16_t)((sign << 15) | 0x7C00); }  /* saturate */
        else { h = (uint16_t)((sign << 15) | ((exp - 112) << 10) | (mant >> 13)); }
        g_r8_to_f16[i] = h;
    }
    g_r8_f16_init = 1;
}

/* 快速 NV21→RGB fp16 NCHW 转换（整数 BT.601 + fp16 LUT）。
 * nv21: YUV420SP 输入, rgb_fp16: NCHW fp16 输出, w/h: 尺寸 */
static void ctbg_nv21_to_rgb_fp16(const uint8_t *nv21, uint16_t *rgb_fp16, int w, int h) {
    int cs = w * h, y;
    for (y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            int Y = nv21[i];
            int uv_idx = cs + (y/2)*(w/2)*2 + (x/2)*2;
            int U = nv21[uv_idx + 1] - 128;
            int V = nv21[uv_idx] - 128;
            /* BT.601 full-range 整数近似（×256 定点）*/
            int R = (Y * 256 + 359 * V + 128) >> 8;
            int G = (Y * 256 - 88 * U - 183 * V + 128) >> 8;
            int B = (Y * 256 + 454 * U + 128) >> 8;
            if (R < 0) R = 0; if (R > 255) R = 255;
            if (G < 0) G = 0; if (G > 255) G = 255;
            if (B < 0) B = 0; if (B > 255) B = 255;
            rgb_fp16[i]        = g_r8_to_f16[R];
            rgb_fp16[cs + i]   = g_r8_to_f16[G];
            rgb_fp16[2*cs + i] = g_r8_to_f16[B];
        }
    }
}

static void ctbg_init_yuv_lut(void) {
    if (g_yuv_lut_init) return;
    for (int i = 0; i < 65536; i++) {
        uint32_t s = (i & 0x8000u) << 16, e = (i >> 10) & 0x1Fu, m = i & 0x3FFu, b;
        if (e == 0) {
            if (m == 0) b = s;
            else { e = 1; while ((m & 0x400) == 0) { m <<= 1; e--; } m &= 0x3FF;
                   b = s | ((e + 127 - 15) << 23) | (m << 13); }
        } else if (e == 0x1F) b = s | 0x7F800000u | (m << 13);
        else b = s | ((e - 15 + 127) << 23) | (m << 13);
        float v; memcpy(&v, &b, 4);
        if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
        float sv = v * 255.0f;
        int idx = (i + 8) >> 4; if (idx >= 4096) idx = 4095;
        g_yuv_lut[idx].yr = (uint8_t)(0.299f*sv+0.5f);
        g_yuv_lut[idx].yg = (uint8_t)(0.587f*sv+0.5f);
        g_yuv_lut[idx].yb = (uint8_t)(0.114f*sv+0.5f);
        g_yuv_lut[idx].ur = (int8_t)(-0.169f*sv);
        g_yuv_lut[idx].ug = (int8_t)(-0.331f*sv);
        g_yuv_lut[idx].ub = (int8_t)(0.500f*sv);
        g_yuv_lut[idx].vr = (int8_t)(0.500f*sv);
        g_yuv_lut[idx].vg = (int8_t)(-0.419f*sv);
        g_yuv_lut[idx].vb = (int8_t)(-0.081f*sv);
    }
    g_yuv_lut_init = 1;
}

static void* stream_worker(void* arg) {
    app_runtime_metrics_t *metrics = (app_runtime_metrics_t *)arg;
    ot_video_frame_info frame;
    int w = PIPELINE_STREAM_WIDTH, h = PIPELINE_STREAM_HEIGHT;

    /* CTBG apply 缓冲区：v8 AIPP OM 接收 NV21（硬件 AIPP）, 输出 RGB fp16 */
    uint16_t *ctbg_rgb = NULL;
    uint8_t  *ctbg_nv  = NULL;
    uint16_t *local_coeff = NULL;
    if (g_ctbg_mode) {
        ctbg_rgb = (uint16_t *)malloc((size_t)w * h * 3 * 2);
        ctbg_nv  = (uint8_t *)malloc((size_t)w * h * 3 / 2);
        local_coeff = (uint16_t *)malloc(g_ctbg_coeff_sz);
        if (!ctbg_rgb || !ctbg_nv || !local_coeff) {
            LOG_ERR("CTBG stream_worker: oom");
            free(ctbg_rgb); free(ctbg_nv); free(local_coeff);
            return TD_NULL;
        }
    }

    int apply_count = 0;
    #define CTBG_WRITEBACK_COOLDOWN 45  /* 写回冷却帧数（~1.5s），避免连续写回阻塞管线 */
    int wb_cooldown = 0;
    while (!g_stop) {
        if (vpss_get_frame(PIPELINE_VPSS_GRP, PIPELINE_VPSS_CHN_STREAM, &frame, 200) != 0) {
            atomic_fetch_add(&metrics->stream_drops, 1);
            atomic_fetch_add(&metrics->frame_timeouts, 1);
            continue;
        }
        if (wb_cooldown > 0) wb_cooldown--;

        /* 取系数 */
        int local_ready = 0;
        if (g_ctbg_mode && ctbg_rgb && ctbg_nv && local_coeff) {
            pthread_mutex_lock(&g_ctbg_coeff_mutex);
            if (g_ctbg_coeff_ready) {
                memcpy(local_coeff, g_ctbg_coeff, g_ctbg_coeff_sz);
                local_ready = 1;
            }
            pthread_mutex_unlock(&g_ctbg_coeff_mutex);
        }

        /* 场景变化触发写回（需 --ctbg-writeback 启用） */
        int scene_trig = (g_ctbg_mode && local_ready &&
                          atomic_exchange(&g_ctbg_scene_changed, 0) == 1);
        int do_writeback = (g_ctbg_writeback && scene_trig &&
                            wb_cooldown == 0 && apply_count < 20);
        size_t ysz = (size_t)w * h, uvsz = ysz / 2;

        if (do_writeback) {
            /* 同步写回路径：持有帧 → apply → 全 NV21 转换 → mmap 写回 → 归还。
             * 耗时 ~103ms，影响本帧延迟但下一帧恢复。间隔 30 帧 → 均摊 <1fps 影响。 */
            td_void *yv = ss_mpi_sys_mmap(frame.video_frame.phys_addr[0], ysz);
            td_void *uv = ss_mpi_sys_mmap(frame.video_frame.phys_addr[1], uvsz);
            if (yv && uv) {
                memcpy(ctbg_nv, yv, ysz);
                memcpy(ctbg_nv + ysz, uv, uvsz);
                ss_mpi_sys_munmap(yv, ysz);
                ss_mpi_sys_munmap(uv, uvsz);

                /* v9 OM：需 CPU NV21→RGB fp16 转换（~10ms），OM 纯 elementwise 20.5ms */
                ctbg_nv21_to_rgb_fp16(ctbg_nv, ctbg_rgb, w, h);
                ctbg_timing_t t;
                long t0 = now_us();
                if (ctbg_apply_run(ctbg_rgb, local_coeff, ctbg_rgb, &t) == 0) {
                    atomic_fetch_add(&metrics->infer_runs, 1);
                    apply_count++;

                    /* 全 NV21 转换（Y 每像素 + UV 仅 1/4 像素） */
                    uint8_t *yp = ctbg_nv, *vp = ctbg_nv + ysz;
                    int cs = w * h;
                    for (int y = 0; y < h; y++) {
                        int row = y * w;
                        for (int x = 0; x < w; x++) {
                            int i = row + x;
                            struct f16_all_yuv lr = g_yuv_lut[(ctbg_rgb[i]+8)>>4];
                            struct f16_all_yuv lg = g_yuv_lut[(ctbg_rgb[cs+i]+8)>>4];
                            struct f16_all_yuv lb = g_yuv_lut[(ctbg_rgb[2*cs+i]+8)>>4];
                            int Y = (int)lr.yr + (int)lg.yg + (int)lb.yb;
                            yp[i] = (uint8_t)(Y > 255 ? 255 : Y);
                            if ((y & 1) == 0 && (x & 1) == 0) {
                                int vi = (y/2)*(w/2)*2 + (x/2)*2;
                                int U = 128+(int)lr.ur+(int)lg.ug+(int)lb.ub;
                                int V = 128+(int)lr.vr+(int)lg.vg+(int)lb.vb;
                                if(U<0)U=0; if(U>255)U=255;
                                if(V<0)V=0; if(V>255)V=255;
                                vp[vi]=(uint8_t)V; vp[vi+1]=(uint8_t)U;
                            }
                        }
                    }
                    /* mmap 独立映射写回 */
                    td_void *wy = ss_mpi_sys_mmap(frame.video_frame.phys_addr[0], ysz);
                    td_void *wuv= ss_mpi_sys_mmap(frame.video_frame.phys_addr[1], uvsz);
                    if (wy && wuv) {
                        memcpy(wy, ctbg_nv, ysz);
                        memcpy(wuv, ctbg_nv + ysz, uvsz);
                        ss_mpi_sys_munmap(wy, ysz);
                        ss_mpi_sys_munmap(wuv, uvsz);
                    }
                    metrics->infer_last_ms = t.app_ms;
                    LOG_INFO("[stream] CTBG writeback #%d: app=%.1fms total=%.1fms",
                             apply_count, t.app_ms, (now_us() - t0) / 1000.0f);
                    wb_cooldown = CTBG_WRITEBACK_COOLDOWN;
                }
            }
        }  /* end writeback path */
        /* 异步诊断：每 15 帧一次（~0.5s），避免 NPU 饱和影响 estimator */
        #define DIAG_INTERVAL 15
        static int diag_seq = 0;
        if (!do_writeback && g_ctbg_mode && local_ready &&
            apply_count < 30 && (++diag_seq % DIAG_INTERVAL) == 0) {
            td_void *yv = ss_mpi_sys_mmap(frame.video_frame.phys_addr[0], ysz);
            td_void *uv = ss_mpi_sys_mmap(frame.video_frame.phys_addr[1], uvsz);
            if (yv && uv) {
                memcpy(ctbg_nv, yv, ysz);
                memcpy(ctbg_nv + ysz, uv, uvsz);
                ss_mpi_sys_munmap(yv, ysz);
                ss_mpi_sys_munmap(uv, uvsz);
            }
        }

        if (stream_send_frame(&frame, 0) == 0) {
            atomic_fetch_add(&metrics->stream_frames, 1);
        } else {
            atomic_fetch_add(&metrics->stream_drops, 1);
            atomic_fetch_add(&metrics->transient_errors, 1);
        }
        if (vpss_release_frame(PIPELINE_VPSS_GRP, PIPELINE_VPSS_CHN_STREAM, &frame) != 0) {
            atomic_fetch_add(&metrics->stream_drops, 1);
            atomic_fetch_add(&metrics->transient_errors, 1);
        }

        /* 异步 apply（仅非写回帧，帧已归还，上限 5 次/运行避免 NPU/CPU 饱和） */
        #define MAX_DIAG_APPLY 5
        static int diag_apply_done = 0;
        if (!do_writeback && g_ctbg_mode && local_ready &&
            ctbg_nv[0] != 0 && diag_apply_done < MAX_DIAG_APPLY) {
            ctbg_nv[0] = 0;
            diag_apply_done++;
            ctbg_nv21_to_rgb_fp16(ctbg_nv, ctbg_rgb, w, h);
            ctbg_timing_t t;
            long t0 = now_us();
            if (ctbg_apply_run(ctbg_rgb, local_coeff, ctbg_rgb, &t) == 0) {
                atomic_fetch_add(&metrics->infer_runs, 1);
                int cs = w * h;
                for (int i = 0; i < cs; i++) {
                    ctbg_nv[i] = (uint8_t)((int)g_yuv_lut[(ctbg_rgb[i]+8)>>4].yr
                        + (int)g_yuv_lut[(ctbg_rgb[cs+i]+8)>>4].yg
                        + (int)g_yuv_lut[(ctbg_rgb[2*cs+i]+8)>>4].yb);
                }
                LOG_INFO("[stream] CTBG diag #%d: app=%.1fms yuv=%ldms",
                         apply_count, t.app_ms, (now_us() - t0) / 1000);
            }
        }
    }
    #undef CTBG_WRITEBACK_COOLDOWN
    #undef DIAG_INTERVAL
    #undef MAX_DIAG_APPLY

    free(ctbg_rgb); free(ctbg_nv); free(local_coeff);
    return TD_NULL;
}

int main(int argc, char** argv) {
    pipeline_config_t cfg;
    capture_cfg_t cap_cfg;
    stream_cfg_t stream_cfg;
    int enable_nn = 1;
    int rc = 1;
    int sys_up = 0, cap_up = 0, vpss_up = 0, bound = 0, disp_up = 0, stream_up = 0;
    int infer_up = 0;
    int ctrl_up = 0, stream_thread_up = 0, app_ctrl_up = 0;
    unsigned in_w = 0, in_h = 0;
    long t0 = 0, t_last_log;
    long t_last_menu = 0;  /* 菜单送显节流时间戳 */
    long run_elapsed_ms = 0;
    pthread_t ctrl_tid, stream_tid, app_ctrl_tid;
    app_control_ctx_t control_ctx = {0};
    app_runtime_metrics_t runtime_metrics = {0};
    pipeline_metrics_t metrics = {0};
    static app_status_ctx_t status_ctx;
    ot_vb_cfg vb_cfg = {0};
    ot_pic_buf_attr buf_attr = {0};
    ot_vb_calc_cfg calc_cfg = {0};
    ot_video_frame_info frame;
    /* Touch menu */
    ot_pic_buf_attr     menu_buf_attr  = {0};
    ot_vb_calc_cfg      menu_calc_cfg  = {0};
    /* 双缓冲：VO 显示其中一块时渲染另一块，避免边显示边重写造成撕裂（RESET 行附近的晃动黑条） */
    ot_vb_blk           menu_blk[2]    = {OT_VB_INVALID_HANDLE, OT_VB_INVALID_HANDLE};
    ot_video_frame_info menu_fi[2]     = {{0}, {0}};
    td_u8              *menu_virt[2]   = {TD_NULL, TD_NULL};
    int                 menu_buf_idx   = 0;
    int touch_fd    = -1;
    int menu_active = 0;
    int prev_pressed = 0;
    vpss_chn_cfg_t chn_cfg[VPSS_CHN_COUNT] = {
        {1, DISPLAY_WIDTH, DISPLAY_HEIGHT, 2}, /* chn0：显示底图 */
        {0, PIPELINE_STREAM_WIDTH, PIPELINE_STREAM_HEIGHT, 2},
        {0, 0, 0, 0},
    };

    setbuf(stdout, NULL);  /* 禁用缓冲，确保崩溃前日志不丢失 */
    setbuf(stderr, NULL);
    pipeline_config_defaults(&cfg);
    cfg.nn_high_clip_guard = APP_NN_HIGH_CLIP_GUARD;
    metrics_set_state(&runtime_metrics, PIPELINE_STATE_STARTING);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sensor") == 0 && i + 1 < argc) {
            cfg.sensor_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--wdr") == 0) {
            cfg.use_wdr = 1;
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            cfg.target_fps = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--strength") == 0 && i + 1 < argc) {
            cfg.tone_strength = (float) atof(argv[++i]);
        } else if (strcmp(argv[i], "--stream") == 0) {
            cfg.enable_stream = 1;
        } else if (strcmp(argv[i], "--no-display") == 0) {
            cfg.enable_display = 0;
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            cfg.stream_bitrate_kbps = (unsigned) strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--rtsp-port") == 0 && i + 1 < argc) {
            cfg.rtsp_port = (unsigned) strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--rtsp-bind") == 0 && i + 1 < argc) {
            cfg.rtsp_bind_addr = argv[++i];
        } else if (strcmp(argv[i], "--ctrl-sock") == 0 && i + 1 < argc) {
            cfg.ctrl_sock_path = argv[++i];
        } else if (strcmp(argv[i], "--stream-path") == 0 && i + 1 < argc) {
            cfg.stream_path = argv[++i];
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            cfg.model_path = argv[++i];
        } else if (strcmp(argv[i], "--no-nn") == 0) {
            enable_nn = 0;
        } else if (strcmp(argv[i], "--nn-high-clip") == 0 && i + 1 < argc) {
            cfg.nn_high_clip_guard = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--ctbg") == 0) {
            g_ctbg_mode = 1;
        } else if (strcmp(argv[i], "--ctbg-writeback") == 0) {
            g_ctbg_writeback = 1;
        } else if (strcmp(argv[i], "--ctbg-est-om") == 0 && i + 1 < argc) {
            cfg.ctbg_est_om_path = argv[++i];
        } else if (strcmp(argv[i], "--ctbg-app-om") == 0 && i + 1 < argc) {
            cfg.ctbg_app_om_path = argv[++i];
        } else {
            LOG_ERR("unknown or incomplete option: %s", argv[i]);
            return 2;
        }
    }
    if (cfg.model_path == NULL && !g_ctbg_mode) {
        enable_nn = 0;
    }
    if (pipeline_config_validate(&cfg) != PIPELINE_OK) {
        LOG_ERR("invalid pipeline configuration");
        return 2;
    }
    cap_cfg.sensor_index = cfg.sensor_index;
    cap_cfg.mode = cfg.use_wdr ? CAPTURE_MODE_WDR_2TO1 : CAPTURE_MODE_LINEAR;
    stream_cfg = (stream_cfg_t){
        .width            = PIPELINE_STREAM_WIDTH,
        .height           = PIPELINE_STREAM_HEIGHT,
        .fps              = cfg.target_fps,
        .bitrate_kbps     = cfg.stream_bitrate_kbps,
        .rtsp_port        = cfg.rtsp_port,
        .stream_path      = cfg.stream_path,
        .rtsp_bind_addr   = cfg.rtsp_bind_addr,
    };
    chn_cfg[PIPELINE_VPSS_CHN_DISPLAY].enable = cfg.enable_display;
    chn_cfg[PIPELINE_VPSS_CHN_STREAM].enable = cfg.enable_stream;
    chn_cfg[PIPELINE_VPSS_CHN_CONTROL] =
        (vpss_chn_cfg_t){enable_nn, PIPELINE_CONTROL_WIDTH, PIPELINE_CONTROL_HEIGHT, 1};
    control_ctx.strength = cfg.tone_strength;
    control_ctx.high_clip_guard = cfg.nn_high_clip_guard;
    control_ctx.nn_enabled = enable_nn;
    control_ctx.enhancement_enabled = 1;  /* 默认全局增强开 */
    control_ctx.tone_enabled = 1;         /* 默认色调开 */
    control_ctx.drc_mode = 1;             /* auto */
    control_ctx.drc_strength = 512;
    control_ctx.ldci_mode = 1;            /* auto */
    control_ctx.metrics = &runtime_metrics;
    control_health_init(&control_ctx.health);

    LOG_INFO("%s v%s — %s realtime exposure correction (sensor=%d mode=%s fps=%u "
             "display=%s stream=%s nn=%s)",
             SOCCHINA_APP_NAME, SOCCHINA_APP_VERSION,
             g_ctbg_mode ? "CTBG v9 6ch" : "CoTF",
             cfg.sensor_index,
             cfg.use_wdr ? "wdr2to1" : "linear", cfg.target_fps,
             cfg.enable_display ? "on" : "off", cfg.enable_stream ? "on" : "off",
             g_ctbg_mode ? "ctbg-estimator" : (enable_nn ? cfg.model_path : "rule-gamma"));

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
    /* VB 池：CTBG 模式固定 12 块（系统上限约 12），避免 display+stream+NN 超限 */
    if (g_ctbg_mode) {
        vb_cfg.common_pool[0].blk_cnt = 12;
    } else {
        vb_cfg.common_pool[0].blk_cnt = 10 + (cfg.enable_stream ? 2 : 0) + (enable_nn ? 2 : 0);
    }
    CHECK_RET_GOTO(sample_comm_sys_init_with_vb_supplement(&vb_cfg, OT_VB_SUPPLEMENT_BNR_MOT_MASK), cleanup);
    sys_up = 1;

    /* Allocate two persistent VB blocks for the touch menu overlay (double buffer) */
    if (cfg.enable_display) {
        menu_buf_attr.width        = DISPLAY_WIDTH;
        menu_buf_attr.height       = DISPLAY_HEIGHT;
        menu_buf_attr.align        = OT_DEFAULT_ALIGN;
        menu_buf_attr.bit_width    = OT_DATA_BIT_WIDTH_8;
        menu_buf_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
        menu_buf_attr.compress_mode = OT_COMPRESS_MODE_NONE;
        ot_common_get_pic_buf_cfg(&menu_buf_attr, &menu_calc_cfg);

        for (int i = 0; i < 2; i++) {
            td_phys_addr_t mphys;
            td_u8 *mvirt;
            ot_video_frame *mf;

            menu_blk[i] = ss_mpi_vb_get_blk(OT_VB_INVALID_POOL_ID, menu_calc_cfg.vb_size, TD_NULL);
            if (menu_blk[i] == OT_VB_INVALID_HANDLE) {
                LOG_WARN("[menu] no VB block for menu frame %d; touch menu disabled", i);
                break;
            }
            mphys = ss_mpi_vb_handle_to_phys_addr(menu_blk[i]);
            mvirt = (td_u8 *)ss_mpi_sys_mmap(mphys, menu_calc_cfg.vb_size);
            if (mphys == 0 || mvirt == TD_NULL) {
                LOG_WARN("[menu] VB mmap failed for frame %d; touch menu disabled", i);
                (void)ss_mpi_vb_release_blk(menu_blk[i]);
                menu_blk[i] = OT_VB_INVALID_HANDLE;
                break;
            }
            menu_virt[i] = mvirt;
            memset(mvirt, 0, menu_calc_cfg.vb_size);
            menu_fi[i].mod_id  = OT_ID_USER;
            menu_fi[i].pool_id = ss_mpi_vb_handle_to_pool_id(menu_blk[i]);
            mf = &menu_fi[i].video_frame;
            mf->width            = DISPLAY_WIDTH;
            mf->height           = DISPLAY_HEIGHT;
            mf->field            = OT_VIDEO_FIELD_FRAME;
            mf->pixel_format     = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
            mf->video_format     = OT_VIDEO_FORMAT_LINEAR;
            mf->compress_mode    = OT_COMPRESS_MODE_NONE;
            mf->dynamic_range    = OT_DYNAMIC_RANGE_SDR8;
            mf->color_gamut      = OT_COLOR_GAMUT_BT601;
            mf->header_stride[0] = menu_calc_cfg.head_stride;
            mf->header_stride[1] = menu_calc_cfg.head_stride;
            mf->header_phys_addr[0] = mphys;
            mf->header_phys_addr[1] = mphys + menu_calc_cfg.head_y_size;
            mf->header_virt_addr[0] = mvirt;
            mf->header_virt_addr[1] = mvirt + menu_calc_cfg.head_y_size;
            mf->stride[0]        = menu_calc_cfg.main_stride;
            mf->stride[1]        = menu_calc_cfg.main_stride;
            mf->phys_addr[0]     = mphys + menu_calc_cfg.head_size;
            mf->phys_addr[1]     = mf->phys_addr[0] + menu_calc_cfg.main_y_size;
            mf->virt_addr[0]     = mvirt + menu_calc_cfg.head_size;
            mf->virt_addr[1]     = mvirt + menu_calc_cfg.head_size + menu_calc_cfg.main_y_size;
        }
        /* 需要两块缓冲都就绪才启用双缓冲菜单；否则回收已分配的并禁用菜单 */
        if (menu_blk[0] == OT_VB_INVALID_HANDLE || menu_blk[1] == OT_VB_INVALID_HANDLE) {
            for (int i = 0; i < 2; i++) {
                if (menu_virt[i] != TD_NULL) {
                    (void)ss_mpi_sys_munmap((td_void *)menu_fi[i].video_frame.header_virt_addr[0],
                                            menu_calc_cfg.vb_size);
                    menu_virt[i] = TD_NULL;
                }
                if (menu_blk[i] != OT_VB_INVALID_HANDLE) {
                    (void)ss_mpi_vb_release_blk(menu_blk[i]);
                    menu_blk[i] = OT_VB_INVALID_HANDLE;
                }
            }
        } else {
            LOG_INFO("[menu] VB double-buffer ready: vb_size=%u main_stride=%u head_size=%u",
                     menu_calc_cfg.vb_size, menu_calc_cfg.main_stride, menu_calc_cfg.head_size);
        }
    }

    if (capture_init(&cap_cfg) != 0) {
        goto cleanup;
    }
    cap_up = 1;
    if (cfg.target_fps != PIPELINE_TARGET_FPS && capture_set_fps((float)cfg.target_fps) != 0) {
        goto cleanup;
    }
    if (vpss_init(0, in_w, in_h, chn_cfg) != 0) {
        goto cleanup;
    }
    vpss_up = 1;
    if (capture_bind_vpss(0) != 0) {
        goto cleanup;
    }
    bound = 1;
    if (cfg.enable_display) {
        if (display_init() != 0) {
            goto cleanup;
        }
        disp_up = 1;
        if (menu_blk[0] != OT_VB_INVALID_HANDLE) {
            touch_fd = touch_open("/dev/input/event0", DISPLAY_WIDTH, DISPLAY_HEIGHT);
            if (touch_fd < 0)
                LOG_WARN("[menu] touch open failed; touch menu disabled");
            else
                LOG_INFO("[menu] touch ready on /dev/input/event0 (corner=bottom-left to open)");
        }
    }
    if (cfg.enable_stream) {
        if (stream_init(&stream_cfg) != 0) {
            goto cleanup;
        }
        stream_up = 1;
    }
    if (enable_nn && !g_ctbg_mode) {
        infer_cfg_t infer_cfg = {
            cfg.model_path, 0, PIPELINE_CONTROL_WIDTH, PIPELINE_CONTROL_HEIGHT,
            PIPELINE_COTF_LUT_DIM, PIPELINE_INPUT_COPY,
        };
        if (infer_init(&infer_cfg) != 0) {
            LOG_ERR("NN init failed; continuing in PIPELINE_DEGRADED with rule Gamma");
            control_ctx.nn_enabled = 0;
            control_health_init(&control_ctx.health);
            control_ctx.health.degraded = 1;
            atomic_fetch_add(&runtime_metrics.degraded_events, 1);
        } else {
            infer_up = 1;
        }
    }

    /* CTBG 初始化（v9 6ch estimator + v9 6ch apply OM） */
    if (g_ctbg_mode) {
        ctbg_cfg_t ctbg_cfg = {
            .est_om_path = cfg.ctbg_est_om_path ? cfg.ctbg_est_om_path
                          : "/root/socchina-2026/ctbg6ch_estimator_256x144.om",
            .app_om_path = cfg.ctbg_app_om_path ? cfg.ctbg_app_om_path
                          : "/root/socchina-2026/ctbg6ch_apply_1024x576.om",
            .device_id = 0,
            .full_w = PIPELINE_STREAM_WIDTH, .full_h = PIPELINE_STREAM_HEIGHT,
            .low_w = PIPELINE_CONTROL_WIDTH, .low_h = PIPELINE_CONTROL_HEIGHT,
            .coeff_ch = 6,  /* v9 6ch，host 侧预上采样到全分辨率 */
        };
        if (ctbg_init(&ctbg_cfg) != 0) {
            LOG_ERR("CTBG init failed; falling back to rule Gamma");
            g_ctbg_mode = 0;
        } else {
            /* 分配系数缓冲区 */
            g_ctbg_coeff_raw_sz = ctbg_coeff_size();              /* raw：6×144×256×2 */
            g_ctbg_coeff_sz     = ctbg_coeff_app_size();          /* up：6×576×1024×2 */
            g_ctbg_coeff_raw = (uint16_t *)malloc(g_ctbg_coeff_raw_sz);
            g_ctbg_coeff     = (uint16_t *)malloc(g_ctbg_coeff_sz);
            g_ctbg_nv21_low  = (uint8_t *)malloc(
                (size_t)PIPELINE_CONTROL_WIDTH * PIPELINE_CONTROL_HEIGHT * 3 / 2);
            if (!g_ctbg_coeff_raw || !g_ctbg_coeff || !g_ctbg_nv21_low) {
                LOG_ERR("CTBG coeff buffer oom");
                free(g_ctbg_coeff_raw); free(g_ctbg_coeff); free(g_ctbg_nv21_low);
                g_ctbg_coeff_raw = NULL; g_ctbg_coeff = NULL; g_ctbg_nv21_low = NULL;
                g_ctbg_mode = 0;
                ctbg_deinit();
            } else {
                /* 初始化为恒等变换（raw=0 → a=1, b=0, g=1） */
                memset(g_ctbg_coeff_raw, 0, g_ctbg_coeff_raw_sz);
                memset(g_ctbg_coeff, 0, g_ctbg_coeff_sz);
                g_ctbg_coeff_ready = 0;  /* apply 在有系数前跳过 */
                infer_up = 1;
                ctbg_init_r8_f16();   /* 8-bit→fp16 LUT（512B） */
                ctbg_init_yuv_lut();  /* fp16→YUV LUT（576KB） */
                LOG_INFO("CTBG mode: estimator=%s apply=%s raw=%zub up=%zub",
                         ctbg_cfg.est_om_path, ctbg_cfg.app_om_path,
                         g_ctbg_coeff_raw_sz, g_ctbg_coeff_sz);
            }
        }
    }

    if (pthread_create(&ctrl_tid, TD_NULL, control_worker, &control_ctx) != 0) {
        LOG_ERR("control thread create failed");
        goto cleanup;
    }
    ctrl_up = 1;
    if (cfg.enable_stream) {
        if (pthread_create(&stream_tid, TD_NULL, stream_worker, &runtime_metrics) != 0) {
            LOG_ERR("stream thread create failed");
            goto cleanup_running;
        }
        stream_thread_up = 1;
    }
    /* 控制 socket 线程（可选）：--ctrl-sock 指定后启动 */
    if (cfg.ctrl_sock_path != NULL) {
        app_ctrl_cfg_t actrl_cfg = {
            .sock_path      = cfg.ctrl_sock_path,
            .status_fn      = ctrl_status_fn,
            .status_opaque  = &status_ctx,
        };
        status_ctx.metrics = &runtime_metrics;
        status_ctx.ctrl    = &control_ctx;
        status_ctx.t0      = now_ms();
        if (app_ctrl_init(&actrl_cfg) != 0 ||
            pthread_create(&app_ctrl_tid, TD_NULL, app_ctrl_worker, NULL) != 0) {
            LOG_WARN("app_ctrl: init/thread failed; continuing without control socket");
        } else {
            app_ctrl_up = 1;
        }
    }

    metrics_set_state(&runtime_metrics, control_ctx.health.degraded ? PIPELINE_STATE_DEGRADED
                                                                   : PIPELINE_STATE_RUNNING);
    LOG_INFO("running: state=%s control %ums%s%s%s; Ctrl-C to stop",
             pipeline_state_name((pipeline_state_t)atomic_load(&runtime_metrics.state)),
             APP_CTRL_PERIOD_MS,
             cfg.enable_display ? " + display" : "",
             cfg.enable_stream ? " + H.264/RTSP" : "",
             app_ctrl_up ? " + ctrl-sock" : "");

    /* 显示线程（主）：全分辨率相机帧直通到 HDMI，~30fps；触摸时可切换为菜单覆盖帧。 */
    t0 = now_ms();
    t_last_log = t0;
    while (!g_stop) {
        if (!cfg.enable_display) {
            usleep(100000);
            if (now_ms() - t_last_log >= 5000) {
                if (cfg.enable_stream) {
                    LOG_INFO("stream %llu frames, %llu drops",
                             (unsigned long long)atomic_load(&runtime_metrics.stream_frames),
                             (unsigned long long)atomic_load(&runtime_metrics.stream_drops));
                }
                t_last_log = now_ms();
            }
            continue;
        }

        /* 非阻塞触摸轮询（每帧头部运行，菜单开关均需要） */
        if (touch_fd >= 0 && menu_blk[0] != OT_VB_INVALID_HANDLE) {
            touch_event_t tev;
            while (touch_poll(touch_fd, &tev) == 1) {
                if (tev.pressed && !prev_pressed) {  /* 仅响应上升沿 */
                    if (!menu_active) {
                        /* 左下角 (x<100, y>DISPLAY_HEIGHT-100) 唤出菜单 */
                        if (tev.x < 100 && tev.y > (int)DISPLAY_HEIGHT - 100) {
                            menu_active = 1;
                            LOG_INFO("[menu] opened (corner touch %d,%d)", tev.x, tev.y);
                        }
                    } else {
                        int btn = menu_hittest(tev.x, tev.y);
                        if (btn == MENU_BTN_CLOSE) {
                            menu_active = 0;
                            LOG_INFO("[menu] closed");
                        } else {
                            app_ctrl_params_t mp = {0};
                            switch (btn) {
                            case MENU_BTN_TONE_DEC:
                                mp.has_tone_strength = 1;
                                mp.tone_strength = control_ctx.strength - 0.05f;
                                if (mp.tone_strength < 0.0f) mp.tone_strength = 0.0f;
                                break;
                            case MENU_BTN_TONE_INC:
                                mp.has_tone_strength = 1;
                                mp.tone_strength = control_ctx.strength + 0.05f;
                                if (mp.tone_strength > 1.0f) mp.tone_strength = 1.0f;
                                break;
                            case MENU_BTN_NN_ON:
                                mp.has_nn_clut_enabled = 1;
                                mp.nn_clut_enabled = 1;
                                break;
                            case MENU_BTN_NN_OFF:
                                mp.has_nn_clut_enabled = 1;
                                mp.nn_clut_enabled = 0;
                                break;
                            case MENU_BTN_GUARD_DEC:
                                mp.has_high_clip_guard = 1;
                                mp.high_clip_guard = control_ctx.high_clip_guard - 0.5f;
                                if (mp.high_clip_guard < 0.5f) mp.high_clip_guard = 0.5f;
                                break;
                            case MENU_BTN_GUARD_INC:
                                mp.has_high_clip_guard = 1;
                                mp.high_clip_guard = control_ctx.high_clip_guard + 0.5f;
                                break;
                            case MENU_BTN_RESET:
                                mp.has_tone_strength   = 1;
                                mp.tone_strength       = cfg.tone_strength;
                                mp.has_nn_clut_enabled = 1;
                                mp.nn_clut_enabled     = enable_nn;
                                mp.has_high_clip_guard = 1;
                                mp.high_clip_guard     = APP_NN_HIGH_CLIP_GUARD;
                                break;
                            default:
                                break;
                            }
                            if (mp.has_tone_strength || mp.has_nn_clut_enabled ||
                                mp.has_high_clip_guard) {
                                app_ctrl_push(&mp);
                                LOG_INFO("[menu] btn=%d pushed params", btn);
                            }
                        }
                    }
                }
                prev_pressed = tev.pressed;
            }
        }

        /* 菜单覆盖路径：渲染菜单帧并直接送到 VO，跳过 VPSS */
        if (menu_active && menu_blk[0] != OT_VB_INVALID_HANDLE) {
            menu_state_t ms;
            ot_video_frame_info *mfi;
            /* 节流到相机同款 ~30fps：菜单以 60fps 自旋送显时 VO 帧交换与面板刷新拍频，
             * 撕裂缝在垂直方向滚动（滚动黑条）。按显示帧间隔限速即可消除，相机路径同速率已验证无撕裂。 */
            long nowm = now_ms();
            long menu_interval_ms = 1000L / (cfg.target_fps ? (long)cfg.target_fps : 30L);
            if (nowm - t_last_menu < menu_interval_ms) {
                usleep(2000);  /* 让出 CPU；触摸已在上方轮询，下一轮再判 */
                continue;
            }
            t_last_menu = nowm;
            /* 渲染到 VO 当前未显示的那一块（back buffer），发送后再翻转，避免边显示边重写的撕裂 */
            mfi = &menu_fi[menu_buf_idx];
            unsigned sc = runtime_metrics.timing_samples < APP_TIMING_SAMPLES
                          ? runtime_metrics.timing_samples : APP_TIMING_SAMPLES;
            long el = now_ms() - t0;
            ms.tone        = control_ctx.strength;
            ms.nn          = control_ctx.nn_enabled;
            ms.guard       = control_ctx.high_clip_guard;
            ms.state_str   = pipeline_state_name(
                                 (pipeline_state_t)atomic_load(&runtime_metrics.state));
            ms.fps         = (el > 0) ? (float)(atomic_load(&runtime_metrics.display_frames)
                                                 * 1000.0 / el) : 0.0f;
            ms.infer_ms    = runtime_metrics.infer_last_ms;
            ms.infer_p95   = percentile95(runtime_metrics.infer_samples, sc);
            ms.stream_frames = (unsigned long long)atomic_load(&runtime_metrics.stream_frames);
            ms.lut_updates   = (unsigned long long)atomic_load(&runtime_metrics.lut_updates);
            menu_render_into((uint8_t *)mfi->video_frame.virt_addr[0],
                             (uint8_t *)mfi->video_frame.virt_addr[1],
                             (int)mfi->video_frame.stride[0], &ms);
            (void)ss_mpi_sys_flush_cache(mfi->video_frame.phys_addr[0],
                                         (td_void *)mfi->video_frame.virt_addr[0],
                                         menu_calc_cfg.main_size);
            if (display_send_frame(mfi, -1) == 0) {
                atomic_fetch_add(&runtime_metrics.display_frames, 1);
                menu_buf_idx ^= 1;  /* 翻转到另一块，下一帧渲染 back buffer */
            } else {
                atomic_fetch_add(&runtime_metrics.display_drops, 1);
            }
            if (now_ms() - t_last_log >= 5000) {
                LOG_INFO("display (menu) %llu frames",
                         (unsigned long long)atomic_load(&runtime_metrics.display_frames));
                t_last_log = now_ms();
            }
            continue;
        }

        /* 正常路径：VPSS 直通帧送显示 */
        if (vpss_get_frame(0, 0, &frame, PIPELINE_FRAME_TIMEOUT_MS) != 0) {
            atomic_fetch_add(&runtime_metrics.display_drops, 1);
            atomic_fetch_add(&runtime_metrics.frame_timeouts, 1);
            LOG_WARN("vpss_get_frame timeout/err");
            continue;
        }
        if (display_send_frame(&frame, -1) != 0) {
            (void) vpss_release_frame(0, 0, &frame);
            atomic_fetch_add(&runtime_metrics.display_drops, 1);
            atomic_fetch_add(&runtime_metrics.fatal_errors, 1);
            goto cleanup_running;
        }
        if (vpss_release_frame(0, 0, &frame) != 0) {
            atomic_fetch_add(&runtime_metrics.display_drops, 1);
            atomic_fetch_add(&runtime_metrics.transient_errors, 1);
        }
        atomic_fetch_add(&runtime_metrics.display_frames, 1);
        if (now_ms() - t_last_log >= 5000) {
            uint64_t display_frames = atomic_load(&runtime_metrics.display_frames);
            LOG_INFO("display %llu frames, %.1f fps", (unsigned long long)display_frames,
                     display_frames * 1000.0 / (now_ms() - t0));
            if (cfg.enable_stream) {
                LOG_INFO("stream %llu frames, %llu drops",
                         (unsigned long long)atomic_load(&runtime_metrics.stream_frames),
                         (unsigned long long)atomic_load(&runtime_metrics.stream_drops));
            }
            t_last_log = now_ms();
        }
    }
    run_elapsed_ms = now_ms() - t0;
    rc = control_ctx.fatal_error ? PIPELINE_EXIT_FATAL_RUNTIME : 0;

cleanup_running:
    g_stop = 1;
cleanup:
    if (!control_ctx.fatal_error) {
        metrics_set_state(&runtime_metrics, PIPELINE_STATE_STOPPING);
    }
    if (app_ctrl_up) {
        app_ctrl_stop();
        (void)pthread_join(app_ctrl_tid, TD_NULL);
        app_ctrl_deinit();
    }
    /* 先 join 所有使用 CTBG 资源的线程，再释放 CTBG */
    if (stream_thread_up) {
        (void) pthread_join(stream_tid, TD_NULL);
    }
    if (ctrl_up) {
        (void) pthread_join(ctrl_tid, TD_NULL);
    }
    if (g_ctbg_mode) {
        /* 确保所有 NPU 操作已完成 */
        usleep(50000);  /* 50ms grace period */
        ctbg_deinit();
        free(g_ctbg_coeff);
        free(g_ctbg_coeff_raw);
        free(g_ctbg_nv21_low);
        g_ctbg_coeff = NULL;
        g_ctbg_coeff_raw = NULL;
        g_ctbg_nv21_low = NULL;
        g_ctbg_coeff_ready = 0;
        pthread_mutex_destroy(&g_ctbg_coeff_mutex);
    }
    if (infer_up) {
        CHECK_RET(infer_deinit());
    }
    if (stream_up) {
        CHECK_RET(stream_deinit());
    }
    touch_close(touch_fd);
    for (int i = 0; i < 2; i++) {
        if (menu_blk[i] != OT_VB_INVALID_HANDLE) {
            (void)ss_mpi_sys_munmap(
                (td_void *)menu_fi[i].video_frame.header_virt_addr[0],
                menu_calc_cfg.vb_size);
            (void)ss_mpi_vb_release_blk(menu_blk[i]);
        }
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
    if (rc != 0 && atomic_load(&runtime_metrics.fatal_errors) == 0) {
        atomic_fetch_add(&runtime_metrics.fatal_errors, 1);
    }
    metrics_set_state(&runtime_metrics,
                      (rc == 0) ? PIPELINE_STATE_STOPPED : PIPELINE_STATE_FAILED);
    metrics_snapshot(&runtime_metrics, run_elapsed_ms, &metrics);
    LOG_INFO("%s exit (display=%llu/%.2ffps display_drops=%llu stream=%llu drops=%llu; "
             "ctrl polls=%llu timeouts=%llu infer=%llu lut_req=%llu lut=%llu fail=%llu "
             "transient=%llu fatal=%llu degraded=%llu state=%s infer_p95=%.2fms "
             "transaction_p95=%.2fms infer_max=%.2fms infer_total_max=%.2fms "
             "transaction_max=%.2fms samples=%u)",
             SOCCHINA_APP_NAME, (unsigned long long)metrics.display_frames, metrics.display_fps,
             (unsigned long long)metrics.display_drops,
             (unsigned long long)metrics.stream_frames,
             (unsigned long long)metrics.stream_drops,
             (unsigned long long)metrics.control_polls,
             (unsigned long long)metrics.frame_timeouts,
             (unsigned long long)metrics.infer_runs,
             (unsigned long long)metrics.lut_requests,
             (unsigned long long)metrics.lut_updates,
             (unsigned long long)metrics.lut_failures,
             (unsigned long long)metrics.transient_errors,
             (unsigned long long)metrics.fatal_errors,
             (unsigned long long)metrics.degraded_events, pipeline_state_name(metrics.state),
             metrics.infer_p95_ms, metrics.transaction_p95_ms, metrics.infer_max_ms,
             metrics.infer_total_max_ms, metrics.transaction_max_ms, metrics.timing_samples);
    LOG_INFO("pipeline lifecycle final state=%s exit_code=%d",
             pipeline_state_name(metrics.state), rc);
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
