#include "isp.h"

#include "log.h"

#ifdef WITH_SS928_SDK

#include <math.h>
#include <string.h>

#include "ot_common_isp.h"
#include "ot_type.h"
#include "ss_mpi_ae.h"
#include "ss_mpi_isp.h"

#define ISP_PIPE 0

/* luma_stats 的黑/白场裁剪阈值（1024-bin 直方图上的 5% / 95% 位置）。 */
#define ISP_HIST_BINS     1024
#define ISP_CLIP_LOW_BIN  51
#define ISP_CLIP_HIGH_BIN 973

int isp_set_antiflicker(int freq_hz)
{
    ot_isp_exposure_attr exp_attr;

    if (freq_hz != 0 && freq_hz != 50 && freq_hz != 60) {
        LOG_ERR("isp_set_antiflicker: invalid freq %d (0/50/60)", freq_hz);
        return -1;
    }
    memset(&exp_attr, 0, sizeof(exp_attr));
    CHECK_RET_GOTO(ss_mpi_isp_get_exposure_attr(ISP_PIPE, &exp_attr), fail);
    exp_attr.auto_attr.antiflicker.enable = (freq_hz != 0) ? TD_TRUE : TD_FALSE;
    if (freq_hz != 0) {
        exp_attr.auto_attr.antiflicker.frequency = (td_u8)freq_hz;
    }
    CHECK_RET_GOTO(ss_mpi_isp_set_exposure_attr(ISP_PIPE, &exp_attr), fail);
    LOG_INFO("isp antiflicker -> %d Hz", freq_hz);
    return 0;
fail:
    return -1;
}

int isp_set_ae_compensation(unsigned char comp)
{
    ot_isp_exposure_attr exp_attr;

    memset(&exp_attr, 0, sizeof(exp_attr));
    CHECK_RET_GOTO(ss_mpi_isp_get_exposure_attr(ISP_PIPE, &exp_attr), fail);
    exp_attr.auto_attr.compensation = comp;
    CHECK_RET_GOTO(ss_mpi_isp_set_exposure_attr(ISP_PIPE, &exp_attr), fail);
    LOG_INFO("isp ae compensation -> %u", comp);
    return 0;
fail:
    return -1;
}

int isp_set_exposure_manual(unsigned exp_time_us, unsigned again_x1024)
{
    ot_isp_exposure_attr exp_attr;
    int manual = (exp_time_us != 0 || again_x1024 != 0);

    memset(&exp_attr, 0, sizeof(exp_attr));
    CHECK_RET_GOTO(ss_mpi_isp_get_exposure_attr(ISP_PIPE, &exp_attr), fail);
    exp_attr.op_type = manual ? OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
    if (manual) {
        /* 四个分量全手动；数字增益固定 1x，否则 AE 会用 AUTO 的数字增益
         * 把亮度补回目标值，手动曝光形同虚设（板端实测踩坑）。 */
        exp_attr.manual_attr.exp_time_op_type = OT_OP_MODE_MANUAL;
        exp_attr.manual_attr.a_gain_op_type = OT_OP_MODE_MANUAL;
        exp_attr.manual_attr.d_gain_op_type = OT_OP_MODE_MANUAL;
        exp_attr.manual_attr.ispd_gain_op_type = OT_OP_MODE_MANUAL;
        exp_attr.manual_attr.exp_time = exp_time_us;
        exp_attr.manual_attr.a_gain = (again_x1024 != 0) ? again_x1024 : 1024;
        exp_attr.manual_attr.d_gain = 1024;
        exp_attr.manual_attr.isp_d_gain = 1024;
    }
    CHECK_RET_GOTO(ss_mpi_isp_set_exposure_attr(ISP_PIPE, &exp_attr), fail);
    LOG_INFO("isp exposure -> %s (time=%uus again=%u/1024)", manual ? "manual" : "auto",
             exp_time_us, again_x1024);
    return 0;
fail:
    return -1;
}

int isp_set_ae_strategy(int highlight_prior)
{
    ot_isp_exposure_attr exp_attr;

    memset(&exp_attr, 0, sizeof(exp_attr));
    CHECK_RET_GOTO(ss_mpi_isp_get_exposure_attr(ISP_PIPE, &exp_attr), fail);
    exp_attr.auto_attr.ae_strategy_mode =
        highlight_prior ? OT_ISP_AE_EXP_HIGHLIGHT_PRIOR : OT_ISP_AE_EXP_LOWLIGHT_PRIOR;
    CHECK_RET_GOTO(ss_mpi_isp_set_exposure_attr(ISP_PIPE, &exp_attr), fail);
    LOG_INFO("isp ae strategy -> %s", highlight_prior ? "highlight prior" : "lowlight prior");
    return 0;
fail:
    return -1;
}

