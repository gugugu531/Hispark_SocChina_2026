/* CTBG 系数图 → ISP DRC/LDCI 参数映射实现。
 *
 * 聚合 estimator 输出的 6ch 逐像素系数到 17×15 分块，翻译为 ISP 硬件
 * 可消费的局部色调曲线和细节增强参数。DRC 模块在 ISP 内部就是空间自适应的——
 * 基于局域亮度选择滤波器和色调权重，本模块通过改变 DRC 参数将 CTBG 的
 * 空间意图注入硬件管线。 */
#include "ctbg_isp_map.h"
#include "isp.h"
#include "log.h"
#include <math.h>
#include <string.h>

#ifdef WITH_SS928_SDK
#include "ot_common_isp.h"
#include "ss_mpi_isp.h"
#define ISP_PIPE 0
#endif
#include <string.h>

/* fp16→float 解码（来自 main.c） */
static float f16tof32(uint16_t v) {
    uint32_t s = (v & 0x8000u) << 16, e = (v >> 10) & 0x1Fu, m = v & 0x3FFu, b;
    if (e == 0) {
        if (m == 0) b = s;
        else { e = 1; while ((m & 0x400) == 0) { m <<= 1; e--; } m &= 0x3FF;
               b = s | ((e + 127 - 15) << 23) | (m << 13); }
    } else if (e == 0x1F) b = s | 0x7F800000u | (m << 13);
    else b = s | ((e - 15 + 127) << 23) | (m << 13);
    float f; memcpy(&f, &b, 4); return f;
}

void ctbg_isp_map_blocks(const uint16_t *coeff_up,
                         unsigned full_w, unsigned full_h,
                         ctbg_block_stat_t blocks[CTBG_ISP_MAP_ROWS][CTBG_ISP_MAP_COLS])
{
    unsigned blk_w = full_w / CTBG_ISP_MAP_COLS;  /* 1024/17 ≈ 60 */
    unsigned blk_h = full_h / CTBG_ISP_MAP_ROWS;  /* 576/15 ≈ 38 */
    unsigned cs = full_w * full_h;

    memset(blocks, 0, sizeof(ctbg_block_stat_t) * CTBG_ISP_MAP_ROWS * CTBG_ISP_MAP_COLS);

    /* 第一遍：累加每块的 a_d(ch0), a_b(ch3), g_d(ch2) */
    for (unsigned by = 0; by < CTBG_ISP_MAP_ROWS; by++) {
        for (unsigned bx = 0; bx < CTBG_ISP_MAP_COLS; bx++) {
            float sum_ad = 0, sum_ab = 0, sum_gd = 0;
            int count = 0;
            /* 4× 子采样：每块仅处理 1/16 像素（15×10 vs 60×38），fp16 解码 LUT 级加速 */
            int step = 4;
            for (unsigned y = by * blk_h; y < (by + 1) * blk_h && y < full_h; y += step) {
                for (unsigned x = bx * blk_w; x < (bx + 1) * blk_w && x < full_w; x += step) {
                    int i = y * full_w + x;
                    sum_ad += f16tof32(coeff_up[i]);
                    sum_gd += f16tof32(coeff_up[2*cs + i]);
                    sum_ab += f16tof32(coeff_up[3*cs + i]);
                    count++;
                }
            }
            if (count > 0) {
                blocks[by][bx].a_dark_mean   = sum_ad / count;
                blocks[by][bx].a_bright_mean = sum_ab / count;
                blocks[by][bx].g_dark_mean   = sum_gd / count;
                /* block_luma: a_dark 越大表示该块越需要提亮（暗块） */
                blocks[by][bx].block_luma = 1.0f - (blocks[by][bx].a_dark_mean - 0.8f) / 1.2f;
                if (blocks[by][bx].block_luma < 0.0f) blocks[by][bx].block_luma = 0.0f;
                if (blocks[by][bx].block_luma > 1.0f) blocks[by][bx].block_luma = 1.0f;
            }
        }
    }
}

