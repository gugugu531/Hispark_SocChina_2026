#include <stdio.h>

#include "control.h"

static int g_fails = 0;

static void check(const char *name, expo_mode_t got, expo_mode_t want)
{
    if (got != want) {
        printf("FAIL %-22s got=%d want=%d\n", name, got, want);
        g_fails++;
    } else {
        printf("ok   %-22s\n", name);
    }
}

static void check_i(const char *name, int got, int want)
{
    if (got != want) {
        printf("FAIL %-22s got=%d want=%d\n", name, got, want);
        g_fails++;
    } else {
        printf("ok   %-22s\n", name);
    }
}

int main(void)
{
    /* 判决用 AE 统计域数值（AE 把 mean 钉在设定点 ≈53；mean>设定点=AE 压不下的过曝，
     * mean<设定点=AE 顶到曝光上限的欠曝）。详见 control.c 阈值标定注释。 */
    check("null->bypass", control_decide(NULL), EXPO_MODE_BYPASS);

    /* 标定锚点：AE 设定点附近、仅不可消除窗光 → 正常偏亮，不动（修复过度提亮的核心用例）。 */
    luma_stats_t anchor = {53.0f, 1.9f, 3.1f};
    check("ae-anchor->bypass", control_decide(&anchor), EXPO_MODE_BYPASS);
    /* 真实欠曝：AE 顶到曝光上限 mean 仍低于设定点、暗部大量裁剪 → 提亮。 */
    luma_stats_t under = {32.0f, 0.2f, 22.0f};
    check("underexposed->brighten", control_decide(&under), EXPO_MODE_BRIGHTEN);
    /* 真实过曝（裁剪驱动）：高光大量裁剪 → 压高光。 */
    luma_stats_t over_clip = {78.0f, 9.0f, 0.1f};
    check("overclip->compress", control_decide(&over_clip), EXPO_MODE_COMPRESS);
    /* 真实过曝（亮度驱动）：AE mean 远超设定点压不下来 → 压高光。 */
    luma_stats_t over_mean = {100.0f, 0.5f, 0.5f};
    check("overmean->compress", control_decide(&over_mean), EXPO_MODE_COMPRESS);
    /* 背光高动态：暗部裁剪 + 高光裁剪并存 → 双向。 */
    luma_stats_t hdr = {45.0f, 5.0f, 14.0f};
    check("hdr->bidir", control_decide(&hdr), EXPO_MODE_BIDIR);

    /* 高光优先（过曝不可逆）：以下场景必须不返回 BRIGHTEN。 */
    /* 暗部裁剪但已有可见窗光裁剪（超提亮抑制阈值）→ 不纯提亮，维持现状。 */
    luma_stats_t dark_with_window = {38.0f, 2.0f, 12.0f};
    check("dark-w-window->bypass", control_decide(&dark_with_window), EXPO_MODE_BYPASS);
    /* AE mean 已在设定点之上（超提亮天花板）却有少量暗部 → 不提亮。 */
    luma_stats_t above_setpoint = {65.0f, 0.3f, 12.0f};
    check("above-ceiling->bypass", control_decide(&above_setpoint), EXPO_MODE_BYPASS);

    /* CoTF LUT 刷新策略：限流 / 周期下限 / 场景变化迟滞 */
    luma_stats_t a = {110.0f, 0.5f, 1.0f};
    luma_stats_t a_small = {113.0f, 0.6f, 1.2f};  /* 变化小于阈值 */
    luma_stats_t a_big = {130.0f, 0.5f, 1.0f};    /* 亮度变化超阈值 */
    check_i("lut: no prev -> refresh", control_should_refresh_lut(NULL, &a, 100), 1);
    check_i("lut: null cur -> no", control_should_refresh_lut(&a, NULL, 100), 0);
    check_i("lut: min-interval limit", control_should_refresh_lut(&a, &a_big, 1), 0);
    check_i("lut: max-interval force", control_should_refresh_lut(&a, &a, 60), 1);
    check_i("lut: scene change -> yes", control_should_refresh_lut(&a, &a_big, 10), 1);
    check_i("lut: small change -> no", control_should_refresh_lut(&a, &a_small, 10), 0);

    {
        control_health_t health;
        control_health_init(&health);
        check_i("health: initial", health.degraded, 0);
        check_i("health: fail 1", control_health_record(&health, 0, 3), 0);
        check_i("health: success resets", control_health_record(&health, 1, 3), 0);
        check_i("health: fail 1 again", control_health_record(&health, 0, 3), 0);
        check_i("health: fail 2", control_health_record(&health, 0, 3), 0);
        check_i("health: fail 3 degrades", control_health_record(&health, 0, 3), 1);
        check_i("health: degraded sticky", control_health_record(&health, 1, 3), 1);
    }

    {
        control_feedback_t feedback;
        luma_stats_t filtered;
        luma_stats_t base = {100.0f, 1.0f, 2.0f};
        luma_stats_t changed = {180.0f, 1.0f, 2.0f};
        int refresh = 0;

        control_feedback_init(&feedback);
        check_i("feedback: first refresh",
                control_feedback_observe(&feedback, &base, &filtered), 1);
        control_feedback_commit(&feedback);
        for (int i = 0; i < 10; i++) {
            refresh |= control_feedback_observe(&feedback, &changed, &filtered);
        }
        check_i("feedback: cooldown blocks", refresh, 0);
        check_i("feedback: confirm 1",
                control_feedback_observe(&feedback, &changed, &filtered), 0);
        check_i("feedback: confirm 2",
                control_feedback_observe(&feedback, &changed, &filtered), 0);
        check_i("feedback: confirm 3",
                control_feedback_observe(&feedback, &changed, &filtered), 1);
        control_feedback_commit(&feedback);
        check_i("feedback: commit cooldown",
                control_feedback_observe(&feedback, &changed, &filtered), 0);
    }

    printf("%s: %d failure(s)\n", g_fails ? "RESULT FAIL" : "RESULT OK", g_fails);
    return g_fails ? 1 : 0;
}
