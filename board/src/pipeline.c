#include "pipeline.h"

#include <math.h>
#include <string.h>

#define PIPELINE_DEFAULT_SENSOR_INDEX      1
#define PIPELINE_DEFAULT_TONE_STRENGTH     0.25f
#define PIPELINE_DEFAULT_HIGH_CLIP_GUARD   3.0f
#define PIPELINE_DEFAULT_BITRATE_KBPS      3000u
#define PIPELINE_DEFAULT_RTSP_PORT         8554u
#define PIPELINE_DEFAULT_RTSP_PATH         "live"

void pipeline_config_defaults(pipeline_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->sensor_index = PIPELINE_DEFAULT_SENSOR_INDEX;
    cfg->target_fps = PIPELINE_TARGET_FPS;
    cfg->input_mode = PIPELINE_INPUT_COPY;
    cfg->enable_display = 1;
    cfg->control_poll_frames = PIPELINE_CONTROL_POLL_FRAMES;
    cfg->tone_strength = PIPELINE_DEFAULT_TONE_STRENGTH;
    cfg->nn_high_clip_guard = PIPELINE_DEFAULT_HIGH_CLIP_GUARD;
    cfg->stream_bitrate_kbps = PIPELINE_DEFAULT_BITRATE_KBPS;
    cfg->rtsp_port      = PIPELINE_DEFAULT_RTSP_PORT;
    cfg->stream_path    = PIPELINE_DEFAULT_RTSP_PATH;
    cfg->rtsp_bind_addr = NULL; /* 默认：所有接口 */
    cfg->ctrl_sock_path = NULL; /* 默认：不开启控制 socket */
    cfg->ctbg_est_om_path = "/root/socchina-2026/ctbg6ch_estimator_256x144.om";
    /* v8 AIPP OM：NV21 输入，硬件 AIPP 做 NV21→RGB fp16，内建 ConvTranspose */
    cfg->ctbg_app_om_path = "/root/socchina-2026/ctbg_apply_twostage_nn_aipp_1024x576.om";
}

int pipeline_config_validate(const pipeline_config_t *cfg)
{
    const char *p;

    if (cfg == NULL || cfg->sensor_index < 0 || cfg->sensor_index > 1) {
        return PIPELINE_ERR_INVALID;
    }
    if ((!cfg->use_wdr && (cfg->target_fps < 1u || cfg->target_fps > 30u)) ||
        (cfg->use_wdr && (cfg->target_fps < 5u || cfg->target_fps > 30u))) {
        return PIPELINE_ERR_INVALID;
    }
    if (!cfg->enable_display && !cfg->enable_stream) {
        return PIPELINE_ERR_INVALID;
    }
    if (cfg->input_mode != PIPELINE_INPUT_COPY &&
        cfg->input_mode != PIPELINE_INPUT_PHYS_IMPORT) {
        return PIPELINE_ERR_INVALID;
    }
    if (cfg->control_poll_frames == 0u || !isfinite(cfg->tone_strength) ||
        !isfinite(cfg->nn_high_clip_guard) || cfg->tone_strength < 0.0f ||
        cfg->tone_strength > 1.0f || cfg->nn_high_clip_guard < 0.0f ||
        cfg->nn_high_clip_guard > 100.0f) {
        return PIPELINE_ERR_INVALID;
    }
    if (cfg->enable_stream &&
        (cfg->stream_bitrate_kbps < 128u || cfg->rtsp_port == 0u ||
         cfg->rtsp_port > 65535u || cfg->stream_path == NULL ||
         cfg->stream_path[0] == '\0')) {
        return PIPELINE_ERR_INVALID;
    }
    if (cfg->stream_path != NULL) {
        for (p = cfg->stream_path; *p != '\0'; ++p) {
            int valid = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                        (*p >= '0' && *p <= '9') || strchr("._~-", *p) != NULL;
            if (!valid) {
                return PIPELINE_ERR_INVALID;
            }
        }
    }
    return PIPELINE_OK;
}

const char *pipeline_state_name(pipeline_state_t state)
{
    switch (state) {
        case PIPELINE_STATE_STOPPED:
            return "STOPPED";
        case PIPELINE_STATE_STARTING:
            return "STARTING";
        case PIPELINE_STATE_RUNNING:
            return "RUNNING";
        case PIPELINE_STATE_DEGRADED:
            return "DEGRADED";
        case PIPELINE_STATE_STOPPING:
            return "STOPPING";
        case PIPELINE_STATE_FAILED:
            return "FAILED";
        default:
            return "UNKNOWN";
    }
}
