#ifndef SOCCHINA_INFER_H
#define SOCCHINA_INFER_H

#include <stddef.h>

#include "pipeline.h"

/* CoTF 参数网络 ACL 封装。首版为同步、单实例、单线程调用。 */

typedef struct {
    const char *model_path;
    int device_id;
    unsigned input_width;
    unsigned input_height;
    unsigned lut_dim;
    pipeline_input_mode_t input_mode;
} infer_cfg_t;

typedef struct {
    float input_copy_ms;
    float execute_ms;
    float output_copy_ms;
    float total_ms;
} infer_timing_t;

/*
 * 前置：
 * - frame_info 实际为 const ot_video_frame_info*；
 * - NV21/YVU420SP、无压缩、尺寸与 cfg 一致；
 * - 调用期间 VPSS 帧仍处于借用状态。
 *
 * 输出：
 * - lut_out 由调用者分配，至少 PIPELINE_COTF_LUT_FLOAT_COUNT 个 float；
 * - 排布为 [RGB][R][G][B]，B 轴最快，与 models/tools/cotf_lut_pack.py 一致；
 * - 函数返回前已完成输入/输出复制，不保留 frame_info 或 lut_out。
 */
int infer_init(const infer_cfg_t *cfg);
int infer_run_nv21(const void *frame_info, float *lut_out, size_t lut_count,
                   infer_timing_t *timing);

/* 固定输入/数值对拍入口：input 必须与 OM 输入缓冲大小完全一致。
 * AIPP OM 输入原始 NV21；裸 OM 输入 NCHW FP16。 */
int infer_run_buffer(const void *input, size_t input_size, float *lut_out, size_t lut_count,
                     infer_timing_t *timing);
int infer_deinit(void);

#endif /* SOCCHINA_INFER_H */
