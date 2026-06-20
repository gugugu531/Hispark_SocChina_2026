#ifndef SOCCHINA_LUT_BRIDGE_H
#define SOCCHINA_LUT_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "pipeline.h"

/* CoTF 立方 LUT → ISP CLUT 的纯逻辑桥，可在主机做单元测试。 */

typedef struct {
    float strength;               /* 0=identity，1=完整模型 LUT。 */
    float max_identity_delta;     /* 单通道相对 identity 的最大偏移。 */
    float highlight_boost_limit;  /* identity>=0.875 时允许的最大继续提亮量。 */
    int preserve_cube_endpoints;  /* 固定黑/白及 RGB/CMY 八个立方体端点。 */
    int enforce_axis_monotonicity; /* 各输出通道沿对应输入主轴保持单调。 */
} lut_bridge_cfg_t;

/* SS928 17v2：17^3 逻辑点、8 个奇偶 bank、4 路交织、R高/G中/B低。 */
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

/* 将候选 packed LUT 相对上一版逐通道限幅，避免一次热刷新产生大幅跳变。
 * max_step_10bit 是 10bit 节点值的最大绝对变化；previous/candidate 可原地相同。 */
int lut_bridge_limit_packed_step(const uint32_t *previous, uint32_t *candidate, size_t count,
                                 unsigned max_step_10bit);

#endif /* SOCCHINA_LUT_BRIDGE_H */
