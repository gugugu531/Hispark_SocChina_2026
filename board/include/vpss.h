#ifndef SOCCHINA_VPSS_H
#define SOCCHINA_VPSS_H

/* vpss — 多路硬件缩放分发（数据通路第 5 级）。
 *
 * 架构约定（architecture.md §2）：
 *   chn0 → 1024x600 显示底图；chn1 → 1024x576 模型输入；chn2 → 320x180 缩略图。
 * 本模块按配置启用通道；VI ONLINE 模式下 CPU 取帧必须经 VPSS 通道
 * （chn 配置 depth>0 才能 vpss_get_frame）。
 *
 * 前置：SYS/VB 已起；输入侧由 capture_bind_vpss 绑定。
 * 当前实现一次只支持一个 VPSS 组（管线仅用 grp0）。
 */

#define VPSS_CHN_COUNT 3

typedef struct {
    int enable;      /* 0 关闭该通道 */
    unsigned width;  /* 输出宽 */
    unsigned height; /* 输出高 */
    unsigned depth;  /* >0 允许 CPU 取帧（队列深度） */
} vpss_chn_cfg_t;

/* 启动 VPSS 组与使能的通道（NV21 输出，无压缩）。max_w/max_h 为输入全幅尺寸。 */
int vpss_init(int grp, unsigned max_width, unsigned max_height,
              const vpss_chn_cfg_t chn_cfg[VPSS_CHN_COUNT]);

/* 取/还一帧（零拷贝，frame_info 实际为 ot_video_frame_info*，与 display_send_frame
 * 直接兼容）。timeout_ms：<0 阻塞，>=0 毫秒超时。成功返回 0。 */
int vpss_get_frame(int grp, int chn, void *frame_info, int timeout_ms);
int vpss_release_frame(int grp, int chn, const void *frame_info);

/* 停组与通道。可重复调用。 */
int vpss_deinit(int grp);

#endif /* SOCCHINA_VPSS_H */
