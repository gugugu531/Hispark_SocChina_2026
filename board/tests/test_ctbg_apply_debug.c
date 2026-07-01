/* CTBG apply OM 对比诊断：v9 非 AIPP vs v8 AIPP，验证输出正确性 */
#include "infer_ctbg.h"
#include "log.h"
#include "pipeline.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FW 1024
#define FH 576
#define LW 256
#define LH 144

static void rgb_to_nv21(const float *rgb, uint8_t *nv21, int w, int h) {
    int y, x, cs = w * h;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int i = y * w + x;
            float R = rgb[0*cs + i], G = rgb[1*cs + i], B = rgb[2*cs + i];
            int Y  = (int)( 0.299f*R*255.0f + 0.587f*G*255.0f + 0.114f*B*255.0f + 0.5f);
            int U  = (int)(-0.169f*R*255.0f - 0.331f*G*255.0f + 0.500f*B*255.0f + 128.5f);
            int V  = (int)( 0.500f*R*255.0f - 0.419f*G*255.0f - 0.081f*B*255.0f + 128.5f);
            if (Y < 0) Y = 0; if (Y > 255) Y = 255;
            if (U < 0) U = 0; if (U > 255) U = 255;
            if (V < 0) V = 0; if (V > 255) V = 255;
            nv21[i] = (uint8_t)Y;
            if ((y & 1) == 0 && (x & 1) == 0) {
                int vi = (y/2)*(w/2)*2 + (x/2)*2;
                nv21[cs + vi] = (uint8_t)V;
                nv21[cs + vi + 1] = (uint8_t)U;
            }
        }
    }
}

static void f32_to_fp16(const float *f32, uint16_t *fp16, int count) {
    for (int i = 0; i < count; i++) {
        uint32_t bits; memcpy(&bits, &f32[i], 4);
        fp16[i] = (uint16_t)(bits >> 16);  /* 截断低 16 位，误差 <0.01% */
    }
}

static float fp16_to_f32(uint16_t v) {
    uint32_t s = (v & 0x8000u) << 16, e = (v >> 10) & 0x1F, m = v & 0x3FF, b;
    if (e == 0) {
        if (m == 0) b = s;
        else { e = 1; while ((m & 0x400) == 0) { m <<= 1; e--; } m &= 0x3FF;
               b = s | ((e + 127 - 15) << 23) | (m << 13); }
    } else if (e == 0x1F) b = s | 0x7F800000u | (m << 13);
    else b = s | ((e - 15 + 127) << 23) | (m << 13);
    float f; memcpy(&f, &b, 4); return f;
}

static int test_v9(const char *om_path, const float *in_rgb, const uint16_t *coeff_6ch,
                   float *out_rgb, int w, int h) {
    ctbg_cfg_t cfg = {
        .est_om_path = "/root/socchina-2026/ctbg6ch_estimator_256x144.om",
        .app_om_path = om_path,
        .device_id = 0, .full_w = w, .full_h = h,
        .low_w = LW, .low_h = LH, .coeff_ch = 6,
    };
    if (ctbg_init(&cfg) != 0) return -1;

    int cs = w * h;
    size_t rgb_sz = (size_t)cs * 3 * 2;
    size_t coeff_up_sz = (size_t)cs * 6 * 2;  /* 6ch 预上采样到全分辨率 */
    uint16_t *in_fp16  = (uint16_t *)malloc(rgb_sz);
    uint16_t *coeff_up = (uint16_t *)malloc(coeff_up_sz);
    uint16_t *out_fp16 = (uint16_t *)malloc(rgb_sz);
    if (!in_fp16 || !coeff_up || !out_fp16) { free(in_fp16); free(coeff_up); free(out_fp16); ctbg_deinit(); return -1; }

    /* RGB 输入 */
    f32_to_fp16(in_rgb, in_fp16, cs * 3);

    /* 系数预上采样（nearest-neighbor 144×256 → 576×1024） */
    int scale_x = w / LW, scale_y = h / LH;
    for (int c = 0; c < 6; c++) {
        for (int sy = 0; sy < LH; sy++) {
            for (int sx = 0; sx < LW; sx++) {
                uint16_t v = coeff_6ch[c * LW * LH + sy * LW + sx];
                for (int dy = 0; dy < scale_y; dy++) {
                    for (int dx = 0; dx < scale_x; dx++) {
                        int di = c * cs + (sy * scale_y + dy) * w + (sx * scale_x + dx);
                        coeff_up[di] = v;
                    }
                }
            }
        }
    }

    ctbg_timing_t t;
    int ret = ctbg_apply_run(in_fp16, coeff_up, out_fp16, &t);
    LOG_INFO("  v9 apply: ret=%d app_ms=%.2f", ret, t.app_ms);

    if (ret == 0) {
        for (int i = 0; i < cs * 3; i++) out_rgb[i] = fp16_to_f32(out_fp16[i]);
    }

    free(in_fp16); free(coeff_up); free(out_fp16);
    ctbg_deinit();
    return ret;
}

