#ifndef SOCCHINA_STREAM_H
#define SOCCHINA_STREAM_H

/* VENC + RTSP 边界。编码器拥有自己的 VPSS chn1 帧，不与显示线程共享借用帧。 */

typedef struct {
    unsigned    width;
    unsigned    height;
    unsigned    fps;
    unsigned    bitrate_kbps;
    unsigned    rtsp_port;
    const char *stream_path;
    const char *rtsp_bind_addr; /* NULL = 所有接口 ([::]); "127.0.0.1" = 仅回环（MediaMTX 模式） */
} stream_cfg_t;

int stream_init(const stream_cfg_t *cfg);

/* frame_info 实际为 const ot_video_frame_info*；同步送入 VENC，不保留借用帧。 */
int stream_send_frame(const void *frame_info, int timeout_ms);

/* 运行时修改 H.264 CBR 目标码率（kbps），不重启编码通道/视频流。
 * 返回 0 成功；-1 失败（未初始化、越界、或 SDK 调用失败）。值不变时为 no-op。 */
int stream_set_bitrate(unsigned kbps);

int stream_deinit(void);

#endif /* SOCCHINA_STREAM_H */
