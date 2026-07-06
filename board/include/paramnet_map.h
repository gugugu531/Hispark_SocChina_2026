#ifndef SOCCHINA_PARAMNET_MAP_H
#define SOCCHINA_PARAMNET_MAP_H

/* ParamNet u(30) → ISP θ(97) 映射的板端 C 实现（路线 B 实时闭环）。
 *
 * ⚠ 域一致性铁律（handoff §1 / docs/isp-param-tuning-agent-prompt.md）：
 *   本映射必须与 models/isp_simulator/paramnet.py:u_to_theta 逐段完全同映射
 *   （tone 对称 ±0.30 / strength / ldci / mix / ctrl / blend / gamma γ 幂曲线）。
 *   改一处必须改两处，否则 ParamNet 输出落到校准/蒸馏域外。
 *
 * 纯逻辑、SDK-free、可主机单测（board/tests/test_paramnet_map.c 对 Python golden 逐值核对）。
 */

#define PARAMNET_U_DIM     30
#define PARAMNET_THETA_DIM 97

/* θ 各段起始偏移（与 models/isp_simulator/params.py 布局一致）。 */
#define THETA_OFF_WDR          0
#define THETA_OFF_GAMMA        1   /* 64 节点 */
#define THETA_OFF_DRC_TONE     65  /* 6 控制点 */
#define THETA_OFF_DRC_MIX      71  /* 12 */
#define THETA_OFF_DRC_CTRL     83  /* 3 */
#define THETA_OFF_DRC_BLEND    86  /* 1 */
#define THETA_OFF_DRC_STRENGTH 87  /* 1 */
#define THETA_OFF_LDCI         88  /* 8 */
#define THETA_OFF_DEHAZE       96  /* 1 */

/* u∈[0,1]^30 → θ∈[0,1]^97。theta 由调用方提供 PARAMNET_THETA_DIM 长度缓冲。 */
void paramnet_u_to_theta(const float u[PARAMNET_U_DIM], float theta[PARAMNET_THETA_DIM]);

#include <stddef.h>

/* B2 blob 最大长度(DRC+LDCI+Gamma,无 guard/color):12+542+13+130 = 697。 */
#define PARAMNET_BLOB_MAX 697

/* θ(97) → ISP v3 blob 字节流,写入 buf(容量 cap),返回写入字节数(0=容量不足)。
 * 字节级等价于 models/isp_simulator/isp_blob.py:sim_params_to_blob
 * (enable_drc + enable_ldci 恒开,不含 guard/color 子段;gamma_on 可选)。
 *   drc_on/ldci_on : 段内 enable 字段(采中性帧时可置 0)。
 *   gamma_on       : 是否携带 Gamma 段(ParamNet θ 含 gamma,实时施加应为 1)。
 *   gamma_strength : Gamma 叠加强度 [0,1]。
 * 产物可直接交板端 blob 解析施加(格式同 isp_load_blob_and_apply)。 */
size_t paramnet_theta_to_blob(const float theta[PARAMNET_THETA_DIM],
                              int drc_on, int ldci_on,
                              int gamma_on, float gamma_strength,
                              unsigned char *buf, size_t cap);

#endif /* SOCCHINA_PARAMNET_MAP_H */