int isp_set_wdr_exposure_ratio(unsigned ratio_x64)
{
    ot_isp_wdr_exposure_attr wdr_attr;

    memset(&wdr_attr, 0, sizeof(wdr_attr));
    CHECK_RET_GOTO(ss_mpi_isp_get_wdr_exposure_attr(ISP_PIPE, &wdr_attr), fail);
    if (ratio_x64 == 0) {
        wdr_attr.exp_ratio_type = OT_OP_MODE_AUTO;
    } else {
        wdr_attr.exp_ratio_type = OT_OP_MODE_MANUAL;
        wdr_attr.exp_ratio[0] = ratio_x64; /* 2to1 WDR 只用第 0 组长短比 */
    }
    CHECK_RET_GOTO(ss_mpi_isp_set_wdr_exposure_attr(ISP_PIPE, &wdr_attr), fail);
    LOG_INFO("isp wdr exp ratio -> %s (%u/64x)", (ratio_x64 == 0) ? "auto" : "manual", ratio_x64);
    return 0;
fail:
    return -1;
}

int isp_get_luma_stats(luma_stats_t *stats)
{
    static ot_isp_ae_stats ae_stats; /* ~40KB，避免压栈；模块接口非线程安全 */
    td_u64 total = 0, weighted = 0, low = 0, high = 0;
    int i;

    if (stats == NULL) {
        return -1;
    }
    CHECK_RET_GOTO(ss_mpi_isp_get_ae_stats(ISP_PIPE, &ae_stats), fail);

    for (i = 0; i < ISP_HIST_BINS; i++) {
        td_u32 cnt = ae_stats.be_hist1024_value[i];
        total += cnt;
        weighted += (td_u64)cnt * (td_u64)i;
        if (i <= ISP_CLIP_LOW_BIN) {
            low += cnt;
        } else if (i >= ISP_CLIP_HIGH_BIN) {
            high += cnt;
        }
    }
    if (total == 0) {
        LOG_WARN("isp_get_luma_stats: empty histogram");
        return -1;
    }
    stats->mean_luma = (float)weighted / (float)total * 255.0f / (ISP_HIST_BINS - 1);
    stats->clip_low_pct = (float)low * 100.0f / (float)total;
    stats->clip_high_pct = (float)high * 100.0f / (float)total;
    return 0;
fail:
    return -1;
}

int isp_set_dehaze(isp_block_mode_t mode, unsigned char strength)
{
    ot_isp_dehaze_attr attr;

    memset(&attr, 0, sizeof(attr));
    CHECK_RET_GOTO(ss_mpi_isp_get_dehaze_attr(ISP_PIPE, &attr), fail);
    attr.en = (mode != ISP_BLOCK_OFF) ? TD_TRUE : TD_FALSE;
    attr.op_type = (mode == ISP_BLOCK_MANUAL) ? OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
    if (mode == ISP_BLOCK_MANUAL) {
        attr.manual_attr.strength = strength;
    }
    CHECK_RET_GOTO(ss_mpi_isp_set_dehaze_attr(ISP_PIPE, &attr), fail);
    LOG_INFO("isp dehaze -> mode=%d strength=%u", mode, strength);
    return 0;
fail:
    return -1;
}

int isp_set_drc(isp_block_mode_t mode, unsigned strength)
{
    ot_isp_drc_attr attr;

    memset(&attr, 0, sizeof(attr));
    CHECK_RET_GOTO(ss_mpi_isp_get_drc_attr(ISP_PIPE, &attr), fail);
    attr.enable = (mode != ISP_BLOCK_OFF) ? TD_TRUE : TD_FALSE;
    attr.op_type = (mode == ISP_BLOCK_MANUAL) ? OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
    if (mode == ISP_BLOCK_MANUAL) {
        attr.manual_attr.strength = (td_u16)strength;
    }
    CHECK_RET_GOTO(ss_mpi_isp_set_drc_attr(ISP_PIPE, &attr), fail);
    LOG_INFO("isp drc -> mode=%d strength=%u", mode, strength);
    return 0;
fail:
    return -1;
}