void ctbg_isp_map_drc_tone(const ctbg_block_stat_t blocks[CTBG_ISP_MAP_ROWS][CTBG_ISP_MAP_COLS],
                           uint16_t tmv[200], float strength)
{
    /* 统计暗块和亮块比例 */
    int dark_count = 0, bright_count = 0, total = CTBG_ISP_MAP_ROWS * CTBG_ISP_MAP_COLS;
    float avg_dark_a = 0, avg_bright_a = 0;
    for (int by = 0; by < CTBG_ISP_MAP_ROWS; by++) {
        for (int bx = 0; bx < CTBG_ISP_MAP_COLS; bx++) {
            if (blocks[by][bx].a_dark_mean > 0.9f) { dark_count++; avg_dark_a += blocks[by][bx].a_dark_mean; }
            if (blocks[by][bx].a_bright_mean < 0.9f) { bright_count++; avg_bright_a += blocks[by][bx].a_bright_mean; }
        }
    }
    float dark_ratio = (float)dark_count / total;
    float bright_ratio = (float)bright_count / total;
    if (dark_count > 0) avg_dark_a /= dark_count;
    if (bright_count > 0) avg_bright_a /= bright_count;

    /* 构造 200 节点色调曲线（输入索引 x=0..199 映射亮度 0..1） */
    for (int i = 0; i < 200; i++) {
        float x = (float)i / 199.0f;  /* 输入亮度 0..1 */
        float y = x;                   /* 默认：直通 */

        /* 暗块 > 30%：上凸曲线 → 提亮暗部 */
        if (dark_ratio > 0.3f) {
            float boost = dark_ratio * strength * 0.5f;
            /* Gamma <1: 暗部提亮 */
            float gamma = 1.0f - boost * 0.6f;  /* γ∈[0.7, 1.0] */
            if (gamma < 0.6f) gamma = 0.6f;
            y = powf(x, gamma);
        }
        /* 亮块 > 20%：下凹曲线 → 压暗高光 */
        if (bright_ratio > 0.2f) {
            float suppress = bright_ratio * strength * 0.4f;
            float gamma = 1.0f + suppress;  /* γ∈[1.0, 1.4] */
            if (gamma > 1.5f) gamma = 1.5f;
            y = powf(x, gamma);
            /* 混合：暗部提亮 + 亮部压暗 */
            float w = x;  /* 暗部权重 x→0, 亮部权重 x→1 */
            float y_dark = powf(x, 0.7f);
            float y_bright = powf(x, 1.4f);
            y = (1.0f - w) * y_dark + w * y_bright;
        }

        /* strength 控制整体强度 */
        y = x + strength * (y - x);
        if (y < 0.0f) { y = 0.0f; } if (y > 1.0f) { y = 1.0f; }
        tmv[i] = (uint16_t)(y * 65535.0f + 0.5f);
    }
}

void ctbg_isp_map_drc_mixing(const ctbg_block_stat_t blocks[CTBG_ISP_MAP_ROWS][CTBG_ISP_MAP_COLS],
                             uint8_t bright_lut[33], uint8_t dark_lut[33])
{
    /* 默认值：适度细节增强 */
    for (int i = 0; i < 33; i++) {
        bright_lut[i] = 64;  /* 0x40, 中等 */
        dark_lut[i]   = 64;
    }

    /* 统计全局 a_dark 的分布，决定增强策略 */
    float avg_ad = 0;
    for (int by = 0; by < CTBG_ISP_MAP_ROWS; by++)
        for (int bx = 0; bx < CTBG_ISP_MAP_COLS; bx++)
            avg_ad += blocks[by][bx].a_dark_mean;
    avg_ad /= (CTBG_ISP_MAP_ROWS * CTBG_ISP_MAP_COLS);

    /* a_dark>1.0 表示暗场景 → 增强暗部细节 */
    if (avg_ad > 1.0f) {
        float scale = (avg_ad - 1.0f) / 0.8f;  /* 1.0→0, 1.8→1.0 */
        if (scale > 1.0f) scale = 1.0f;
        for (int i = 0; i < 16; i++) {  /* 低亮度段 (0-15) */
            bright_lut[i] = (uint8_t)(64.0f + 64.0f * scale);
            dark_lut[i]   = (uint8_t)(64.0f + 32.0f * scale);
        }
    }
    /* a_bright<1.0 表示亮场景 → 适度降低亮部增强防 halo */
    float avg_ab = 0;
    for (int by = 0; by < CTBG_ISP_MAP_ROWS; by++)
        for (int bx = 0; bx < CTBG_ISP_MAP_COLS; bx++)
            avg_ab += blocks[by][bx].a_bright_mean;
    avg_ab /= (CTBG_ISP_MAP_ROWS * CTBG_ISP_MAP_COLS);
    if (avg_ab < 0.9f) {
        float scale = (1.0f - avg_ab) / 0.3f;
        if (scale > 1.0f) scale = 1.0f;
        for (int i = 16; i < 33; i++) {  /* 高亮度段 */
            bright_lut[i] = (uint8_t)(64.0f * (1.0f - scale * 0.5f));
        }
    }
}

