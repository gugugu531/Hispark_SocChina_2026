/* ui_port_board — 板端 LVGL port(方案 C:离屏渲染 + 合成进 VO 视频帧,绕开 GFBG)。见 ui_port_board.h。 */

#include "ui_port_board.h"

#include "log.h"

#if defined(WITH_SS928_SDK) && defined(ENABLE_LVGL)

#include "lvgl.h"
#include "src/drivers/evdev/lv_evdev.h"

#include "app_control.h"          /* app_ctrl_params_t / app_ctrl_push */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UI_SCR_W 1024
#define UI_SCR_H 600
#define UI_UPDATE_MS 200          /* 状态刷新 ~5Hz */

static pthread_t     g_tid;
static volatile int  g_run;
static ui_snapshot_fn g_snap;
static void         *g_snap_user;
static char          g_touch_dev[64] = "/dev/input/event0";

/* LVGL 离屏渲染缓冲(ARGB8888,FULL 模式整屏一次 flush) */
static uint8_t      *g_render_buf;        /* UI_SCR_W*UI_SCR_H*4,LVGL 渲染目标 */

/* worker → 显示线程的共享 UI 帧(受锁保护) */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t      *g_shared_argb;       /* 最新一帧 ARGB8888(flush_cb 写入) */
static uint32_t      g_shared_version;    /* 每 flush 自增 */
static int           g_have_frame;        /* 首帧已产出 */

/* 显示线程本地:ARGB 快照 + 烘焙的 NV21+alpha 平面(仅版本变化时重算) */
static uint8_t      *g_local_argb;
static uint32_t      g_local_version;     /* 与 g_shared_version 比较 */
static uint8_t      *g_bake_y;            /* UI_SCR_W*UI_SCR_H */
static uint8_t      *g_bake_cr;           /* V */
static uint8_t      *g_bake_cb;           /* U */
static uint8_t      *g_bake_a;            /* alpha */
static int           g_baked_valid;

/* LVGL tick 源:单调时钟毫秒 */
static uint32_t tick_ms_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

/* UI 命令 → app_ctrl_params_t → app_ctrl_push(与 web/旧菜单同一队列,last-write-wins) */
static void cmd_cb(const ui_cmd_t *c, void *user)
{
    (void)user;
    app_ctrl_params_t p;
    memset(&p, 0, sizeof(p));
    if (c->has_enhancement_enabled) { p.has_enhancement_enabled = 1; p.enhancement_enabled = c->enhancement_enabled; }
    if (c->has_nn_clut_enabled)     { p.has_nn_clut_enabled = 1;     p.nn_clut_enabled = c->nn_clut_enabled; }
    if (c->has_tone_enabled)        { p.has_tone_enabled = 1;        p.tone_enabled = c->tone_enabled; }
    if (c->has_tone_strength)       { p.has_tone_strength = 1;       p.tone_strength = c->tone_strength; }
    if (c->has_high_clip_guard)     { p.has_high_clip_guard = 1;     p.high_clip_guard = c->high_clip_guard; }
    if (c->has_drc_mode)            { p.has_drc_mode = 1;            p.drc_mode = c->drc_mode; }
    if (c->has_drc_strength)        { p.has_drc_strength = 1;        p.drc_strength = c->drc_strength; }
    if (c->has_ldci_mode)           { p.has_ldci_mode = 1;           p.ldci_mode = c->ldci_mode; }
    app_ctrl_push(&p);
}

/* LVGL flush:方案 C 不写 fb,而是把整屏 ARGB8888 拷进共享缓冲供显示线程合成。
 * FULL 渲染模式下 area 覆盖整屏、px_map 指向整块渲染缓冲,故整帧拷贝。 */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    pthread_mutex_lock(&g_lock);
    memcpy(g_shared_argb, px_map, (size_t)UI_SCR_W * UI_SCR_H * 4);
    g_shared_version++;
    g_have_frame = 1;
    pthread_mutex_unlock(&g_lock);
    lv_display_flush_ready(disp);
}

/* 由 g_local_argb 烘焙 NV21+alpha 平面。BT.601 limited-range(与 menu_render 的
 * 16..235 调色板一致),NV21=Y 平面 + 交织 VU。ARGB8888 内存序:B,G,R,A。 */
