#include "lut_bridge.h"

#include <math.h>
#include <string.h>

#define LUT_DIM 17u
#define LUT_MAX 1023u
#define LUT_BANKS 8u
#define LUT_BANK_CAPACITY 729u

static const unsigned g_bank_counts[LUT_BANKS] = {729, 648, 648, 576, 648, 576, 576, 512};

static size_t cubic_index(unsigned channel, unsigned r, unsigned g, unsigned b)
{
    return (((size_t)channel * LUT_DIM + r) * LUT_DIM + g) * LUT_DIM + b;
}

static uint32_t pack_rgb(const float rgb[3])
{
    uint32_t r = (uint32_t)(rgb[0] * LUT_MAX + 0.5f);
    uint32_t g = (uint32_t)(rgb[1] * LUT_MAX + 0.5f);
    uint32_t b = (uint32_t)(rgb[2] * LUT_MAX + 0.5f);
    return (r << 20) | (g << 10) | b;
}

void lut_bridge_default_cfg(lut_bridge_cfg_t *cfg)
{
    if (cfg == NULL) return;
    cfg->strength = 1.0f;
}

int lut_bridge_pack(const lut_bridge_cfg_t *cfg, const float *cubic_lut, size_t cubic_count,
                    uint32_t *packed_out, size_t packed_count)
{
    static float banks[LUT_BANKS][LUT_BANK_CAPACITY][3];
    unsigned positions[LUT_BANKS] = {0};
    unsigned r, g, b, c, bank, i;
    size_t out = 0;
    float strength;

    if (cfg == NULL || cubic_lut == NULL || packed_out == NULL ||
        cubic_count != PIPELINE_COTF_LUT_FLOAT_COUNT ||
        packed_count != PIPELINE_ISP_CLUT_NODE_COUNT) {
        return -1;
    }
    strength = cfg->strength;
    if (!isfinite(strength) || strength < 0.0f || strength > 1.0f) return -1;
    memset(banks, 0, sizeof(banks));

    /* 与 PQTools 17v2 一致：B 外层、G 中层、R 内层，按三轴奇偶拆成 8 bank。 */
    for (b = 0; b < LUT_DIM; b++) {
        for (g = 0; g < LUT_DIM; g++) {
            for (r = 0; r < LUT_DIM; r++) {
                bank = (r & 1u) | ((g & 1u) << 1) | ((b & 1u) << 2);
                i = positions[bank]++;
                if (i >= g_bank_counts[bank]) return -1;
                for (c = 0; c < 3; c++) {
                    float raw = cubic_lut[cubic_index(c, r, g, b)];
                    float identity = (c == 0) ? (float)r / 16.0f
                                             : ((c == 1) ? (float)g / 16.0f
                                                         : (float)b / 16.0f);
                    float value;
                    if (!isfinite(raw) || raw < -0.05f || raw > 1.05f) return -1;
                    if (raw < 0.0f) raw = 0.0f;
                    if (raw > 1.0f) raw = 1.0f;
                    value = identity + strength * (raw - identity);
                    if (value < 0.0f) value = 0.0f;
                    if (value > 1.0f) value = 1.0f;
                    banks[bank][i][c] = value;
                }
            }
        }
    }
    for (bank = 0; bank < LUT_BANKS; bank++) {
        if (positions[bank] != g_bank_counts[bank]) return -1;
    }

    /* 精确复现 17v2 的 4 路交织；bank2/6 的尾部连续读取下一 bank。 */
    for (i = 0; i < 729; i++) {
        packed_out[out++] = pack_rgb(banks[0][i]);
        packed_out[out++] = (i < g_bank_counts[1]) ? pack_rgb(banks[1][i]) : 0;
        packed_out[out++] = (i < g_bank_counts[2]) ? pack_rgb(banks[2][i])
                                                   : pack_rgb(banks[3][i - g_bank_counts[2]]);
        packed_out[out++] = (i < g_bank_counts[3]) ? pack_rgb(banks[3][i]) : 0;
    }
    for (i = 0; i < 648; i++) {
        packed_out[out++] = pack_rgb(banks[4][i]);
        packed_out[out++] = (i < g_bank_counts[5]) ? pack_rgb(banks[5][i]) : 0;
        packed_out[out++] = (i < g_bank_counts[6]) ? pack_rgb(banks[6][i])
                                                   : pack_rgb(banks[7][i - g_bank_counts[6]]);
        packed_out[out++] = (i < g_bank_counts[7]) ? pack_rgb(banks[7][i]) : 0;
    }
    return out == PIPELINE_ISP_CLUT_NODE_COUNT ? 0 : -1;
}

int lut_bridge_make_identity(const lut_bridge_cfg_t *cfg, uint32_t *packed_out,
                             size_t packed_count)
{
    static float identity[PIPELINE_COTF_LUT_FLOAT_COUNT];
    lut_bridge_cfg_t full = {1.0f};
    unsigned r, g, b;
    (void)cfg;
    for (r = 0; r < LUT_DIM; r++) {
        for (g = 0; g < LUT_DIM; g++) {
            for (b = 0; b < LUT_DIM; b++) {
                identity[cubic_index(0, r, g, b)] = (float)r / 16.0f;
                identity[cubic_index(1, r, g, b)] = (float)g / 16.0f;
                identity[cubic_index(2, r, g, b)] = (float)b / 16.0f;
            }
        }
    }
    return lut_bridge_pack(&full, identity, PIPELINE_COTF_LUT_FLOAT_COUNT, packed_out,
                           packed_count);
}
