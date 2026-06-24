#ifndef SOCCHINA_STREAM_H
#define SOCCHINA_STREAM_H

/* VENC + RTSP 边界。编码器拥有自己的 VPSS chn1 帧，不与显示线程共享借用帧。 */

typedef struct {
    unsigned width;
    unsigned height;
    unsigned fps;
    unsigned bitrate_kbps;
    unsigned rtsp_port;
    const char *stream_path;
    const char *rtsp_bind_addr;  /* NULL 或 "" 表示所有接口；否则为 loopback 地址 */
} stream_cfg_t;

int stream_init(const stream_cfg_t *cfg);

/* frame_info 实际为 const ot_video_frame_info*；同步送入 VENC，不保留借用帧。 */
int stream_send_frame(const void *frame_info, int timeout_ms);

int stream_deinit(void);

#endif /* SOCCHINA_STREAM_H */
