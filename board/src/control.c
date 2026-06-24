#include <stddef.h>

#include "control.h"

/* 判决阈值（重标定起点，高光优先）。
 *
 * 设计依据：过曝是 sensor 像素阱饱和导致的**不可逆**信息丢失，而提亮暗部是可逆的。
 * 因此判决偏保守：宁可暗部欠提一点，也不让规则 Gamma 把已接近裁剪的高光顶爆。
 *
 * 注：这些仍是经验起点，需用**输出域**（如 RTSP 抓帧）实测的 mean/clip 再标定——
 * AE 统计的亮度域不一定与输出亮度域对齐（见 docs/TODO.md 阈值标定项）。
 */
#define HIGH_CLIP_COMPRESS_TH 1.5f   /* 高光裁剪超此 → 主动压高光（原 3.0 偏松，2.5% 已可见裁剪） */
#define HIGH_CLIP_INHIBIT_TH  0.8f   /* 高光裁剪超此 → 禁止纯提亮（提亮会推高已裁剪区） */
#define BRIGHT_LUMA_TH        160.0f /* 整体偏亮阈值（原 180） */
#define BRIGHTEN_LUMA_CEIL    140.0f /* 整体亮度达此即不再纯提亮 */
#define LOW_CLIP_TH           10.0f  /* 暗部裁剪比例阈值 % */
#define DARK_LUMA_TH          60.0f  /* 整体偏暗阈值 */

expo_mode_t control_decide(const luma_stats_t *stats)
{
    int over;
    int dark;
    int brighten_safe;

    if (stats == NULL) {
        return EXPO_MODE_BYPASS;
    }

    over = (stats->clip_high_pct > HIGH_CLIP_COMPRESS_TH) ||
           (stats->mean_luma > BRIGHT_LUMA_TH);
    dark = (stats->clip_low_pct > LOW_CLIP_TH) ||
           (stats->mean_luma < DARK_LUMA_TH);
    /* 高光优先：已有可观高光裁剪或整体已偏亮时，纯提亮会把高光顶爆 → 抑制 BRIGHTEN。
     * COMPRESS（gamma>1）与 BIDIR（log 曲线）本身压高光，不受此抑制。 */
    brighten_safe = (stats->clip_high_pct <= HIGH_CLIP_INHIBIT_TH) &&
                    (stats->mean_luma < BRIGHTEN_LUMA_CEIL);

    if (over && dark) {
        return EXPO_MODE_BIDIR;     /* 高动态：log 曲线同时提暗压亮，不顶爆高光 */
    }
    if (over) {
        return EXPO_MODE_COMPRESS;  /* 高光优先压制 */
    }
    if (dark && brighten_safe) {
        return EXPO_MODE_BRIGHTEN;  /* 仅当提亮不会顶爆高光时才提亮 */
    }
    return EXPO_MODE_BYPASS;        /* 含“暗但提亮不安全”→ 维持现状，避免过曝 */
}

/* LUT 刷新策略阈值（初版经验值，待相机实测标定；假定控制环 ~30Hz）。 */
#define LUT_MIN_INTERVAL    3    /* 最快 ~10Hz 刷新，限流防抖 */
#define LUT_MAX_INTERVAL    60   /* 最慢 ~0.5Hz 刷新，缓慢漂移也跟上 */
#define LUT_DLUMA_TH        8.0f /* 平均亮度变化阈值 */
#define LUT_DCLIP_TH        3.0f /* 高/低裁剪比例变化阈值 % */

static float fabsf_local(float x)
{
    return x < 0.0f ? -x : x;
}

