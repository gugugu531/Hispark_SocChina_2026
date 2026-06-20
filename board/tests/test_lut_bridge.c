#include "lut_bridge.h"

#include <math.h>
#include <stdio.h>

int main(void)
{
    lut_bridge_cfg_t cfg;
    static uint32_t packed[PIPELINE_ISP_CLUT_NODE_COUNT];
    static uint32_t candidate[PIPELINE_ISP_CLUT_NODE_COUNT];
    static float invalid[PIPELINE_COTF_LUT_FLOAT_COUNT];
    lut_bridge_default_cfg(&cfg);
    if (cfg.strength != 1.0f ||
        lut_bridge_make_identity(&cfg, packed, PIPELINE_ISP_CLUT_NODE_COUNT) != 0 ||
        packed[0] != 0u || packed[1] != (64u << 20) || packed[2] != (64u << 10) ||
        packed[3] != ((64u << 20) | (64u << 10))) {
        fprintf(stderr, "lut bridge identity layout mismatch\n");
        return 1;
    }
    for (size_t i = 0; i < PIPELINE_ISP_CLUT_NODE_COUNT; i++) candidate[i] = 0x3FFFFFFFu;
    if (lut_bridge_limit_packed_step(packed, candidate, PIPELINE_ISP_CLUT_NODE_COUNT, 16) != 0 ||
        ((candidate[0] >> 20) & 0x3FFu) > 16u || ((candidate[0] >> 10) & 0x3FFu) > 16u ||
        (candidate[0] & 0x3FFu) > 16u) {
        fprintf(stderr, "lut bridge temporal step guard mismatch\n");
        return 1;
    }
    invalid[0] = NAN;
    if (lut_bridge_pack(&cfg, invalid, PIPELINE_COTF_LUT_FLOAT_COUNT, candidate,
                        PIPELINE_ISP_CLUT_NODE_COUNT) == 0) {
        fprintf(stderr, "lut bridge accepted invalid model output\n");
        return 1;
    }
    puts("lut bridge 17v2 identity OK");
    return 0;
}