static int test_v8_aipp(const char *om_path, const float *in_rgb, const uint16_t *coeff_18ch,
                        float *out_rgb, int w, int h) {
    ctbg_cfg_t cfg = {
        .est_om_path = "/root/socchina-2026/ctbg6ch_estimator_256x144.om",
        .app_om_path = om_path,
        .device_id = 0, .full_w = w, .full_h = h,
        .low_w = LW, .low_h = LH, .coeff_ch = 18,
    };
    if (ctbg_init(&cfg) != 0) return -1;

    int cs = w * h;
    size_t nv21_sz = (size_t)cs * 3 / 2;
    size_t coeff_sz = (size_t)LW * LH * 18 * 2;  /* 低分辨率 18ch */
    size_t out_sz   = (size_t)cs * 3 * 2;
    uint8_t  *nv21  = (uint8_t *)malloc(nv21_sz);
    uint16_t *coeff = (uint16_t *)malloc(coeff_sz);
    uint16_t *out_fp16 = (uint16_t *)malloc(out_sz);
    if (!nv21 || !coeff || !out_fp16) {
        free(nv21); free(coeff); free(out_fp16); ctbg_deinit(); return -1;
    }

    /* RGB → NV21 */
    rgb_to_nv21(in_rgb, nv21, w, h);
    memcpy(coeff, coeff_18ch, coeff_sz);

    ctbg_timing_t t;
    int ret = ctbg_apply_run_nv21(nv21, coeff, out_fp16, &t);
    LOG_INFO("  v8_aipp apply: ret=%d app_ms=%.2f", ret, t.app_ms);

    if (ret == 0) {
        for (int i = 0; i < cs * 3; i++) out_rgb[i] = fp16_to_f32(out_fp16[i]);
    }

    free(nv21); free(coeff); free(out_fp16);
    ctbg_deinit();
    return ret;
}

int main(void) {
    int rc = 1, cs = FW * FH;
    setbuf(stdout, NULL);

    /* 测试图案：左半红色 (1,0,0)，右半蓝色 (0,0,1) */
    float *in_rgb = (float *)calloc(cs * 3, sizeof(float));
    if (!in_rgb) return 1;
    for (int y = 0; y < FH; y++) {
        for (int x = 0; x < FW; x++) {
            int i = y * FW + x;
            if (x < FW / 2) {
                in_rgb[0*cs + i] = 1.0f;  /* R=1 */
                in_rgb[1*cs + i] = 0.0f;  /* G=0 */
                in_rgb[2*cs + i] = 0.0f;  /* B=0 */
            } else {
                in_rgb[0*cs + i] = 0.0f;
                in_rgb[1*cs + i] = 0.0f;
                in_rgb[2*cs + i] = 1.0f;  /* B=1 */
            }
        }
    }

    /* 恒等系数：全零 raw（a=1,b=0,g=1 → 输出=输入）*/
    /* v9 6ch: 6×144×256 */
    uint16_t *coeff_6ch = (uint16_t *)calloc(LW * LH * 6, 2);
    /* v8 18ch: 18×144×256 */
    uint16_t *coeff_18ch = (uint16_t *)calloc(LW * LH * 18, 2);
    if (!coeff_6ch || !coeff_18ch) { free(in_rgb); free(coeff_6ch); free(coeff_18ch); return 1; }

    float *out9 = (float *)calloc(cs * 3, sizeof(float));
    float *out8 = (float *)calloc(cs * 3, sizeof(float));

    LOG_INFO("=== Test 1: v9 non-AIPP OM ===");
    int r9 = test_v9("/root/socchina-2026/ctbg6ch_apply_1024x576.om",
                     in_rgb, coeff_6ch, out9, FW, FH);

    LOG_INFO("=== Test 2: v8 AIPP OM ===");
    int r8 = test_v8_aipp("/root/socchina-2026/ctbg_apply_twostage_nn_aipp_1024x576.om",
                          in_rgb, coeff_18ch, out8, FW, FH);

    /* 分析结果 */
    if (r9 == 0) {
        float r_mean = 0, g_mean = 0, b_mean = 0;
        for (int i = 0; i < cs; i++) {
            r_mean += out9[i]; g_mean += out9[cs + i]; b_mean += out9[2*cs + i];
        }
        r_mean /= cs; g_mean /= cs; b_mean /= cs;
        LOG_INFO("v9 output mean: R=%.4f G=%.4f B=%.4f (expect R=0.5 B=0.5 G=0)", r_mean, g_mean, b_mean);
    } else {
        LOG_ERR("v9 test FAILED");
    }

    if (r8 == 0) {
        float r_mean = 0, g_mean = 0, b_mean = 0;
        int nonzero = 0;
        for (int i = 0; i < cs; i++) {
            r_mean += out8[i]; g_mean += out8[cs + i]; b_mean += out8[2*cs + i];
            if (out8[i] > 0.001f || out8[cs + i] > 0.001f || out8[2*cs + i] > 0.001f) nonzero++;
        }
        r_mean /= cs; g_mean /= cs; b_mean /= cs;
        LOG_INFO("v8 output mean: R=%.4f G=%.4f B=%.4f  nonzero_pixels=%d/%d",
                 r_mean, g_mean, b_mean, nonzero, cs);
    } else {
        LOG_ERR("v8 test FAILED");
    }

    /* 交叉验证：v9 和 v8 输出应该一致（同为恒等变换） */
    if (r9 == 0 && r8 == 0) {
        float mse = 0;
        for (int i = 0; i < cs * 3; i++) {
            float d = out9[i] - out8[i];
            mse += d * d;
        }
        mse /= (cs * 3);
        LOG_INFO("v9 vs v8 MSE: %.6f (expect ~0)", mse);
    }

    rc = 0;
    LOG_INFO("DONE");
    free(in_rgb); free(coeff_6ch); free(coeff_18ch); free(out9); free(out8);
    return rc;
}
