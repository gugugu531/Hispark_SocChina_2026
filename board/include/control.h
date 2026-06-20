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

/* CoTF 路线的 LUT 刷新策略（纯逻辑、可主机单测）。
 *
 * CoTF 把"出 LUT"（NN/NPU, ~1ms）与"施加"（ISP 硬件, 每帧, 零 NPU）解耦：ISP 按 30fps 对每帧施加当前
 * LUT，NN 只在**需要时**低频刷新 LUT。本函数判决"此刻是否该重跑 NN 刷新 LUT"，把模型移出每帧关键路径。
 *
 * 策略（带迟滞/限流，避免抖动与无谓刷新）：
 *   - frames_since_refresh < MIN_INTERVAL  → 不刷（限流，防抖）
 *   - frames_since_refresh >= MAX_INTERVAL → 刷（周期下限，缓慢漂移也跟上）
 *   - 否则：场景较上次刷新变化超阈值（亮度/裁剪比例）→ 刷
 *
 * prev = 上次刷新 LUT 时的统计；cur = 当前统计；frames_since_refresh = 距上次刷新的帧数。
 * 阈值/间隔为初版经验值，待相机实测标定。返回非 0 表示应刷新。
 */
int control_should_refresh_lut(const luma_stats_t *prev, const luma_stats_t *cur,
                               unsigned frames_since_refresh);

/* 连续失败降级门：success 清零；failure 累计到 limit 后进入 degraded。 */
typedef struct {
    unsigned consecutive_failures;
    int degraded;
} control_health_t;

void control_health_init(control_health_t *health);
int control_health_record(control_health_t *health, int success, unsigned failure_limit);

/* post-CLUT 反馈抑制：对统计做低通，场景变化需连续确认，刷新后进入短冷却期。
 * 所有计数单位都是 control worker 的轮询周期。 */
typedef struct {
    luma_stats_t filtered;
    luma_stats_t committed;
    unsigned ticks_since_commit;
    unsigned cooldown_remaining;
    unsigned change_streak;
    int have_filtered;
    int have_committed;
} control_feedback_t;

void control_feedback_init(control_feedback_t *feedback);
int control_feedback_observe(control_feedback_t *feedback, const luma_stats_t *raw,
                             luma_stats_t *filtered_out);
void control_feedback_commit(control_feedback_t *feedback);

#endif /* SOCCHINA_CONTROL_H */
