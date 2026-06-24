#ifndef SOCCHINA_TOUCH_H
#define SOCCHINA_TOUCH_H

/* touch — Linux evdev 单点触摸读取（/dev/input/eventN）。
 * SDK-free；可在主机单独编译。
 * 调用方在 display_worker 里非阻塞轮询 touch_poll()，不需要独立线程。 */

typedef struct {
    int x;        /* 屏幕坐标，已映射到 [0, DISPLAY_WIDTH) */
    int y;        /* 屏幕坐标，已映射到 [0, DISPLAY_HEIGHT) */
    int pressed;  /* 1=按下/滑动，0=抬起 */
} touch_event_t;

/* 打开触摸设备。成功返回 >=0 的 fd，失败返回 -1。
 * display_w/h：屏幕分辨率（用于坐标归一化，axis range → screen）。 */
int touch_open(const char *dev, int display_w, int display_h);

/* 非阻塞读一次触摸事件。返回 1 表示有新事件写入 *ev；返回 0 表示无事件；返回 -1 表示出错。
 * 每次调用只返回至多一个逻辑触摸事件（SYN 边界对齐）。 */
int touch_poll(int fd, touch_event_t *ev);

/* 关闭设备 fd。 */
void touch_close(int fd);

#endif /* SOCCHINA_TOUCH_H */
