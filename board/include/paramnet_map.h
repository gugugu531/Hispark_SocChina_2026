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

#endif /* SOCCHINA_PARAMNET_MAP_H */