int isp_set_ldci(isp_block_mode_t mode)
{
    ot_isp_ldci_attr attr;

    memset(&attr, 0, sizeof(attr));
    CHECK_RET_GOTO(ss_mpi_isp_get_ldci_attr(ISP_PIPE, &attr), fail);
    attr.en = (mode != ISP_BLOCK_OFF) ? TD_TRUE : TD_FALSE;
    attr.op_type = OT_OP_MODE_AUTO; /* LDCI 无单值手动强度，统一用 SDK 自动参数 */
    CHECK_RET_GOTO(ss_mpi_isp_set_ldci_attr(ISP_PIPE, &attr), fail);
    LOG_INFO("isp ldci -> mode=%d", mode);
    return 0;
fail:
    return -1;
}

/* CLUT（3D-LUT）开关 + 全局 RGB 增益。CoTF 路线的硬件施加端：NN 在低分辨率出 LUT，
 * 由 ISP 硬件做全分辨率三线性施加（零 NPU）—— NPU 无法跑 3D-LUT 三线性(grid_sample)。 */
int isp_set_clut(isp_block_mode_t mode, unsigned gain_r, unsigned gain_g, unsigned gain_b)
{
    ot_isp_clut_attr attr;

    memset(&attr, 0, sizeof(attr));
    CHECK_RET_GOTO(ss_mpi_isp_get_clut_attr(ISP_PIPE, &attr), fail);
    attr.en = (mode != ISP_BLOCK_OFF) ? TD_TRUE : TD_FALSE;
    attr.gain_r = (gain_r > OT_ISP_CLUT_GAIN_MAX) ? OT_ISP_CLUT_GAIN_MAX : gain_r;
    attr.gain_g = (gain_g > OT_ISP_CLUT_GAIN_MAX) ? OT_ISP_CLUT_GAIN_MAX : gain_g;
    attr.gain_b = (gain_b > OT_ISP_CLUT_GAIN_MAX) ? OT_ISP_CLUT_GAIN_MAX : gain_b;
    CHECK_RET_GOTO(ss_mpi_isp_set_clut_attr(ISP_PIPE, &attr), fail);
    LOG_INFO("isp clut -> mode=%d gain=(%u,%u,%u)", mode, attr.gain_r, attr.gain_g, attr.gain_b);
    return 0;
fail:
    return -1;
}

/* 加载 5508 节点 3D-LUT 系数（每节点 u32 打包 3×10bit RGB）。
 * lut 由 models/tools/cotf_lut_pack.py 从 NN 立方 LUT 打包得到；len 必须 == OT_ISP_CLUT_LUT_LENGTH。 */
int isp_load_clut_lut(const unsigned int *lut, unsigned len)
{
    ot_isp_clut_lut clut_lut;

    if (lut == TD_NULL || len != OT_ISP_CLUT_LUT_LENGTH) {
        LOG_ERR("isp_load_clut_lut: lut=%p len=%u (need %d)", (const void *)lut, len,
                OT_ISP_CLUT_LUT_LENGTH);
        return -1;
    }
    memset(&clut_lut, 0, sizeof(clut_lut));
    memcpy(clut_lut.lut, lut, sizeof(clut_lut.lut));
    CHECK_RET_GOTO(ss_mpi_isp_set_clut_coeff(ISP_PIPE, &clut_lut), fail);
    LOG_INFO("isp clut lut loaded (%u nodes)", len);
    return 0;
fail:
    return -1;
}

int isp_clut_get_coeff(unsigned int *lut, unsigned len)
{
    static ot_isp_clut_lut clut_lut;

    if (lut == TD_NULL || len != OT_ISP_CLUT_LUT_LENGTH) {
        LOG_ERR("isp_clut_get_coeff: lut=%p len=%u (need %d)", (void *)lut, len,
                OT_ISP_CLUT_LUT_LENGTH);
        return -1;
    }
    memset(&clut_lut, 0, sizeof(clut_lut));
    CHECK_RET_GOTO(ss_mpi_isp_get_clut_coeff(ISP_PIPE, &clut_lut), fail);
    memcpy(lut, clut_lut.lut, sizeof(clut_lut.lut));
    return 0;
fail:
    return -1;
}

