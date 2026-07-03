#ifndef SOCCHINA_UI_PORT_BOARD_H
#define SOCCHINA_UI_PORT_BOARD_H

/* ui_port_board — board/ui 的板端 LVGL port(方案 A:GFBG G0 图形层叠加)。
 *
 * 职责:配置 GFBG 图形层 G0(ARGB8888 / 双缓冲 / 抗闪烁 / 像素 alpha)→
 *       lv_linux_fbdev 指向 /dev/fbN → lv_evdev 接触摸 → 起独立 worker 线程
 *       跑 LVGL 循环。UI 本体见 ui_lvgl.{c,h},与本 port 解耦。
 *
 * G0 是独立硬件图形层,由 VO 硬件叠加在视频层之上,因此本 worker 与
 * main.c 的 VPSS→视频层显示循环互不干扰(前提:display_init 已使能 VO 设备层)。
 *
 * 命令:UI 改参数 → 映射为 app_ctrl_params_t → app_ctrl_push(与 socket/旧菜单同队列)。
 * 状态:worker 周期回调 snapshot_fn 取一份 ui_state_t 喂给 ui_lvgl_update。
 */

#include "ui_lvgl.h"

/* 由 main.c 提供:填充一份实时状态快照(读 metrics/control,线程安全)。 */
typedef void (*ui_snapshot_fn)(ui_state_t *out, void *user);

/* 启动板端 LVGL worker。须在 display_init() 之后调用(依赖 VO 设备层已使能)。
 *   fb_dev    : 图形层设备,如 "/dev/fb0"(G0)
 *   touch_dev : 触摸设备,如 "/dev/input/event0"
 * 返回 0 成功。 */
int  ui_port_board_start(const char *fb_dev, const char *touch_dev,
                         ui_snapshot_fn snap, void *user);

/* 停止 worker 并释放(阻塞至线程退出)。 */
void ui_port_board_stop(void);

#endif /* SOCCHINA_UI_PORT_BOARD_H */
