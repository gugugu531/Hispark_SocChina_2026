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

    printf("%s: %d failure(s)\n", g_fails ? "RESULT FAIL" : "RESULT OK", g_fails);
    return g_fails ? 1 : 0;
}