int control_should_refresh_lut(const luma_stats_t *prev, const luma_stats_t *cur,
                               unsigned frames_since_refresh)
{
    if (cur == NULL) {
        return 0;
    }
    if (prev == NULL) {
        return 1; /* 尚无 LUT，立即刷一次 */
    }
    if (frames_since_refresh < LUT_MIN_INTERVAL) {
        return 0; /* 限流 */
    }
    if (frames_since_refresh >= LUT_MAX_INTERVAL) {
        return 1; /* 周期下限 */
    }
    int scene_changed = (fabsf_local(cur->mean_luma - prev->mean_luma) > LUT_DLUMA_TH) ||
                        (fabsf_local(cur->clip_high_pct - prev->clip_high_pct) > LUT_DCLIP_TH) ||
                        (fabsf_local(cur->clip_low_pct - prev->clip_low_pct) > LUT_DCLIP_TH);
    return scene_changed ? 1 : 0;
}

void control_health_init(control_health_t *health)
{
    if (health == NULL) {
        return;
    }
    health->consecutive_failures = 0;
    health->degraded = 0;
}

int control_health_record(control_health_t *health, int success, unsigned failure_limit)
{
    if (health == NULL || failure_limit == 0) {
        return 0;
    }
    if (success) {
        health->consecutive_failures = 0;
        return health->degraded;
    }
    if (health->consecutive_failures < failure_limit) {
        health->consecutive_failures++;
    }
    if (health->consecutive_failures >= failure_limit) {
        health->degraded = 1;
    }
    return health->degraded;
}

#define FEEDBACK_ALPHA_NUM      1u
#define FEEDBACK_ALPHA_DEN      4u
#define FEEDBACK_CONFIRM_POLLS  3u
#define FEEDBACK_COOLDOWN_POLLS 10u

static float lowpass(float previous, float current)
{
    return previous +
           (current - previous) * (float)FEEDBACK_ALPHA_NUM / (float)FEEDBACK_ALPHA_DEN;
}

void control_feedback_init(control_feedback_t *feedback)
{
    if (feedback == NULL) {
        return;
    }
    feedback->filtered = (luma_stats_t){0};
    feedback->committed = (luma_stats_t){0};
    feedback->ticks_since_commit = 0;
    feedback->cooldown_remaining = 0;
    feedback->change_streak = 0;
    feedback->have_filtered = 0;
    feedback->have_committed = 0;
}

int control_feedback_observe(control_feedback_t *feedback, const luma_stats_t *raw,
                             luma_stats_t *filtered_out)
{
    int changed;

    if (feedback == NULL || raw == NULL) {
        return 0;
    }
    if (!feedback->have_filtered) {
        feedback->filtered = *raw;
        feedback->have_filtered = 1;
    } else {
        feedback->filtered.mean_luma =
            lowpass(feedback->filtered.mean_luma, raw->mean_luma);
        feedback->filtered.clip_high_pct =
            lowpass(feedback->filtered.clip_high_pct, raw->clip_high_pct);
        feedback->filtered.clip_low_pct =
            lowpass(feedback->filtered.clip_low_pct, raw->clip_low_pct);
    }
    if (filtered_out != NULL) {
        *filtered_out = feedback->filtered;
    }
    feedback->ticks_since_commit++;

    if (!feedback->have_committed) {
        return 1;
    }
    if (feedback->cooldown_remaining > 0) {
        feedback->cooldown_remaining--;
        feedback->change_streak = 0;
        return 0;
    }
    changed = control_should_refresh_lut(&feedback->committed, &feedback->filtered,
                                         feedback->ticks_since_commit);
    if (!changed) {
        feedback->change_streak = 0;
        return 0;
    }
    feedback->change_streak++;
    return feedback->change_streak >= FEEDBACK_CONFIRM_POLLS;
}

void control_feedback_commit(control_feedback_t *feedback)
{
    if (feedback == NULL || !feedback->have_filtered) {
        return;
    }
    feedback->committed = feedback->filtered;
    feedback->have_committed = 1;
    feedback->ticks_since_commit = 0;
    feedback->cooldown_remaining = FEEDBACK_COOLDOWN_POLLS;
    feedback->change_streak = 0;
}
