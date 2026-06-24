#ifndef SOCCHINA_PIPELINE_H
#define SOCCHINA_PIPELINE_H

#include <stddef.h>
#include <stdint.h>

/* Config-R 的稳定跨模块约定。具体 SDK 帧类型继续由模块边界用 void* 隔离。 */

#define PIPELINE_VPSS_GRP 0

typedef enum {
    PIPELINE_VPSS_CHN_DISPLAY = 0, /* 1024x600，本地显示 */
    PIPELINE_VPSS_CHN_STREAM = 1,  /* 1024x576，RTSP */
    PIPELINE_VPSS_CHN_CONTROL = 2, /* 256x144，CoTF 参数网络 */
    PIPELINE_VPSS_CHN_COUNT = 3
} pipeline_vpss_channel_t;

#define PIPELINE_DISPLAY_WIDTH  1024u
#define PIPELINE_DISPLAY_HEIGHT 600u
#define PIPELINE_STREAM_WIDTH   1024u
#define PIPELINE_STREAM_HEIGHT  576u
#define PIPELINE_CONTROL_WIDTH  256u
#define PIPELINE_CONTROL_HEIGHT 144u

#define PIPELINE_TARGET_FPS 30u
#define PIPELINE_FRAME_TIMEOUT_MS 1000
#define PIPELINE_CONTROL_POLL_FRAMES 3u /* 30fps 时约 10Hz 读取统计 */
#define PIPELINE_EXIT_FATAL_RUNTIME 70  /* ACL/SMMU 上下文损坏，需要重启设备 */

#define PIPELINE_COTF_LUT_DIM 17u
#define PIPELINE_COTF_LUT_CHANNELS 3u
#define PIPELINE_COTF_LUT_FLOAT_COUNT \
    (PIPELINE_COTF_LUT_CHANNELS * PIPELINE_COTF_LUT_DIM * PIPELINE_COTF_LUT_DIM * \
     PIPELINE_COTF_LUT_DIM)
#define PIPELINE_ISP_CLUT_NODE_COUNT 5508u

typedef enum {
    PIPELINE_INPUT_COPY = 0,   /* 首版：NV21 缩略图复制到 ACL 输入缓冲，已知可实现 */
    PIPELINE_INPUT_PHYS_IMPORT /* 实验项：物理地址直接导入 ACL，验证前不得作为默认 */
} pipeline_input_mode_t;

typedef enum {
    PIPELINE_STATE_STOPPED = 0,
    PIPELINE_STATE_STARTING,
    PIPELINE_STATE_RUNNING,
    PIPELINE_STATE_DEGRADED, /* 相机显示继续，NN/CLUT 暂停 */
    PIPELINE_STATE_STOPPING,
    PIPELINE_STATE_FAILED
} pipeline_state_t;

typedef enum {
    PIPELINE_OK = 0,
    PIPELINE_ERR_INVALID = -1,
    PIPELINE_ERR_STATE = -2,
    PIPELINE_ERR_TIMEOUT = -3,
    PIPELINE_ERR_IO = -4,
    PIPELINE_ERR_RUNTIME = -5
} pipeline_status_t;

typedef struct {
    int sensor_index;
    int use_wdr;
    unsigned target_fps;
    const char *model_path;
    pipeline_input_mode_t input_mode;
    int enable_display;
    int enable_stream;
    unsigned control_poll_frames;
    float tone_strength;
    float nn_high_clip_guard;
    unsigned    stream_bitrate_kbps;
    unsigned    rtsp_port;
    const char *stream_path;
    const char *rtsp_bind_addr;  /* NULL = 所有接口; "127.0.0.1" = 仅回环（MediaMTX 模式） */
    const char *ctrl_sock_path;  /* NULL = 不开启控制 socket; e.g. "/run/socchina/app-control.sock" */
} pipeline_config_t;

typedef struct {
    pipeline_state_t state;
    uint64_t display_frames;
    uint64_t display_drops;
    uint64_t stream_frames;
    uint64_t stream_drops;
    uint64_t control_polls;
    uint64_t frame_timeouts;
    uint64_t infer_runs;
    uint64_t lut_requests;
    uint64_t lut_updates;
    uint64_t lut_failures;
    uint64_t transient_errors;
    uint64_t fatal_errors;
    uint64_t degraded_events;
    float display_fps;
    float infer_last_ms;
    float infer_max_ms;
    float infer_total_last_ms;
    float infer_total_max_ms;
    float transaction_last_ms;
    float transaction_max_ms;
    float infer_p95_ms;
    float transaction_p95_ms;
    unsigned timing_samples;
} pipeline_metrics_t;

void pipeline_config_defaults(pipeline_config_t *cfg);
int pipeline_config_validate(const pipeline_config_t *cfg);
const char *pipeline_state_name(pipeline_state_t state);

/*
 * 借用帧规则：
 * 1. vpss_get_frame 成功后，调用者只借用 frame_info。
 * 2. 必须由同一 grp/chn 调用 vpss_release_frame，且恰好一次。
 * 3. display_send_frame / infer_run_nv21 必须在 release 前同步返回，不能保存 frame_info。
 * 4. 禁止跨线程转交借用帧；需要异步时复制缩略图或使用独立 VB 所有权机制。
 */

_Static_assert(PIPELINE_VPSS_CHN_COUNT == 3, "VPSS channel contract changed");
_Static_assert(PIPELINE_COTF_LUT_FLOAT_COUNT == 14739u, "unexpected CoTF LUT size");
_Static_assert(PIPELINE_ISP_CLUT_NODE_COUNT == 5508u, "unexpected ISP CLUT size");

#endif /* SOCCHINA_PIPELINE_H */
