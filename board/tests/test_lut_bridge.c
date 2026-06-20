#include "lut_bridge.h"

#include <stdio.h>

int main(void)
{
    lut_bridge_cfg_t cfg;
    static uint32_t packed[PIPELINE_ISP_CLUT_NODE_COUNT];
    lut_bridge_default_cfg(&cfg);
    if (cfg.strength != 1.0f ||
        lut_bridge_make_identity(&cfg, packed, PIPELINE_ISP_CLUT_NODE_COUNT) != 0 ||
        packed[0] != 0u || packed[1] != (64u << 20) || packed[2] != (64u << 10) ||
        packed[3] != ((64u << 20) | (64u << 10))) {
        fprintf(stderr, "lut bridge identity layout mismatch\n");
        return 1;
    }
    puts("lut bridge 17v2 identity OK");
    return 0;
}
