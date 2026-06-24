#ifndef SOCCHINA_APP_CONTROL_SOCK_H
#define SOCCHINA_APP_CONTROL_SOCK_H

#include "pipeline.h"

/* 应用控制 socket 上下文。control worker 每个周期调用 app_control_sock_poll()
 * 处理挂起的客户端请求。所有 ISP/NPU 状态变更由 control worker 串行应用，
 * socket 线程只负责解析和入队。 */

typedef struct app_control_sock app_control_sock_t;

/* 启动 Unix socket 监听，返回句柄。仅在 enable_stream 时创建。 */
app_control_sock_t *app_control_sock_start(const char *path);

/* 处理已连接客户端的挂起数据（非阻塞，每个控制周期调用一次）。
 * 必须在 control worker 线程中调用，因为需要读写 metrics 和控制参数。
 * params_dirty 在接受 set 命令后置 1，供调用者强制刷新 ISP。 */
void app_control_sock_poll(app_control_sock_t *s, pipeline_metrics_t *metrics,
                           float *tone_strength, float *nn_high_clip_guard,
                           int *nn_enabled, int *drc_mode, int *params_dirty,
                           int *enhancement_enabled, int *tone_enabled,
                           int *drc_strength, int *ldci_mode);

/* 停止 socket 并释放资源。 */
void app_control_sock_stop(app_control_sock_t *s);

#endif /* SOCCHINA_APP_CONTROL_SOCK_H */
