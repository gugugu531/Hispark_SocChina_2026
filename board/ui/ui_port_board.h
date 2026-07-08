#ifndef SOCCHINA_UI_PORT_BOARD_H
#define SOCCHINA_UI_PORT_BOARD_H

/* ui_port_board — board/ui 的板端 LVGL port(方案 C:离屏渲染 + 合成进视频帧)。
 *
 * 背景:方案 A(GFBG G0 图形层叠加,/dev/fb0)经板端逐层实测证伪——fb0 被 gfbg.ko
 * 钉死 3840x2160 ARGB1555 且硬件缩放+双缓冲,userspace 线性 mmap 写入喂不对其显示
 * buffer(结构化内容必现扫描线),运行时几何/格式改不动,FBIO_REFRESH 返回 EPERM。
 * 详见项目记忆 lvgl-gfbg-fb0-fixed-3840。
 *
 * 方案 C:LVGL 离屏渲染到一块 1024x600 ARGB8888 内存缓冲(自定义 flush_cb,不经 fbdev);
 * 显示循环(main.c)在 display_send_frame 之前调用 ui_port_board_composite_nv21(),把 UI
 * 逐像素按 alpha 合成进既有 VI→ISP→VPSS→VO→HDMI 视频通路的 NV21 帧(该通路在 1024x600
 * NV21 下已验证干净)。native 1024x600 上屏,无缩放/无 ARGB1555/无双缓冲扫描线。
 *
 * 线程模型:
 *   - worker 线程跑 LVGL(lv_timer_handler + 周期 ui_lvgl_update ~5Hz),flush_cb 把渲染结果
 *     拷进受锁保护的共享 ARGB 缓冲并 bump 版本号;
 *   - 显示线程(main.c)按帧调用 composite:仅当版本变化时把共享 ARGB 重烘焙为 NV21+alpha
 *     掩码(缓存),每帧只做掩码合成。
 *
 * 命令:UI 改参数 → 映射为 app_ctrl_params_t → app_ctrl_push(与 socket/旧菜单同队列)。
 * 状态:worker 周期回调 snapshot_fn 取一份 ui_state_t 喂给 ui_lvgl_update。
 */

#include <stdint.h>

#include "ui_lvgl.h"

/* 由 main.c 提供:填充一份实时状态快照(读 metrics/control,线程安全)。 */
typedef void (*ui_snapshot_fn)(ui_state_t *out, void *user);

/* 启动板端 LVGL worker(离屏 display + evdev 触摸 + 渲染线程)。须在 display_init()
 * 之后调用(合成写入的是 VPSS→VO 视频通路帧)。
 *   fb_dev    : 方案 C 不再使用(保留形参以兼容既有调用);传任意值均被忽略。
 *   touch_dev : 触摸设备,如 "/dev/input/event0"
 * 返回 0 成功。 */
int  ui_port_board_start(const char *fb_dev, const char *touch_dev,
                         ui_snapshot_fn snap, void *user);

/* 停止 worker 并释放(阻塞至线程退出)。 */
void ui_port_board_stop(void);

/* 是否已就绪可合成(worker 运行中且已产出至少一帧 UI)。显示循环用它决定是否
 * 为本帧做 mmap+合成,避免 UI 未起时的无谓开销。 */
int  ui_port_board_active(void);

/* 把最新 UI 帧按 per-pixel alpha 合成进一块已 mmap 的 NV21(YVU420SP)帧。
 *   y   : Y 平面虚拟地址
 *   uv  : 交织 VU 平面虚拟地址(NV21:偶字节=V,奇字节=U)
 *   stride : 帧真实行距(字节,Y 与 UV 平面同 stride;非 width)
 *   w,h : 帧宽高(应为 1024x600)
 * alpha 语义:a==0 保留视频不写;a==255 覆盖为 UI 颜色;0<a<255 与底层视频混合。
 * 线程安全:内部对共享 ARGB 取锁快照,仅显示线程调用。 */
void ui_port_board_composite_nv21(uint8_t *y, uint8_t *uv, int stride, int w, int h);

#endif /* SOCCHINA_UI_PORT_BOARD_H */