/* 把 tone 曲线 f(x) 离散成 1024-entry 查表（10bit 输入域），施加在节点输出值上（几何无关）。 */
static void build_tone_lut(td_u16 curve[1024], isp_tone_t tone, float strength)
{
    int i;
    float s = (strength < 0.0f) ? 0.0f : (strength > 1.0f ? 1.0f : strength);
    for (i = 0; i < 1024; i++) {
        float x = (float)i / 1023.0f;
        float y = x;
        switch (tone) {
            case ISP_TONE_BRIGHTEN: /* gamma<1 提亮暗部 */
                y = powf(x, 1.0f / (1.0f + 2.0f * s));
                break;
            case ISP_TONE_COMPRESS: /* gamma>1 压制高光 */
                y = powf(x, 1.0f + 1.5f * s);
                break;
            case ISP_TONE_BIDIR: { /* log 曲线：提暗部 + 压高光（动态范围压缩） */
                float k = 1.0f + 12.0f * s;
                y = logf(1.0f + k * x) / logf(1.0f + k);
                break;
            }
            case ISP_TONE_BYPASS:
            default:
                y = x;
                break;
        }
        if (y < 0.0f) y = 0.0f;
        if (y > 1.0f) y = 1.0f;
        curve[i] = (td_u16)(y * 1023.0f + 0.5f);
    }
}

int isp_clut_apply_tone(const unsigned int *base_lut, unsigned len, isp_tone_t tone, float strength)
{
    static unsigned int out[OT_ISP_CLUT_LUT_LENGTH];
    td_u16 curve[1024];
    unsigned i;

    if (base_lut == TD_NULL || len != OT_ISP_CLUT_LUT_LENGTH) {
        LOG_ERR("isp_clut_apply_tone: bad args len=%u", len);
        return -1;
    }
    if (tone == ISP_TONE_BYPASS) {
        return isp_set_clut(ISP_BLOCK_OFF, 1024, 1024, 1024); /* 关 CLUT = 原始相机图 */
    }
    build_tone_lut(curve, tone, strength);
    for (i = 0; i < len; i++) {
        unsigned r = (base_lut[i] >> 20) & 0x3FF;
        unsigned g = (base_lut[i] >> 10) & 0x3FF;
        unsigned b = base_lut[i] & 0x3FF;
        out[i] = ((unsigned)curve[r] << 20) | ((unsigned)curve[g] << 10) | (unsigned)curve[b];
    }
    if (isp_load_clut_lut(out, len) != 0) {
        return -1;
    }
    if (isp_set_clut(ISP_BLOCK_AUTO, 1024, 1024, 1024) != 0) { /* unity gain，启用 */
        return -1;
    }
    LOG_INFO("isp clut tone applied: mode=%d strength=%.2f", tone, strength);
    return 0;
}

#else /* !WITH_SS928_SDK */

/* SDK-free 构建桩。 */

int isp_set_antiflicker(int freq_hz)
{
    (void)freq_hz;
    LOG_ERR("isp: built without SS928 SDK");
    return -1;
}

int isp_set_ae_compensation(unsigned char comp)
{
    (void)comp;
    return -1;
}

int isp_set_exposure_manual(unsigned exp_time_us, unsigned again_x1024)
{
    (void)exp_time_us;
    (void)again_x1024;
    return -1;
}

int isp_set_ae_strategy(int highlight_prior)
{
    (void)highlight_prior;
    return -1;
}

int isp_set_wdr_exposure_ratio(unsigned ratio_x64)
{
    (void)ratio_x64;
    return -1;
}

int isp_get_luma_stats(luma_stats_t *stats)
{
    (void)stats;
    return -1;
}

int isp_set_dehaze(isp_block_mode_t mode, unsigned char strength)
{
    (void)mode;
    (void)strength;
    return -1;
}

int isp_set_drc(isp_block_mode_t mode, unsigned strength)
{
    (void)mode;
    (void)strength;
    return -1;
}

int isp_set_ldci(isp_block_mode_t mode)
{
    (void)mode;
    return -1;
}

int isp_set_clut(isp_block_mode_t mode, unsigned gain_r, unsigned gain_g, unsigned gain_b)
{
    (void)mode;
    (void)gain_r;
    (void)gain_g;
    (void)gain_b;
    return -1;
}

int isp_load_clut_lut(const unsigned int *lut, unsigned len)
{
    (void)lut;
    (void)len;
    return -1;
}

int isp_clut_get_coeff(unsigned int *lut, unsigned len)
{
    (void)lut;
    (void)len;
    return -1;
}

int isp_clut_apply_tone(const unsigned int *base_lut, unsigned len, isp_tone_t tone, float strength)
{
    (void)base_lut;
    (void)len;
    (void)tone;
    (void)strength;
    return -1;
}

#endif /* WITH_SS928_SDK */
