#ifndef SOCCHINA_MENU_RENDER_H
#define SOCCHINA_MENU_RENDER_H

#include <stdint.h>

/* ---- 按钮 ID ---- */
#define MENU_BTN_NONE       (-1)
#define MENU_BTN_CLOSE       0   /* 右上角 [X] */
#define MENU_BTN_TONE_DEC    1   /* Tone [-] */
#define MENU_BTN_TONE_INC    2   /* Tone [+] */
#define MENU_BTN_NN_ON       3   /* NN [ON]  */
#define MENU_BTN_NN_OFF      4   /* NN [OFF] */
#define MENU_BTN_GUARD_DEC   5   /* Guard [-] */
#define MENU_BTN_GUARD_INC   6   /* Guard [+] */
#define MENU_BTN_RESET       7   /* [RESET]  */
#define MENU_BTN_COUNT       8

/* ---- 菜单状态（由调用方维护） ---- */
typedef struct {
    float tone;      /* 0.0 .. 1.0 */
    int   nn;        /* 0/1 */
    float guard;     /* > 0.0 */
    /* 实时指标（display_worker 从 metrics 快照填写） */
    const char *state_str;  /* "RUNNING" / "STARTING" / ... */
    float fps;
    float infer_ms;
    float infer_p95;
    unsigned long long stream_frames;
    unsigned long long lut_updates;
} menu_state_t;

/* 把菜单渲染到已分配的 NV21 帧缓冲。
 *   y_plane  : Y 平面起始地址（虚拟地址，已加 head_size 偏移）
 *   uv_plane : UV 平面起始地址（紧跟 Y 平面之后）
 *   stride   : 行步长（字节），通常 = 1024（4:2:0 时 UV 同步长）
 *   ms       : 菜单当前状态
 * 函数保证只写 [0, 1024) x [0, 600) 范围内像素，不越界。 */
void menu_render_into(uint8_t *y_plane, uint8_t *uv_plane, int stride,
                      const menu_state_t *ms);

/* 触摸点击命中测试。返回 MENU_BTN_* 或 MENU_BTN_NONE。
 * 坐标为屏幕坐标（已归一化到 0..1023 / 0..599）。 */
int menu_hittest(int tx, int ty);

#endif /* SOCCHINA_MENU_RENDER_H */
