#ifndef SOCCHINA_RTSP_SERVER_H
#define SOCCHINA_RTSP_SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef void (*rtsp_idr_callback_t)(void* opaque);

typedef struct {
    unsigned port;
    unsigned fps;
    const char* stream_path;
    const char* bind_addr;   /* NULL 或 "" 表示绑定所有接口；否则为 "127.0.0.1" 或 "::1" 等 */
    rtsp_idr_callback_t request_idr;
    void* request_idr_opaque;
} rtsp_server_cfg_t;

int rtsp_server_start(const rtsp_server_cfg_t* cfg);
int rtsp_server_publish_h264(const uint8_t* annexb, size_t len);
int rtsp_server_stop(void);

#endif /* SOCCHINA_RTSP_SERVER_H */
