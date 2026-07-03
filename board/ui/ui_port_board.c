/* ui_port_board — 板端 LVGL port(方案 A:GFBG G0 图形层叠加)。见 ui_port_board.h。 */

#include "ui_port_board.h"

#include "log.h"

#if defined(WITH_SS928_SDK) && defined(ENABLE_LVGL)

#include "lvgl.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include "src/drivers/evdev/lv_evdev.h"

#include "ot_type.h"
#include "gfbg.h"                 /* GFBG ioctl + ot_fb_* */

#include "app_control.h"          /* app_ctrl_params_t / app_ctrl_push */

#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define UI_SCR_W 1024
#define UI_SCR_H 600
#define UI_UPDATE_MS 200          /* 状态刷新 ~5Hz */

static pthread_t     g_tid;
static volatile int  g_run;
static ui_snapshot_fn g_snap;
static void         *g_snap_user;
static char          g_fb_dev[64]    = "/dev/fb0";
static char          g_touch_dev[64] = "/dev/input/event0";

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

/* 配置 GFBG G0:ARGB8888 / 双缓冲 / 抗闪烁 auto / 像素 alpha / 显示使能。
 * 在 lv_linux_fbdev 打开该 fb 之前完成(设备层由 display_init 已使能)。 */
static int gfbg_setup(const char *dev)
{
    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        LOG_ERR("[lvgl] gfbg open %s failed", dev);
        return -1;
    }

    /* 1) vscreeninfo:32bpp ARGB8888,1024x600,yres_virtual 双缓冲 */
    struct fb_var_screeninfo vinfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        LOG_ERR("[lvgl] FBIOGET_VSCREENINFO failed");
        close(fd);
        return -1;
    }
    vinfo.xres = UI_SCR_W;
    vinfo.yres = UI_SCR_H;
    vinfo.xres_virtual = UI_SCR_W;
    vinfo.yres_virtual = UI_SCR_H * 2;   /* 双缓冲 */
    vinfo.bits_per_pixel = 32;
    vinfo.transp.offset = 24; vinfo.transp.length = 8;
    vinfo.red.offset    = 16; vinfo.red.length    = 8;
    vinfo.green.offset  = 8;  vinfo.green.length  = 8;
    vinfo.blue.offset   = 0;  vinfo.blue.length   = 8;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &vinfo) < 0) {
        LOG_ERR("[lvgl] FBIOPUT_VSCREENINFO failed");
        close(fd);
        return -1;
    }

    /* 2) layer info:双缓冲 + 抗闪烁 auto + 全屏位置/尺寸 */
    ot_fb_layer_info li;
    memset(&li, 0, sizeof(li));
    li.buf_mode = OT_FB_LAYER_BUF_DOUBLE;
    li.antiflicker_level = OT_FB_LAYER_ANTIFLICKER_AUTO;
    li.x_pos = 0;
    li.y_pos = 0;
    li.canvas_width  = UI_SCR_W; li.canvas_height  = UI_SCR_H;
    li.display_width = UI_SCR_W; li.display_height = UI_SCR_H;
    li.screen_width  = UI_SCR_W; li.screen_height  = UI_SCR_H;
    li.mask = OT_FB_LAYER_MASK_BUF_MODE | OT_FB_LAYER_MASK_ANTIFLICKER_MODE |
              OT_FB_LAYER_MASK_POS | OT_FB_LAYER_MASK_CANVAS_SIZE |
              OT_FB_LAYER_MASK_DISPLAY_SIZE | OT_FB_LAYER_MASK_SCREEN_SIZE;
    if (ioctl(fd, FBIOPUT_LAYER_INFO, &li) < 0) {
        LOG_WARN("[lvgl] FBIOPUT_LAYER_INFO failed (continuing)");
    }

    /* 3) 像素 alpha 使能:未绘制区域(alpha=0)透出下方视频层 */
    ot_fb_alpha alpha;
    memset(&alpha, 0, sizeof(alpha));
    alpha.alpha_en = TD_TRUE;         /* per-pixel alpha */
    alpha.alpha_chn_en = TD_FALSE;
    alpha.global_alpha = 0xff;
    if (ioctl(fd, FBIOPUT_ALPHA_GFBG, &alpha) < 0) {
        LOG_WARN("[lvgl] FBIOPUT_ALPHA_GFBG failed (continuing)");
    }

    /* 4) 显示使能 */
    td_bool show = TD_TRUE;
    if (ioctl(fd, FBIOPUT_SHOW_GFBG, &show) < 0) {
        LOG_WARN("[lvgl] FBIOPUT_SHOW_GFBG failed (continuing)");
    }

    close(fd);
    LOG_INFO("[lvgl] GFBG G0 configured: %s ARGB8888 %dx%d double-buffer antiflicker=auto",
             dev, UI_SCR_W, UI_SCR_H);
    return 0;
}

static void *worker(void *arg)
{
    (void)arg;

    lv_init();
    lv_tick_set_cb(tick_ms_cb);

    lv_display_t *disp = lv_linux_fbdev_create();
    if (!disp) {
        LOG_ERR("[lvgl] lv_linux_fbdev_create failed");
        return NULL;
    }
    lv_linux_fbdev_set_file(disp, g_fb_dev);

    lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, g_touch_dev);
    if (touch) {
        /* 触摸原始坐标 → 屏幕坐标;实际标定值待上板调 */
        lv_evdev_set_calibration(touch, 0, 0, UI_SCR_W - 1, UI_SCR_H - 1);
    } else {
        LOG_WARN("[lvgl] evdev %s open failed; touch disabled", g_touch_dev);
    }

    ui_lvgl_build(cmd_cb, NULL);

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

int ui_port_board_start(const char *fb_dev, const char *touch_dev,
                        ui_snapshot_fn snap, void *user)
{
    if (fb_dev)    { strncpy(g_fb_dev, fb_dev, sizeof(g_fb_dev) - 1);       g_fb_dev[sizeof(g_fb_dev) - 1] = '\0'; }
    if (touch_dev) { strncpy(g_touch_dev, touch_dev, sizeof(g_touch_dev) - 1); g_touch_dev[sizeof(g_touch_dev) - 1] = '\0'; }
    g_snap = snap;
    g_snap_user = user;

    if (gfbg_setup(g_fb_dev) != 0) {
        return -1;
    }

    g_run = 1;
    if (pthread_create(&g_tid, NULL, worker, NULL) != 0) {
        LOG_ERR("[lvgl] worker thread create failed");
        g_run = 0;
        return -1;
    }
    LOG_INFO("[lvgl] board UI worker started (fb=%s touch=%s)", g_fb_dev, g_touch_dev);
    return 0;
}

void ui_port_board_stop(void)
{
    if (!g_run) {
        return;
    }
    g_run = 0;
    pthread_join(g_tid, TD_NULL);
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

#endif
