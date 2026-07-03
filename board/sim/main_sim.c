/* main_sim — board/ui 的主机 LVGL 模拟器。
 * SDL 显示 + 鼠标/键盘 indev + mock 数据源,让 ui_lvgl 在 PC 上可交互预览。
 * 与板端共用同一份 ui_lvgl.c;差异只在这里的 port(SDL flush/indev)与 mock 数据。 */

#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_keyboard.h"

#include "ui_lvgl.h"

#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>

#define SIM_W 1024
#define SIM_H 600

static ui_state_t g_mock;

/* UI 改动参数 -> 更新 mock 状态并打印(镜像板端 app_ctrl_push 的效果) */
static void on_cmd(const ui_cmd_t *c, void *user)
{
    (void)user;
    if (c->has_enhancement_enabled) g_mock.enhancement_enabled = c->enhancement_enabled;
    if (c->has_nn_clut_enabled)     g_mock.nn_enabled          = c->nn_clut_enabled;
    if (c->has_tone_enabled)        g_mock.tone_enabled        = c->tone_enabled;
    if (c->has_tone_strength)       g_mock.tone_strength       = c->tone_strength;
    if (c->has_high_clip_guard)     g_mock.high_clip_guard     = c->high_clip_guard;
    if (c->has_drc_mode)            g_mock.drc_mode            = c->drc_mode;
    if (c->has_drc_strength)        g_mock.drc_strength        = c->drc_strength;
    if (c->has_ldci_mode)           g_mock.ldci_mode           = c->ldci_mode;

    printf("[cmd]");
    if (c->has_enhancement_enabled) printf(" enh=%d", c->enhancement_enabled);
    if (c->has_nn_clut_enabled)     printf(" nn=%d", c->nn_clut_enabled);
    if (c->has_tone_enabled)        printf(" tone_en=%d", c->tone_enabled);
    if (c->has_tone_strength)       printf(" tone=%.2f", c->tone_strength);
    if (c->has_high_clip_guard)     printf(" guard=%.0f", c->high_clip_guard);
    if (c->has_drc_mode)            printf(" drc=%d", c->drc_mode);
    if (c->has_drc_strength)        printf(" drc_str=%d", c->drc_strength);
    if (c->has_ldci_mode)           printf(" ldci=%d", c->ldci_mode);
    printf("\n");
    fflush(stdout);
}

static void mock_init(void)
{
    g_mock.state_str           = "RUNNING";
    g_mock.capture_mode        = 0;
    g_mock.target_fps          = 30;
    g_mock.fps                 = 30.0f;
    g_mock.infer_ms            = 1.35f;
    g_mock.infer_p95           = 1.40f;
    g_mock.txn_p95             = 4.40f;
    g_mock.stream_frames       = 0;
    g_mock.lut_updates         = 0;
    g_mock.stream_drops        = 0;
    g_mock.timeouts            = 0;
    g_mock.nn_degraded         = 0;
    g_mock.hdmi                = 1;
    g_mock.rtsp                = 1;
    g_mock.viewers             = 1;
    g_mock.enhancement_enabled = 1;
    g_mock.nn_enabled          = 1;
    g_mock.tone_enabled        = 1;
    g_mock.tone_strength       = 0.25f;
    g_mock.high_clip_guard     = 3.0f;
    g_mock.drc_mode            = 1;
    g_mock.drc_strength        = 512;
    g_mock.ldci_mode           = 1;
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(SDL_GetTicks);

    lv_display_t *disp = lv_sdl_window_create(SIM_W, SIM_H);
    (void)disp;
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();

    mock_init();
    ui_lvgl_build(on_cmd, NULL);
    ui_lvgl_update(&g_mock);

    printf("socchina_sim: LVGL %d.%d.%d @ %dx%d — 拖动滑块/切开关/点预设试试\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, SIM_W, SIM_H);
    fflush(stdout);

    uint32_t last = SDL_GetTicks();
    for (;;) {
        lv_timer_handler();

        uint32_t now = SDL_GetTicks();
        if (now - last >= 200) {   /* ~5Hz 刷新 mock 指标,模拟实时数据 */
            last = now;
            double t = now / 1000.0;
            g_mock.fps       = 30.0f + (float)sin(t) * 0.3f;
            g_mock.infer_p95 = 1.35f + (float)fabs(sin(t * 0.7)) * 0.25f;
            g_mock.txn_p95   = g_mock.infer_p95 + 3.0f;
            g_mock.stream_frames += 6;
            g_mock.lut_updates   += 3;
            ui_lvgl_update(&g_mock);
        }
        SDL_Delay(5);
    }
    return 0;
}
