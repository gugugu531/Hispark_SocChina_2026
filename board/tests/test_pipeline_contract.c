#include "pipeline.h"

#include <stdio.h>

int main(void)
{
    pipeline_config_t cfg;

    pipeline_config_defaults(&cfg);
    cfg.use_wdr = 1;
    cfg.model_path = "cotf_paramnet_256x144_fp16_aipp.om";

    if (pipeline_config_validate(&cfg) != PIPELINE_OK || cfg.target_fps != 30 ||
        PIPELINE_VPSS_CHN_STREAM != 1 ||
        PIPELINE_VPSS_CHN_CONTROL != 2 || PIPELINE_CONTROL_WIDTH != 256u ||
        PIPELINE_CONTROL_HEIGHT != 144u ||
        PIPELINE_COTF_LUT_FLOAT_COUNT != 14739u ||
        PIPELINE_ISP_CLUT_NODE_COUNT != 5508u ||
        pipeline_state_name(PIPELINE_STATE_DEGRADED)[0] != 'D') {
        return 1;
    }
    cfg.target_fps = 4;
    if (pipeline_config_validate(&cfg) != PIPELINE_ERR_INVALID) {
        return 1;
    }
    cfg.target_fps = 30;
    cfg.enable_display = 0;
    cfg.enable_stream = 0;
    if (pipeline_config_validate(&cfg) != PIPELINE_ERR_INVALID) {
        return 1;
    }
    {
        pipeline_metrics_t metrics = {0};
        metrics.state = PIPELINE_STATE_RUNNING;
        metrics.lut_requests = 1;
        if (metrics.state != PIPELINE_STATE_RUNNING || metrics.lut_requests != 1) {
            return 1;
        }
    }
    printf("pipeline contract OK\n");
    return 0;
}
