#include "pipeline.h"

#include <stdio.h>

int main(void)
{
    pipeline_config_t cfg = {
        .sensor_index = 1,
        .use_wdr = 1,
        .target_fps = PIPELINE_TARGET_FPS,
        .model_path = "cotf_paramnet_256x144_fp16_aipp.om",
        .input_mode = PIPELINE_INPUT_COPY,
        .enable_display = 1,
        .enable_stream = 0,
        .control_poll_frames = PIPELINE_CONTROL_POLL_FRAMES,
    };

    if (cfg.target_fps != 30 || PIPELINE_VPSS_CHN_STREAM != 1 ||
        PIPELINE_VPSS_CHN_CONTROL != 2 || PIPELINE_CONTROL_WIDTH != 256u ||
        PIPELINE_CONTROL_HEIGHT != 144u ||
        PIPELINE_COTF_LUT_FLOAT_COUNT != 14739u ||
        PIPELINE_ISP_CLUT_NODE_COUNT != 5508u) {
        return 1;
    }
    printf("pipeline contract OK\n");
    return 0;
}
