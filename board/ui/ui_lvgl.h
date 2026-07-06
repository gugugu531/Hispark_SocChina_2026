#ifndef SOCCHINA_UI_LVGL_H
#define SOCCHINA_UI_LVGL_H

/* ui_lvgl — 板端触摸控制台 UI（LVGL 实现，形态对齐 web-console）。
 *
 * 本文件是 UI 的"抽象口":UI 只依赖
 *   - ui_state_t：一份实时状态快照（读）；
 *   - ui_cmd_t + ui_cmd_cb：参数变更命令（写）。
 * 与"画面怎么上屏"和"数据从哪来"完全解耦:
 *   - 板端:cb 把 ui_cmd_t 映射为 app_ctrl_params_t 并 app_ctrl_push;
 *           update 由 display_worker 从 metrics/control 快照周期喂入。
 *   - 主机模拟器:cb 更新 mock 状态并打印;update 由 mock 数据源喂入。
 * LVGL 的 display flush / indev 由各自的 port 层提供,不在本模块内。
 */

#include <stdint.h>

/* 实时状态快照(镜像 web /api/v1/status 的字段集)。 */
typedef struct {
    const char        *state_str;        /* "RUNNING" / "DEGRADED" / "FAILED" / ... */
    int                capture_mode;     /* 0 linear / 1 wdr2to1 */
    int                target_fps;
    float              fps;              /* 实时显示帧率 */
    float              infer_ms;         /* 最近一次推理耗时 */
    float              infer_p95;        /* 推理 p95 */
    float              txn_p95;          /* LUT 事务 p95 */
    unsigned long long stream_frames;
    unsigned long long lut_updates;
    long               stream_drops;
    long               timeouts;
    long               transient_errors;
    long               fatal_errors;
    int                nn_degraded;      /* 推理降级 */
    /* 输出通道(板端可只填部分,-1 表示未知) */
    int                hdmi, rtsp, viewers;

    /* 当前热参数(用于回填控件,避免与用户操作打架由 UI 内部处理) */
    int                enhancement_enabled;
    int                nn_enabled;
    int                tone_enabled;
    float              tone_strength;    /* 0..1 */
    float              high_clip_guard;  /* 0..100 */
    int                drc_mode;         /* 0 off / 1 auto */
    int                drc_strength;     /* 0..1023 */
    int                ldci_mode;        /* 0 off / 1 auto */
} ui_state_t;

/* UI -> 后端命令(镜像 app_ctrl_params_t 的 "has_ + 值" 风格,
 * 只携带用户刚改动的字段)。 */
typedef struct {
    int   has_enhancement_enabled; int   enhancement_enabled;
    int   has_nn_clut_enabled;     int   nn_clut_enabled;
    int   has_tone_enabled;        int   tone_enabled;
    int   has_tone_strength;       float tone_strength;
    int   has_high_clip_guard;     float high_clip_guard;
    int   has_drc_mode;            int   drc_mode;
    int   has_drc_strength;        int   drc_strength;
    int   has_ldci_mode;           int   ldci_mode;
} ui_cmd_t;

/* 用户在 UI 上改动参数时回调。cmd 只置位改动的字段。 */
typedef void (*ui_cmd_cb)(const ui_cmd_t *cmd, void *user);

/* 在 LVGL 当前 active screen 上构建 UI(控件、布局、样式、事件)。
 * cb/user 保存供事件回调使用。调用前需已 lv_init() 且 display 已注册。 */
void ui_lvgl_build(ui_cmd_cb cb, void *user);

/* 板端 overlay 模式：屏幕/视频区置透明，让 GFBG G0 下方 VO 视频层透出（顶栏/侧边栏仍不透明）。
 * 在 ui_lvgl_build 之后调用；sim 不调用（保持不透明预览）。 */
void ui_lvgl_set_overlay_mode(void);

/* 用最新状态刷新只读显示与控件回填(建议 5..10 Hz 周期调用)。
 * 回填控件时会屏蔽事件,不会反向触发 cb。 */
void ui_lvgl_update(const ui_state_t *st);

#endif /* SOCCHINA_UI_LVGL_H */
