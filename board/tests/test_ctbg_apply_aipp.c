/* 最小 CTBG v8 AIPP apply OM 冒烟测试：仅 ACL，不涉及 MPP */
#include "infer_ctbg.h"
#include "log.h"
#include "pipeline.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    int rc = 1;
    setbuf(stdout, NULL);

    ctbg_cfg_t cfg = {
        .est_om_path = "/root/socchina-2026/ctbg6ch_estimator_256x144.om",
        .app_om_path = "/root/socchina-2026/ctbg_apply_twostage_nn_aipp_1024x576.om",
        .device_id = 0,
        .full_w = PIPELINE_STREAM_WIDTH, .full_h = PIPELINE_STREAM_HEIGHT,
        .low_w = PIPELINE_CONTROL_WIDTH, .low_h = PIPELINE_CONTROL_HEIGHT,
        .coeff_ch = 18,
    };

    LOG_INFO("init CTBG...");
    if (ctbg_init(&cfg) != 0) { LOG_ERR("init failed"); return 1; }
    LOG_INFO("init OK, raw_sz=%zu app_sz=%zu", ctbg_coeff_size(), ctbg_coeff_app_size());

    /* 分配输入缓冲区 */
    size_t nv21_sz = (size_t)PIPELINE_STREAM_WIDTH * PIPELINE_STREAM_HEIGHT * 3 / 2;
    size_t coeff_sz = ctbg_coeff_app_size();
    size_t out_sz   = (size_t)PIPELINE_STREAM_WIDTH * PIPELINE_STREAM_HEIGHT * 3 * 2;
    uint8_t  *nv21  = (uint8_t *)malloc(nv21_sz);
    uint16_t *coeff = (uint16_t *)malloc(coeff_sz);
    uint16_t *out   = (uint16_t *)malloc(out_sz);
    if (!nv21 || !coeff || !out) { LOG_ERR("oom"); goto cleanup; }

    /* 灰色 NV21（Y=128, U=V=128 → 中灰 RGB） */
    memset(nv21, 128, nv21_sz);
    /* 恒等系数（raw=0 → a=1,b=0,g=1 → 不改变画面） */
    memset(coeff, 0, coeff_sz);

    LOG_INFO("running apply (3 iterations)...");
    for (int i = 0; i < 3; i++) {
        ctbg_timing_t t;
        int ret = ctbg_apply_run_nv21(nv21, coeff, out, &t);
        LOG_INFO("  iter %d: ret=%d app_ms=%.2f", i, ret, t.app_ms);
        if (ret != 0) { LOG_ERR("apply failed at iter %d", i); goto cleanup; }
    }

    /* 检查输出：恒等系数下 RGB 应接近 (0.5, 0.5, 0.5) */
    float r = 0, g = 0, b = 0;
    int cs = PIPELINE_STREAM_WIDTH * PIPELINE_STREAM_HEIGHT;
    for (int i = 0; i < cs; i++) {
        uint32_t ri = (uint32_t)out[i] << 16;
        uint32_t gi = (uint32_t)out[cs + i] << 16;
        uint32_t bi = (uint32_t)out[2*cs + i] << 16;
        float rf, gf, bf;
        memcpy(&rf, &ri, 4); memcpy(&gf, &gi, 4); memcpy(&bf, &bi, 4);
        r += rf; g += gf; b += bf;
    }
    r /= cs; g /= cs; b /= cs;
    LOG_INFO("output mean RGB: %.4f %.4f %.4f (expect ~0.5)", r, g, b);

    rc = 0;
    LOG_INFO("PASSED");

cleanup:
    ctbg_deinit();
    free(nv21); free(coeff); free(out);
    return rc;
}
