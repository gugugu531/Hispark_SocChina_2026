#ifndef SOCCHINA_LUT_BRIDGE_H
#define SOCCHINA_LUT_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "pipeline.h"

/* CoTF 立方 LUT → ISP CLUT 的纯逻辑桥，可在主机做单元测试。 */

typedef enum {
    LUT_CHANNEL_ORDER_RGB = 0,
    LUT_CHANNEL_ORDER_BGR
} lut_channel_order_t;

typedef struct {
    unsigned mesh_r;
    unsigned mesh_g;
    unsigned mesh_b;
    lut_channel_order_t channel_order;
} lut_bridge_cfg_t;

/* 当前 17x18x18 仅为待 SDK 调优文档确认的候选 mesh，不得当作已标定事实。 */
void lut_bridge_default_cfg(lut_bridge_cfg_t *cfg);

/*
 * cubic_lut 排布：[RGB][R][G][B]，B 轴最快，共 PIPELINE_COTF_LUT_FLOAT_COUNT 项。
 * packed_out 为 PIPELINE_ISP_CLUT_NODE_COUNT 个 u32，每节点 3x10bit。
 * 本函数不分配内存；输入输出可由控制线程长期复用。
 */
int lut_bridge_pack(const lut_bridge_cfg_t *cfg, const float *cubic_lut, size_t cubic_count,
                    uint32_t *packed_out, size_t packed_count);

/* 生成诊断用恒等 LUT，供 CLUT 联机点亮和 mesh 核对，不依赖 NN。 */
int lut_bridge_make_identity(const lut_bridge_cfg_t *cfg, uint32_t *packed_out,
                             size_t packed_count);

#endif /* SOCCHINA_LUT_BRIDGE_H */
