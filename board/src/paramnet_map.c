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

/* ================================================================
 * θ(97) → ISP v3 blob（对齐 isp_blob.py:sim_params_to_blob）
 * ================================================================ */

#include <stdint.h>

/* 舍入模式必须逐处匹配 Python：
 *   torch .round() → 半偶（lrintf 默认 FE_TONEAREST）；tone / ldci hp,hn / gamma 曲线。
 *   int(x+0.5)     → 半上；scalars / strength / blc / sigma / gamma_strength。
 *   astype(uint8)  → 截断；mixing bright_x / dark_x。 */
static int round_even(float x) { return (int)lrintf(x); }
static int round_up(float x)   { return (int)(x + 0.5f); }
static int iclampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float fclampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void put_u32(unsigned char *b, size_t *pos, uint32_t v)
{
    b[(*pos)++] = (unsigned char)(v & 0xff);
    b[(*pos)++] = (unsigned char)((v >> 8) & 0xff);
    b[(*pos)++] = (unsigned char)((v >> 16) & 0xff);
    b[(*pos)++] = (unsigned char)((v >> 24) & 0xff);
}
static void put_u16(unsigned char *b, size_t *pos, uint16_t v)
{
    b[(*pos)++] = (unsigned char)(v & 0xff);
    b[(*pos)++] = (unsigned char)((v >> 8) & 0xff);
}

/* DRC tone: 6 控制点 → 200 节点。Catmull-Rom（均匀）→ cummax → 端点固定。 */
static void drc_tone_curve200(const float cp[6], float out[200])
{
    for (int j = 0; j < 200; j++) {
        float x = fclampf((float)j / 199.0f, 0.0f, 1.0f);
        float idx_f = x / 0.2f;                 /* dx = 1/5 */
        int idx0 = iclampi((int)idx_f, 0, 4);
        float t = idx_f - (float)idx0;
        float p0 = cp[iclampi(idx0 - 1, 0, 5)];
        float p1 = cp[iclampi(idx0,     0, 5)];
        float p2 = cp[iclampi(idx0 + 1, 0, 5)];
        float p3 = cp[iclampi(idx0 + 2, 0, 5)];
        float tt = t * t, ttt = tt * t;
        float w0 = 0.5f * (-t + 2.0f * tt - ttt);
        float w1 = 0.5f * (2.0f - 5.0f * tt + 3.0f * ttt);
        float w2 = 0.5f * (t + 4.0f * tt - 3.0f * ttt);
        float w3 = 0.5f * (-tt + ttt);
        out[j] = fclampf(p0 * w0 + p1 * w1 + p2 * w2 + p3 * w3, 0.0f, 1.0f);
    }
    for (int j = 1; j < 200; j++) {
        if (out[j] < out[j - 1]) out[j] = out[j - 1];   /* cummax */
    }
    out[0] = 0.0f;
    out[199] = 1.0f;
}

/* mixing 6 keypoints → 33 节点线性插值（np.interp），再 ×128 截断为 uint8。 */
static void mix6to33_u8(const float cp[6], unsigned char out[33])
{
    for (int j = 0; j < 33; j++) {
        float f = ((float)j / 32.0f) * 5.0f;
        int k = iclampi((int)f, 0, 4);
        float t = f - (float)k;
        float val = cp[k] * (1.0f - t) + cp[k + 1] * t;
        out[j] = (unsigned char)iclampi((int)fclampf(val * 128.0f, 0.0f, 128.0f), 0, 128);
    }
}

size_t paramnet_theta_to_blob(const float th[PARAMNET_THETA_DIM],
                              int drc_on, int ldci_on,
                              int gamma_on, float gamma_strength,
                              unsigned char *buf, size_t cap)
{
    size_t need = 12 + 542 + 13 + (gamma_on ? 130 : 0);
    if (!buf || cap < need) return 0;

    size_t pos = 0;
    uint32_t flags = 1u | 2u | (gamma_on ? 4u : 0u);   /* DRC | LDCI | (Gamma) */
    put_u32(buf, &pos, 0x49535000u);
    put_u32(buf, &pos, 3u);
    put_u32(buf, &pos, flags);

    /* ---- DRC ---- */
    put_u32(buf, &pos, drc_on ? 1u : 0u);
    {
        float tone[200];
        drc_tone_curve200(&th[THETA_OFF_DRC_TONE], tone);
        for (int i = 0; i < 200; i++)
            put_u16(buf, &pos, (uint16_t)iclampi(round_even(fclampf(tone[i] * 65535.0f, 0.0f, 65535.0f)), 0, 65535));
    }
    unsigned char bx[33], dx[33];
    mix6to33_u8(&th[THETA_OFF_DRC_MIX],     bx);   /* drc_mix[0:6] */
    mix6to33_u8(&th[THETA_OFF_DRC_MIX + 6], dx);   /* drc_mix[6:12] */
    for (int i = 0; i < 33; i++) buf[pos++] = bx[i];
    for (int i = 0; i < 33; i++) buf[pos++] = dx[i];
    buf[pos++] = (unsigned char)iclampi(round_up(th[THETA_OFF_DRC_CTRL + 0] * 5.0f),  0, 5);
    buf[pos++] = (unsigned char)iclampi(round_up(th[THETA_OFF_DRC_CTRL + 1] * 10.0f), 0, 10);
    buf[pos++] = (unsigned char)iclampi(round_up(th[THETA_OFF_DRC_CTRL + 2] * 15.0f), 0, 15);
    buf[pos++] = (unsigned char)iclampi(round_up(th[THETA_OFF_DRC_BLEND] * 255.0f),   0, 255);
    put_u16(buf, &pos, (uint16_t)iclampi(round_up(th[THETA_OFF_DRC_STRENGTH] * 1023.0f), 0, 1023));
    /* v3 主通路 mixing（与 X 通路同值） */
    for (int i = 0; i < 33; i++) buf[pos++] = bx[i];
    for (int i = 0; i < 33; i++) buf[pos++] = dx[i];

    /* ---- LDCI ---- */
    put_u32(buf, &pos, ldci_on ? 1u : 0u);
    for (int i = 0; i < 3; i++)   /* he_pos */
        buf[pos++] = (unsigned char)iclampi(round_even(fclampf(th[THETA_OFF_LDCI + i] * 255.0f, 0.0f, 255.0f)), 0, 255);
    for (int i = 0; i < 3; i++)   /* he_neg */
        buf[pos++] = (unsigned char)iclampi(round_even(fclampf(th[THETA_OFF_LDCI + 3 + i] * 255.0f, 0.0f, 255.0f)), 0, 255);
    put_u16(buf, &pos, (uint16_t)iclampi(round_up(th[THETA_OFF_LDCI + 6] * 511.0f), 0, 511));   /* blc */
    buf[pos++] = (unsigned char)iclampi(round_up(th[THETA_OFF_LDCI + 7] * 254.0f + 1.0f), 1, 255); /* sigma */

    /* ---- Gamma ---- */
    if (gamma_on) {
        for (int i = 0; i < 64; i++)
            put_u16(buf, &pos, (uint16_t)iclampi(round_even(fclampf(th[THETA_OFF_GAMMA + i] * 65535.0f, 0.0f, 65535.0f)), 0, 65535));
        put_u16(buf, &pos, (uint16_t)iclampi(round_up(gamma_strength * 1024.0f), 0, 1024));
    }

    return pos;
}
