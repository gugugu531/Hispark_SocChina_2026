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
    cfg->max_identity_delta = 0.25f;
    cfg->highlight_boost_limit = 0.02f;
    cfg->preserve_cube_endpoints = 1;
    cfg->enforce_axis_monotonicity = 1;
}

int lut_bridge_pack(const lut_bridge_cfg_t *cfg, const float *cubic_lut, size_t cubic_count,
                    uint32_t *packed_out, size_t packed_count)
{
    static float banks[LUT_BANKS][LUT_BANK_CAPACITY][3];
    static float safe[3][LUT_DIM][LUT_DIM][LUT_DIM];
    unsigned positions[LUT_BANKS] = {0};
    unsigned r, g, b, c, bank, i;
    size_t out = 0;
    float strength, max_delta, highlight_boost;

    if (cfg == NULL || cubic_lut == NULL || packed_out == NULL ||
        cubic_count != PIPELINE_COTF_LUT_FLOAT_COUNT ||
        packed_count != PIPELINE_ISP_CLUT_NODE_COUNT) {
        return -1;
    }
    strength = cfg->strength;
    max_delta = cfg->max_identity_delta;
    highlight_boost = cfg->highlight_boost_limit;
    if (!isfinite(strength) || strength < 0.0f || strength > 1.0f ||
        !isfinite(max_delta) || max_delta < 0.0f || max_delta > 1.0f ||
        !isfinite(highlight_boost) || highlight_boost < 0.0f || highlight_boost > 1.0f) {
        return -1;
    }
    memset(banks, 0, sizeof(banks));
    memset(safe, 0, sizeof(safe));

    /* 第一阶段：有限值、identity 混合、最大偏移、高光与立方体端点保护。 */
    for (b = 0; b < LUT_DIM; b++) {
        for (g = 0; g < LUT_DIM; g++) {
            for (r = 0; r < LUT_DIM; r++) {
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
                    if (value > identity + max_delta) value = identity + max_delta;
                    if (value < identity - max_delta) value = identity - max_delta;
                    if (identity >= 0.875f && value > identity + highlight_boost) {
                        value = identity + highlight_boost;
                    }
                    if (cfg->preserve_cube_endpoints &&
                        (r == 0 || r == LUT_DIM - 1) && (g == 0 || g == LUT_DIM - 1) &&
                        (b == 0 || b == LUT_DIM - 1)) {
                        value = identity;
                    }
                    if (value < 0.0f) value = 0.0f;
                    if (value > 1.0f) value = 1.0f;
                    safe[c][r][g][b] = value;
                }
            }
        }
    }

    /* 第二阶段：每个输出通道沿对应输入主轴做累计最大，阻止颜色反转/banding。 */
    if (cfg->enforce_axis_monotonicity) {
        for (g = 0; g < LUT_DIM; g++) {
            for (b = 0; b < LUT_DIM; b++) {
                for (r = 1; r < LUT_DIM; r++) {
                    if (safe[0][r][g][b] < safe[0][r - 1][g][b])
                        safe[0][r][g][b] = safe[0][r - 1][g][b];
                }
            }
        }
        for (r = 0; r < LUT_DIM; r++) {
            for (b = 0; b < LUT_DIM; b++) {
                for (g = 1; g < LUT_DIM; g++) {
                    if (safe[1][r][g][b] < safe[1][r][g - 1][b])
                        safe[1][r][g][b] = safe[1][r][g - 1][b];
                }
            }
        }
        for (r = 0; r < LUT_DIM; r++) {
            for (g = 0; g < LUT_DIM; g++) {
                for (b = 1; b < LUT_DIM; b++) {
                    if (safe[2][r][g][b] < safe[2][r][g][b - 1])
                        safe[2][r][g][b] = safe[2][r][g][b - 1];
                }
            }
        }
    }

    /* 第三阶段：与 PQTools 17v2 一致地拆成 8 bank。 */
    for (b = 0; b < LUT_DIM; b++) {
        for (g = 0; g < LUT_DIM; g++) {
            for (r = 0; r < LUT_DIM; r++) {
                bank = (r & 1u) | ((g & 1u) << 1) | ((b & 1u) << 2);
                i = positions[bank]++;
                if (i >= g_bank_counts[bank]) return -1;
                for (c = 0; c < 3; c++) banks[bank][i][c] = safe[c][r][g][b];
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
    lut_bridge_cfg_t full;
    unsigned r, g, b;
    (void)cfg;
    lut_bridge_default_cfg(&full);
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

static unsigned limit_component(unsigned previous, unsigned candidate, unsigned max_step)
{
    if (candidate > previous + max_step) return previous + max_step;
    if (previous > candidate + max_step) return previous - max_step;
    return candidate;
}

int lut_bridge_limit_packed_step(const uint32_t *previous, uint32_t *candidate, size_t count,
                                 unsigned max_step_10bit)
{
    size_t i;
    if (previous == NULL || candidate == NULL || count != PIPELINE_ISP_CLUT_NODE_COUNT ||
        max_step_10bit == 0 || max_step_10bit > LUT_MAX) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        unsigned pr = (previous[i] >> 20) & LUT_MAX;
        unsigned pg = (previous[i] >> 10) & LUT_MAX;
        unsigned pb = previous[i] & LUT_MAX;
        unsigned cr = (candidate[i] >> 20) & LUT_MAX;
        unsigned cg = (candidate[i] >> 10) & LUT_MAX;
        unsigned cb = candidate[i] & LUT_MAX;
        cr = limit_component(pr, cr, max_step_10bit);
        cg = limit_component(pg, cg, max_step_10bit);
        cb = limit_component(pb, cb, max_step_10bit);
        candidate[i] = (cr << 20) | (cg << 10) | cb;
    }
    return 0;
}
