#ifndef SOCCHINA_APP_CONTROL_H
#define SOCCHINA_APP_CONTROL_H

#include <stddef.h>

/*
 * Unix socket 控制接口：接收来自 socchina-web 的 JSON 行命令，分发给 control_worker。
 * SDK-free，可主机单测。
 *
 * 协议（一行一个 JSON，以 \n 分隔）：
 *   请求: {"id":N,"op":"status"}
 *         {"id":N,"op":"set","tone_strength":0.25,"nn_clut_enabled":true,
 *                 "nn_high_clip_guard":3.0,"enhancement_enabled":true,"tone_enabled":true,
 *                 "drc_mode":"auto","drc_strength":512,"ldci_mode":"auto"}
 *   响应: {"id":N,"ok":true,"pipeline":{...},"processing":{...},"outputs":{...}}
 *         {"id":N,"ok":true,"queued":true}
 *         {"id":N,"ok":false,"error":"..."}
 *
 * 同一参数的新值覆盖尚未被 control_worker 消费的旧值（last-write-wins per field）。
 * 状态响应载荷由 main.c 的 status_fn 产出，键名与 web 控制台契约一致。
 */

typedef struct {
    int   has_tone_strength;
    float tone_strength;          /* 0.0 .. 1.0 */
    int   has_nn_clut_enabled;
    int   nn_clut_enabled;
    int   has_high_clip_guard;
    float high_clip_guard;        /* 0.0 .. 100.0（高光裁剪百分比门限） */
    int   has_enhancement_enabled;
    int   enhancement_enabled;    /* 全局增强总开关 */
    int   has_tone_enabled;
    int   tone_enabled;           /* 规则 Gamma 色调开关 */
    int   has_drc_mode;
    int   drc_mode;               /* 0 off / 1 auto / 2 manual */
    int   has_drc_strength;
    int   drc_strength;           /* 0 .. 1023 */
    int   has_ldci_mode;
    int   ldci_mode;              /* 0 off / 1 auto */
    int   has_load_isp;           /* 从文件加载完整 ISP 参数 */
    char  isp_blob_path[256];     /* 二进制 ISP 参数文件路径 */
} app_ctrl_params_t;

/* 由 main.c 提供：将当前 pipeline 状态序列化为 JSON 对象成员列表写入 buf，
 * 不含最外层花括号（如 "pipeline":{...},"processing":{...},"outputs":{...}）；
 * 由 handle_client 拼进 {"id":N,"ok":true,<此处>}。buflen > 0。
 * 必须线程安全（从 socket 线程调用）。 */
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
