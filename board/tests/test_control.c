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
    luma_stats_t dark = {30.0f, 0.1f, 25.0f};
    luma_stats_t bright = {200.0f, 8.0f, 0.2f};
    luma_stats_t mixed = {120.0f, 6.0f, 15.0f};
    luma_stats_t normal = {110.0f, 0.5f, 1.0f};

    check("dark->brighten", control_decide(&dark), EXPO_MODE_BRIGHTEN);
    check("bright->compress", control_decide(&bright), EXPO_MODE_COMPRESS);
    check("mixed->bidir", control_decide(&mixed), EXPO_MODE_BIDIR);
    check("normal->bypass", control_decide(&normal), EXPO_MODE_BYPASS);
    check("null->bypass", control_decide(NULL), EXPO_MODE_BYPASS);

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

    printf("%s: %d failure(s)\n", g_fails ? "RESULT FAIL" : "RESULT OK", g_fails);
    return g_fails ? 1 : 0;
}