static void rebake(void)
{
    const int n = UI_SCR_W * UI_SCR_H;
    const uint8_t *p = g_local_argb;
    for (int i = 0; i < n; i++, p += 4) {
        uint8_t a = p[3];
        g_bake_a[i] = a;
        if (a == 0) {
            continue;              /* 透明:不需要颜色 */
        }
        int b = p[0], g = p[1], r = p[2];
        int Y  = ((  66 * r + 129 * g +  25 * b + 128) >> 8) + 16;
        int Cb = (((-38 * r -  74 * g + 112 * b + 128) >> 8) + 128);   /* U */
        int Cr = ((( 112 * r -  94 * g -  18 * b + 128) >> 8) + 128);   /* V */
        if (Y  < 0) Y = 0;   else if (Y  > 255) Y = 255;
        if (Cb < 0) Cb = 0;  else if (Cb > 255) Cb = 255;
        if (Cr < 0) Cr = 0;  else if (Cr > 255) Cr = 255;
        g_bake_y[i]  = (uint8_t)Y;
        g_bake_cb[i] = (uint8_t)Cb;
        g_bake_cr[i] = (uint8_t)Cr;
    }
    g_baked_valid = 1;
}

int ui_port_board_active(void)
{
    return g_run && g_have_frame;
}

void ui_port_board_composite_nv21(uint8_t *y, uint8_t *uv, int stride, int w, int h)
{
    if (!y || !uv || w != UI_SCR_W || h != UI_SCR_H) {
        return;
    }

    /* 取共享 ARGB 快照(仅版本变化时拷贝),锁只护一次 memcpy */
    int changed = 0;
    pthread_mutex_lock(&g_lock);
    int have = g_have_frame;
    if (have && g_shared_version != g_local_version) {
        memcpy(g_local_argb, g_shared_argb, (size_t)UI_SCR_W * UI_SCR_H * 4);
        g_local_version = g_shared_version;
        changed = 1;
    }
    pthread_mutex_unlock(&g_lock);

    if (!have) {
        return;
    }
    if (changed || !g_baked_valid) {
        rebake();
    }

    /* 逐像素合成:a==0 跳过、a==255 覆盖、否则与底层视频混合。
     * 色度按 NV21 2x2 子采样,取块左上像素的 alpha。 */
    for (int j = 0; j < h; j++) {
        const int row = j * UI_SCR_W;
        const int yrow = j * stride;
        const int crow = (j >> 1) * stride;
        for (int i = 0; i < w; i++) {
            const int idx = row + i;
            const uint8_t a = g_bake_a[idx];
            if (a == 0) {
                continue;
            }
            const int yoff = yrow + i;
            if (a == 255) {
                y[yoff] = g_bake_y[idx];
            } else {
                int ia = 255 - a;
                y[yoff] = (uint8_t)((g_bake_y[idx] * a + y[yoff] * ia + 127) / 255);
            }
            if ((i & 1) == 0 && (j & 1) == 0) {
                const int coff = crow + i;     /* i 已偶 */
                if (a == 255) {
                    uv[coff]     = g_bake_cr[idx];  /* V */
                    uv[coff + 1] = g_bake_cb[idx];  /* U */
                } else {
                    int ia = 255 - a;
                    uv[coff]     = (uint8_t)((g_bake_cr[idx] * a + uv[coff] * ia + 127) / 255);
                    uv[coff + 1] = (uint8_t)((g_bake_cb[idx] * a + uv[coff + 1] * ia + 127) / 255);
                }
            }
        }
    }
}

