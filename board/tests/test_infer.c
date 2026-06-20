/* test_infer — CoTF param-net 板端 NPU 推理冒烟（触硬件 + NPU/ACL）。
 *
 * 链路：相机 → VI → ISP → VPSS chn2 256x144 NV21 → AIPP
 *      → infer_run_nv21（ACL 在 NPU 跑 param-net OM）→ LUT 系数。
 * 验证 infer.c 的 ACL 推理通路：模型加载、输入复制、NPU 执行、输出取回与耗时分解。
 * 默认模型为 LCDP best checkpoint 转换的 256x144+AIPP OM；仍需生产安全参数桥与现场画质验收。
 *
 * 用法：./test_infer [--model <om>] [--iters N] [--sensor 0|1]
 * 前置：板上接 OS08A20；OM 已部署；LD_LIBRARY_PATH 含 /opt/lib/npu（libascendcl 等）。
 */

#include "capture.h"
#include "display.h"
#include "infer.h"
#include "log.h"
#include "vpss.h"

#ifndef WITH_SS928_SDK

int main(void)
{
    LOG_WARN("test_infer skipped: built without SS928 SDK (host build)");
    return 0;
}

#else /* WITH_SS928_SDK */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ot_buffer.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_type.h"
#include "sample_comm.h"

int main(int argc, char **argv)
{
    int iters = 30;
    capture_cfg_t cap_cfg = {CAPTURE_SENSOR_INDEX_DEFAULT, CAPTURE_MODE_LINEAR};
    const char *model = "/root/socchina-2026/cotf_paramnet_256x144_lcdp_best_e0167_fp16_aipp.om";
    int rc = 1;
    int sys_up = 0, cap_up = 0, vpss_up = 0, bound = 0, infer_up = 0;
    unsigned in_w = 0, in_h = 0;
    int i, ok = 0;
    float exec_sum = 0.0f, exec_max = 0.0f;
    static float lut_out[PIPELINE_COTF_LUT_FLOAT_COUNT];
    ot_vb_cfg vb_cfg = {0};
    ot_pic_buf_attr buf_attr = {0};
    ot_vb_calc_cfg calc_cfg = {0};
    ot_video_frame_info frame;
    infer_cfg_t icfg;
    vpss_chn_cfg_t chn_cfg[VPSS_CHN_COUNT] = {
        {0, 0, 0, 0},
        {0, 0, 0, 0},
        {1, PIPELINE_CONTROL_WIDTH, PIPELINE_CONTROL_HEIGHT, 2},
    };

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model = argv[++i];
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sensor") == 0 && i + 1 < argc) {
            cap_cfg.sensor_index = atoi(argv[++i]);
        }
    }

    if (capture_query_in_size(&in_w, &in_h) != 0) {
        return 1;
    }
    LOG_INFO("test_infer: model=%s iters=%d sensor=%d", model, iters, cap_cfg.sensor_index);

    buf_attr.width = in_w;
    buf_attr.height = in_h;
    buf_attr.align = OT_DEFAULT_ALIGN;
    buf_attr.bit_width = OT_DATA_BIT_WIDTH_8;
    buf_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    buf_attr.compress_mode = OT_COMPRESS_MODE_SEG;
    ot_common_get_pic_buf_cfg(&buf_attr, &calc_cfg);
    vb_cfg.max_pool_cnt = 2;
    vb_cfg.common_pool[0].blk_size = calc_cfg.vb_size;
    vb_cfg.common_pool[0].blk_cnt = 10;
    CHECK_RET_GOTO(sample_comm_sys_init_with_vb_supplement(&vb_cfg, OT_VB_SUPPLEMENT_BNR_MOT_MASK),
                   cleanup);
    sys_up = 1;

    if (capture_init(&cap_cfg) != 0) {
        goto cleanup;
    }
    cap_up = 1;
    if (vpss_init(0, in_w, in_h, chn_cfg) != 0) {
        goto cleanup;
    }
    vpss_up = 1;
    if (capture_bind_vpss(0) != 0) {
        goto cleanup;
    }
    bound = 1;

    icfg.model_path = model;
    icfg.device_id = 0;
    icfg.input_width = PIPELINE_CONTROL_WIDTH;
    icfg.input_height = PIPELINE_CONTROL_HEIGHT;
    icfg.lut_dim = PIPELINE_COTF_LUT_DIM;
    icfg.input_mode = PIPELINE_INPUT_COPY;
    if (infer_init(&icfg) != 0) {
        goto cleanup;
    }
    infer_up = 1;

    for (i = 0; i < iters; i++) {
        infer_timing_t t = {0};
        if (vpss_get_frame(0, PIPELINE_VPSS_CHN_CONTROL, &frame, 1000) != 0) {
            LOG_WARN("vpss_get_frame timeout");
            continue;
        }
        if (infer_run_nv21(&frame, lut_out, PIPELINE_COTF_LUT_FLOAT_COUNT, &t) == 0) {
            float lo = lut_out[0], hi = lut_out[0], sum = 0.0f;
            unsigned k;
            for (k = 0; k < PIPELINE_COTF_LUT_FLOAT_COUNT; k++) {
                if (lut_out[k] < lo) lo = lut_out[k];
                if (lut_out[k] > hi) hi = lut_out[k];
                sum += lut_out[k];
            }
            exec_sum += t.execute_ms;
            if (t.execute_ms > exec_max) exec_max = t.execute_ms;
            ok++;
            if (i < 3 || i % 10 == 0) {
                LOG_INFO("iter %d: in_copy=%.2f exec=%.2f out_copy=%.2f total=%.2f ms | lut[min=%.3f max=%.3f mean=%.3f]",
                         i, t.input_copy_ms, t.execute_ms, t.output_copy_ms, t.total_ms,
                         lo, hi, sum / PIPELINE_COTF_LUT_FLOAT_COUNT);
            }
        }
        (void)vpss_release_frame(0, PIPELINE_VPSS_CHN_CONTROL, &frame);
    }

    if (ok > 0) {
        rc = 0;
        LOG_INFO("test_infer PASS: %d/%d ok, NPU exec avg=%.2fms max=%.2fms (LUT %d floats)", ok,
                 iters, exec_sum / ok, exec_max, PIPELINE_COTF_LUT_FLOAT_COUNT);
    } else {
        LOG_ERR("test_infer: no successful inference");
    }

cleanup:
    if (infer_up) {
        (void)infer_deinit();
    }
    if (bound) {
        CHECK_RET(capture_unbind_vpss(0));
    }
    if (vpss_up) {
        CHECK_RET(vpss_deinit(0));
    }
    if (cap_up) {
        CHECK_RET(capture_deinit());
    }
    if (sys_up) {
        sample_comm_sys_exit();
    }
    return rc;
}

#endif /* WITH_SS928_SDK */
