#define _POSIX_C_SOURCE 200809L

#include "touch.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
    int fd;
    int disp_w, disp_h;
    int axis_x_min, axis_x_range;
    int axis_y_min, axis_y_range;
    /* 当前待提交的原始坐标 */
    int raw_x, raw_y, pressed;
    int pending;  /* 1 = SYN 到达，待取 */
} touch_state_t;

static touch_state_t g_ts;

int touch_open(const char *dev, int display_w, int display_h)
{
    struct input_absinfo ax, ay;

    int fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[touch] open %s: %s\n", dev, strerror(errno));
        return -1;
    }

    memset(&g_ts, 0, sizeof(g_ts));
    g_ts.fd      = fd;
    g_ts.disp_w  = display_w;
    g_ts.disp_h  = display_h;

    /* 读取轴范围（EVIOCGABS），用于坐标归一化 */
    if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 && ax.maximum > ax.minimum) {
        g_ts.axis_x_min   = ax.minimum;
        g_ts.axis_x_range = ax.maximum - ax.minimum;
    } else {
        g_ts.axis_x_min   = 0;
        g_ts.axis_x_range = display_w - 1;
    }
    if (ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0 && ay.maximum > ay.minimum) {
        g_ts.axis_y_min   = ay.minimum;
        g_ts.axis_y_range = ay.maximum - ay.minimum;
    } else {
        g_ts.axis_y_min   = 0;
        g_ts.axis_y_range = display_h - 1;
    }

    g_ts.raw_x   = display_w / 2;
    g_ts.raw_y   = display_h / 2;
    g_ts.pressed = 0;
    g_ts.pending = 0;
    return fd;
}

/* 将原始轴值映射到屏幕坐标 */
static int map_axis(int raw, int raw_min, int raw_range, int screen_size)
{
    if (raw_range <= 0) return raw;
    int v = (int)(((long)(raw - raw_min) * screen_size) / raw_range);
    if (v < 0) v = 0;
    if (v >= screen_size) v = screen_size - 1;
    return v;
}

int touch_poll(int fd, touch_event_t *ev)
{
    struct input_event ie;

    while (1) {
        ssize_t n = read(fd, &ie, sizeof(ie));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        if (n < (ssize_t)sizeof(ie)) break;

        switch (ie.type) {
        case EV_ABS:
            if (ie.code == ABS_X || ie.code == ABS_MT_POSITION_X)
                g_ts.raw_x = ie.value;
            else if (ie.code == ABS_Y || ie.code == ABS_MT_POSITION_Y)
                g_ts.raw_y = ie.value;
            break;
        case EV_KEY:
            if (ie.code == BTN_TOUCH || ie.code == BTN_LEFT)
                g_ts.pressed = (ie.value != 0);
            break;
        case EV_SYN:
            if (ie.code == SYN_REPORT) {
                g_ts.pending = 1;
            }
            break;
        default:
            break;
        }

        if (g_ts.pending) {
            g_ts.pending = 0;
            ev->x       = map_axis(g_ts.raw_x, g_ts.axis_x_min, g_ts.axis_x_range, g_ts.disp_w);
            ev->y       = map_axis(g_ts.raw_y, g_ts.axis_y_min, g_ts.axis_y_range, g_ts.disp_h);
            ev->pressed = g_ts.pressed;
            return 1;
        }
    }
    return 0;
}

void touch_close(int fd)
{
    if (fd >= 0) close(fd);
}
