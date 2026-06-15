#include <stddef.h>

#include "control.h"

/* 判决阈值（初版经验值，后续按相机/显示实测标定）。 */
#define HIGH_CLIP_TH   3.0f   /* 高光裁剪比例阈值 % */
#define LOW_CLIP_TH    10.0f  /* 暗部裁剪比例阈值 % */
#define BRIGHT_LUMA_TH 180.0f /* 整体偏亮阈值 */
#define DARK_LUMA_TH   60.0f  /* 整体偏暗阈值 */

expo_mode_t control_decide(const luma_stats_t *stats)
{
    if (stats == NULL) {
        return EXPO_MODE_BYPASS;
    }

    int over = (stats->clip_high_pct > HIGH_CLIP_TH) || (stats->mean_luma > BRIGHT_LUMA_TH);
    int under = (stats->clip_low_pct > LOW_CLIP_TH) || (stats->mean_luma < DARK_LUMA_TH);

    if (over && under) {
        return EXPO_MODE_BIDIR;
    }
    if (over) {
        return EXPO_MODE_COMPRESS;
    }
    if (under) {
        return EXPO_MODE_BRIGHTEN;
    }
    return EXPO_MODE_BYPASS;
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
