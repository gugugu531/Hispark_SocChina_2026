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
