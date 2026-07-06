/* ParamNet u(30) → ISP θ(97) 映射（板端 C 实现）。见 paramnet_map.h。
 * 与 models/isp_simulator/paramnet.py:u_to_theta 逐段同映射。 */

#include "paramnet_map.h"

#include <math.h>

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void paramnet_u_to_theta(const float u[PARAMNET_U_DIM], float theta[PARAMNET_THETA_DIM])
{
    /* 未被 u 覆盖的两维保持 identity（make_identity_params）：wdr=0、dehaze=0。 */
    theta[THETA_OFF_WDR]    = 0.0f;
    theta[THETA_OFF_DEHAZE] = 0.0f;

    /* Gamma [1:65]：node=linspace(0,1,64)，γ = 0.45 + u[29]*1.10，θ = node.clamp(1e-6)^γ */
    {
        float gam = 0.45f + u[29] * 1.10f;
        for (int i = 0; i < 64; i++) {
            float node = (float)i / 63.0f;
            if (node < 1e-6f) node = 1e-6f;
            theta[THETA_OFF_GAMMA + i] = powf(node, gam);
        }
    }

    /* DRC tone [65:71]：base=linspace(0,1,6)；delta=u[0:4]*0.60-0.30；
     * mid=(base[1:5]+delta).clamp(0,1)；cp=[base[0],mid,base[5]]；cummax。 */
    {
        float cp[6];
        cp[0] = 0.0f;                    /* base[0] */
        for (int i = 0; i < 4; i++) {
            float base = (float)(1 + i) / 5.0f;
            cp[1 + i] = clampf(base + (u[i] * 0.60f - 0.30f), 0.0f, 1.0f);
        }
        cp[5] = 1.0f;                    /* base[5] */
        for (int i = 1; i < 6; i++) {
            if (cp[i] < cp[i - 1]) cp[i] = cp[i - 1];   /* cummax */
        }
        for (int i = 0; i < 6; i++) theta[THETA_OFF_DRC_TONE + i] = cp[i];
    }

    /* DRC strength [87] = u[4] */
    theta[THETA_OFF_DRC_STRENGTH] = u[4];

    /* LDCI [88:96] = lo + u[5:13]*sp */
    {
        static const float lo[8] = {0.0f, 0.2f, 0.1f, 0.0f, 0.2f, 0.4f, 0.0f, 0.2f};
        static const float sp[8] = {0.90f, 0.6f, 0.5f, 0.60f, 0.6f, 0.5f, 0.5f, 0.6f};
        for (int i = 0; i < 8; i++) theta[THETA_OFF_LDCI + i] = lo[i] + u[5 + i] * sp[i];
    }

    /* DRC mix [71:83] = 0.2 + u[13:25]*0.7 */
    for (int i = 0; i < 12; i++) theta[THETA_OFF_DRC_MIX + i] = 0.2f + u[13 + i] * 0.7f;

    /* DRC ctrl [83:86] = 0.2 + u[25:28]*0.6 */
    for (int i = 0; i < 3; i++) theta[THETA_OFF_DRC_CTRL + i] = 0.2f + u[25 + i] * 0.6f;

    /* DRC blend [86] = 0.2 + u[28]*0.6 */
    theta[THETA_OFF_DRC_BLEND] = 0.2f + u[28] * 0.6f;
}
