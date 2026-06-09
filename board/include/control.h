#ifndef SOCCHINA_CONTROL_H
#define SOCCHINA_CONTROL_H

/* 场景自适应曝光校正模式判决（数据通路的“控制大脑”）。
 * 纯逻辑、不依赖 SDK/硬件，可在主机做单元测试。 */

typedef enum {
    EXPO_MODE_BYPASS = 0, /* 正常曝光：不增强 */
    EXPO_MODE_BRIGHTEN,   /* 欠曝：提亮暗部 */
    EXPO_MODE_COMPRESS,   /* 过曝：压制高光 */
    EXPO_MODE_BIDIR       /* 混合曝光：双向校正 */
} expo_mode_t;

typedef struct {
    float mean_luma;     /* 平均亮度 0..255 */
    float clip_high_pct; /* 接近白场（过曝）像素比例 0..100 */
    float clip_low_pct;  /* 接近黑场（欠曝）像素比例 0..100 */
} luma_stats_t;

/* 根据亮度统计判决曝光校正模式。 */
expo_mode_t control_decide(const luma_stats_t *stats);

#endif /* SOCCHINA_CONTROL_H */
