#ifndef SOCCHINA_APP_CONTROL_H
#define SOCCHINA_APP_CONTROL_H

#include <stddef.h>

/*
 * Unix socket 控制接口：接收来自 socchina-web 的 JSON 行命令，分发给 control_worker。
 * SDK-free，可主机单测。
 *
 * 协议（一行一个 JSON，以 \n 分隔）：
 *   请求: {"id":N,"op":"status"}
 *         {"id":N,"op":"set","params":{"tone_strength":0.25,"nn_clut_enabled":true}}
 *   响应: {"id":N,"ok":true,"status":{...}}
 *         {"id":N,"ok":true,"queued":true}
 *         {"id":N,"ok":false,"error":"..."}
 *
 * 同一参数的新值覆盖尚未被 control_worker 消费的旧值（last-write-wins per field）。
 */

typedef struct {
    int   has_tone_strength;
    float tone_strength;       /* 0.0 .. 1.0 */
    int   has_nn_clut_enabled;
    int   nn_clut_enabled;
    int   has_high_clip_guard;
    float high_clip_guard;     /* > 0.0 */
} app_ctrl_params_t;

/* 由 main.c 提供：将当前 pipeline 状态序列化为完整 JSON 对象（含外层 {}）写入 buf。
 * buflen > 0。必须线程安全（从 socket 线程调用）。 */
typedef void (*app_ctrl_status_fn)(void *opaque, char *buf, size_t buflen);

typedef struct {
    const char          *sock_path;
    app_ctrl_status_fn   status_fn;
    void                *status_opaque;
} app_ctrl_cfg_t;

/* 初始化内部状态（不创建 socket）。 */
int   app_ctrl_init(const app_ctrl_cfg_t *cfg);

/* pthread 入口：创建并监听 socket，每次处理一个客户端连接，直到 app_ctrl_stop()。 */
void *app_ctrl_worker(void *arg);

/* 由 control_worker 在每个轮询周期头部调用：取出待应用的参数并清空挂起槽。
 * 返回 1 表示 *out 已填充，0 表示无挂起更新。 */
int   app_ctrl_drain(app_ctrl_params_t *out);

/* 请求 worker 线程退出（线程安全），最多等待一个 poll 周期（~200ms）。 */
void  app_ctrl_stop(void);

/* 释放资源（须在 worker 线程已退出后调用）。 */
void  app_ctrl_deinit(void);

/* 进程内直接推送参数（由触摸菜单调用，last-write-wins）。
 * 线程安全；不需要 socket，效果与 op:set 相同。 */
void  app_ctrl_push(const app_ctrl_params_t *p);

#endif /* SOCCHINA_APP_CONTROL_H */