int ctbg_isp_map_apply(const uint16_t *coeff_up,
                       unsigned full_w, unsigned full_h,
                       float strength)
{
#ifdef WITH_SS928_SDK
    ctbg_block_stat_t blocks[CTBG_ISP_MAP_ROWS][CTBG_ISP_MAP_COLS];
    uint16_t tmv[200];
    uint8_t  bright_lut[33], dark_lut[33];
    ot_isp_drc_attr drc_attr;
    ot_isp_ldci_attr ldci_attr;

    /* 1. 聚合系数图 */
    ctbg_isp_map_blocks(coeff_up, full_w, full_h, blocks);

    /* 2. 映射 DRC 色调曲线 */
    ctbg_isp_map_drc_tone(blocks, tmv, strength);
    ctbg_isp_map_drc_mixing(blocks, bright_lut, dark_lut);

    /* 3. 获取当前 DRC 参数并更新 */
    memset(&drc_attr, 0, sizeof(drc_attr));
    if (ss_mpi_isp_get_drc_attr(ISP_PIPE, &drc_attr) != 0) {
        LOG_ERR("ctbg_isp_map: get drc attr failed");
        return -1;
    }
    drc_attr.enable = TD_TRUE;
    drc_attr.op_type = OT_OP_MODE_MANUAL;
    drc_attr.curve_select = OT_ISP_DRC_CURVE_USER;
    drc_attr.spatial_filter_coef = 3;   /* 中等空间滤波 */
    drc_attr.range_filter_coef = 5;     /* 中等保边 */
    drc_attr.contrast_ctrl = 8;         /* 典型值 */
    memcpy(drc_attr.tone_mapping_value, tmv, sizeof(tmv));
    memcpy(drc_attr.local_mixing_bright_x, bright_lut, sizeof(bright_lut));
    memcpy(drc_attr.local_mixing_dark_x, dark_lut, sizeof(dark_lut));
    /* FilterX 为主（背光/暗部优先），Filter 为辅 */
    drc_attr.blend_luma_max = 0x40;  /* 偏 FilterX */
    drc_attr.detail_adjust_coef_x = 8;
    drc_attr.manual_attr.strength = (td_u16)(strength * 1023.0f);

    if (ss_mpi_isp_set_drc_attr(ISP_PIPE, &drc_attr) != 0) {
        LOG_ERR("ctbg_isp_map: set drc attr failed");
        return -1;
    }

    /* 4. LDCI：根据暗块比例调整局域对比度 */
    int dark_count = 0, total = CTBG_ISP_MAP_ROWS * CTBG_ISP_MAP_COLS;
    for (int by = 0; by < CTBG_ISP_MAP_ROWS; by++)
        for (int bx = 0; bx < CTBG_ISP_MAP_COLS; bx++)
            if (blocks[by][bx].block_luma < 0.4f) dark_count++;
    float dark_ratio = (float)dark_count / total;

    memset(&ldci_attr, 0, sizeof(ldci_attr));
    if (ss_mpi_isp_get_ldci_attr(ISP_PIPE, &ldci_attr) != 0) {
        LOG_WARN("ctbg_isp_map: get ldci attr failed, skipping");
    } else {
        ldci_attr.en = TD_TRUE;
        ldci_attr.op_type = OT_OP_MODE_MANUAL;
        ldci_attr.manual_attr.he_wgt.he_pos_wgt.wgt   = (td_u8)(128.0f + 64.0f * dark_ratio);
        ldci_attr.manual_attr.he_wgt.he_pos_wgt.sigma = 8;
        ldci_attr.manual_attr.he_wgt.he_pos_wgt.mean  = 64;
        ldci_attr.manual_attr.he_wgt.he_neg_wgt.wgt   = (td_u8)(128.0f - 32.0f * dark_ratio);
        ldci_attr.manual_attr.he_wgt.he_neg_wgt.sigma = 4;
        ldci_attr.manual_attr.he_wgt.he_neg_wgt.mean  = 192;
        ldci_attr.manual_attr.blc_ctrl = (td_u16)(128.0f + 128.0f * dark_ratio);
        ldci_attr.gauss_lpf_sigma = 4;
        if (ss_mpi_isp_set_ldci_attr(ISP_PIPE, &ldci_attr) != 0) {
            LOG_WARN("ctbg_isp_map: set ldci attr failed");
        }
    }

    LOG_INFO("ctbg_isp_map: DRC+LUT+LDCI (dark=%.0f%%, strength=%.2f)",
             dark_ratio * 100.0f, strength);
    return 0;
#else
    (void)coeff_up; (void)full_w; (void)full_h; (void)strength;
    return -1;
#endif
}