static void *worker(void *arg)
{
    (void)arg;

    lv_init();
    lv_tick_set_cb(tick_ms_cb);

    lv_display_t *disp = lv_display_create(UI_SCR_W, UI_SCR_H);
    if (!disp) {
        LOG_ERR("[lvgl] lv_display_create failed");
        return NULL;
    }
    /* ARGB8888:保留 per-pixel alpha,透明屏区域 alpha=0,供合成器让视频透出。
     * LVGL v9 在 has-alpha 的 display 上每次刷新前会把缓冲清为透明。 */
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(disp, g_render_buf, NULL,
                           (uint32_t)UI_SCR_W * UI_SCR_H * 4, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, g_touch_dev);
    if (touch) {
        /* 面板即 1024x600,触摸 1:1 标定 */
        lv_evdev_set_calibration(touch, 0, 0, UI_SCR_W - 1, UI_SCR_H - 1);
    } else {
        LOG_WARN("[lvgl] evdev %s open failed; touch disabled", g_touch_dev);
    }

    ui_lvgl_build(cmd_cb, NULL);
    ui_lvgl_set_overlay_mode();   /* 屏幕/视频区透明:合成时视频透出,顶栏/侧栏覆盖 */

    uint32_t last = tick_ms_cb();
    while (g_run) {
        lv_timer_handler();
        uint32_t now = tick_ms_cb();
        if (now - last >= UI_UPDATE_MS) {
            last = now;
            if (g_snap) {
                ui_state_t st;
                memset(&st, 0, sizeof(st));
                g_snap(&st, g_snap_user);
                ui_lvgl_update(&st);
            }
        }
        usleep(5000);
    }

    lv_deinit();
    return NULL;
}

static void free_buffers(void)
{
    free(g_render_buf);  g_render_buf = NULL;
    free(g_shared_argb); g_shared_argb = NULL;
    free(g_local_argb);  g_local_argb = NULL;
    free(g_bake_y);      g_bake_y = NULL;
    free(g_bake_cr);     g_bake_cr = NULL;
    free(g_bake_cb);     g_bake_cb = NULL;
    free(g_bake_a);      g_bake_a = NULL;
}

int ui_port_board_start(const char *fb_dev, const char *touch_dev,
                        ui_snapshot_fn snap, void *user)
{
    (void)fb_dev;   /* 方案 C 不再使用 fb 设备 */
    if (touch_dev) { strncpy(g_touch_dev, touch_dev, sizeof(g_touch_dev) - 1); g_touch_dev[sizeof(g_touch_dev) - 1] = '\0'; }
    g_snap = snap;
    g_snap_user = user;

    const size_t argb_sz = (size_t)UI_SCR_W * UI_SCR_H * 4;
    const size_t plane_sz = (size_t)UI_SCR_W * UI_SCR_H;
    g_render_buf  = (uint8_t *)malloc(argb_sz);
    g_shared_argb = (uint8_t *)malloc(argb_sz);
    g_local_argb  = (uint8_t *)malloc(argb_sz);
    g_bake_y      = (uint8_t *)malloc(plane_sz);
    g_bake_cr     = (uint8_t *)malloc(plane_sz);
    g_bake_cb     = (uint8_t *)malloc(plane_sz);
    g_bake_a      = (uint8_t *)calloc(plane_sz, 1);   /* 首帧前 alpha=0 → 合成为 no-op */
    if (!g_render_buf || !g_shared_argb || !g_local_argb ||
        !g_bake_y || !g_bake_cr || !g_bake_cb || !g_bake_a) {
        LOG_ERR("[lvgl] UI buffers oom");
        free_buffers();
        return -1;
    }
    g_shared_version = 0;
    g_local_version  = 0;
    g_have_frame     = 0;
    g_baked_valid    = 0;

    g_run = 1;
    if (pthread_create(&g_tid, NULL, worker, NULL) != 0) {
        LOG_ERR("[lvgl] worker thread create failed");
        g_run = 0;
        free_buffers();
        return -1;
    }
    LOG_INFO("[lvgl] board UI worker started (composite path, touch=%s)", g_touch_dev);
    return 0;
}

void ui_port_board_stop(void)
{
    if (!g_run) {
        return;
    }
    g_run = 0;
    pthread_join(g_tid, NULL);
    free_buffers();
    LOG_INFO("[lvgl] board UI worker stopped");
}

#else /* !(WITH_SS928_SDK && ENABLE_LVGL) */

/* 桩:未启用 LVGL 板端 port 时保持链接闭合。 */
int ui_port_board_start(const char *fb_dev, const char *touch_dev,
                        ui_snapshot_fn snap, void *user)
{
    (void)fb_dev; (void)touch_dev; (void)snap; (void)user;
    return -1;
}
void ui_port_board_stop(void) {}
int  ui_port_board_active(void) { return 0; }
void ui_port_board_composite_nv21(uint8_t *y, uint8_t *uv, int stride, int w, int h)
{
    (void)y; (void)uv; (void)stride; (void)w; (void)h;
}

#endif
