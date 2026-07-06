#define _GNU_SOURCE   /* fmemopen（内存 buffer 施加 blob） */

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

static ot_isp_gamma_attr g_gamma_default;
static int g_gamma_cached = 0;

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

/* 把归一化值 x∈[0,1] 过 tone 曲线（与 build_tone_lut 同一组曲线，浮点版）。 */
static float tone_map(float x, isp_tone_t tone, float s)
{
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    switch (tone) {
        case ISP_TONE_BRIGHTEN: return powf(x, 1.0f / (1.0f + 2.0f * s));
        case ISP_TONE_COMPRESS: return powf(x, 1.0f + 1.5f * s);
        case ISP_TONE_BIDIR: {
            float k = 1.0f + 12.0f * s;
            return logf(1.0f + k * x) / logf(1.0f + k);
        }
        default: return x;
    }
}

/* 原生 Gamma 块色调校正：首次缓存默认 Gamma 作基线，把 tone 叠在其输出上写回 USER_DEFINE。 */
int isp_gamma_apply_tone(isp_tone_t tone, float strength)
{
    ot_isp_gamma_attr attr;
    int i, base_valid;
    float s = (strength < 0.0f) ? 0.0f : (strength > 1.0f ? 1.0f : strength);

    memset(&attr, 0, sizeof(attr));
    CHECK_RET_GOTO(ss_mpi_isp_get_gamma_attr(ISP_PIPE, &attr), fail);
    if (!g_gamma_cached) {
        g_gamma_default = attr; /* 缓存上电默认 Gamma（曲线+表）作基线 */
        g_gamma_cached = 1;
    }
    if (tone == ISP_TONE_BYPASS) {
        CHECK_RET_GOTO(ss_mpi_isp_set_gamma_attr(ISP_PIPE, &g_gamma_default), fail);
        LOG_INFO("isp gamma -> bypass (restore default)");
        return 0;
    }
    /* 基线表：默认表单调有效则用之，否则退化用 sRGB 近似 (x^(1/2.2))。 */
    base_valid = (g_gamma_default.table[OT_ISP_GAMMA_NODE_NUM - 1] > g_gamma_default.table[0]);
    attr = g_gamma_default;
    for (i = 0; i < OT_ISP_GAMMA_NODE_NUM; i++) {
        float x = (float)i / (float)(OT_ISP_GAMMA_NODE_NUM - 1);
        float base = base_valid ? (float)g_gamma_default.table[i] / 4095.0f : powf(x, 1.0f / 2.2f);
        float y = tone_map(base, tone, s) * 4095.0f + 0.5f;
        attr.table[i] = (td_u16)((y > 4095.0f) ? 4095.0f : y);
    }
    attr.enable = TD_TRUE;
    attr.curve_type = OT_ISP_GAMMA_CURVE_USER_DEFINE;
    CHECK_RET_GOTO(ss_mpi_isp_set_gamma_attr(ISP_PIPE, &attr), fail);
    LOG_INFO("isp gamma tone applied: mode=%d strength=%.2f (base=%s)", tone, strength,
             base_valid ? "default-table" : "sRGB");
    return 0;
fail:
    return -1;
}

int isp_gamma_apply_curve(const float *curve, unsigned nodes, float strength)
{
    ot_isp_gamma_attr attr;
    float safe[64];
    float s;
    int i, base_valid;
    unsigned j;

    if (curve == TD_NULL || nodes < 2 || nodes > 64) {
        LOG_ERR("isp_gamma_apply_curve: curve=%p nodes=%u", (const void *)curve, nodes);
        return -1;
    }
    s = (strength < 0.0f) ? 0.0f : (strength > 1.0f ? 1.0f : strength);
    memset(&attr, 0, sizeof(attr));
    CHECK_RET_GOTO(ss_mpi_isp_get_gamma_attr(ISP_PIPE, &attr), fail);
    if (!g_gamma_cached) {
        g_gamma_default = attr;
        g_gamma_cached = 1;
    }
    if (s == 0.0f) {
        CHECK_RET_GOTO(ss_mpi_isp_set_gamma_attr(ISP_PIPE, &g_gamma_default), fail);
        LOG_INFO("isp gamma curve -> bypass (restore default)");
        return 0;
    }

    safe[0] = 0.0f;
    for (j = 1; j + 1 < nodes; j++) {
        float value = isfinite(curve[j]) ? curve[j] : safe[j - 1];
        if (value < safe[j - 1]) value = safe[j - 1];
        if (value > 1.0f) value = 1.0f;
        safe[j] = value;
    }
    safe[nodes - 1] = 1.0f;

    base_valid = (g_gamma_default.table[OT_ISP_GAMMA_NODE_NUM - 1] > g_gamma_default.table[0]);
    attr = g_gamma_default;
    for (i = 0; i < OT_ISP_GAMMA_NODE_NUM; i++) {
        float x = (float)i / (float)(OT_ISP_GAMMA_NODE_NUM - 1);
        float base = base_valid ? (float)g_gamma_default.table[i] / 4095.0f : powf(x, 1.0f / 2.2f);
        float pos = base * (nodes - 1);
        unsigned lo = (unsigned)pos;
        float frac, mapped, mixed;
        if (lo >= nodes - 1) {
            lo = nodes - 2;
            frac = 1.0f;
        } else {
            frac = pos - lo;
        }
        mapped = safe[lo] + frac * (safe[lo + 1] - safe[lo]);
        mixed = base + s * (mapped - base);
        if (mixed < 0.0f) mixed = 0.0f;
        if (mixed > 1.0f) mixed = 1.0f;
        attr.table[i] = (td_u16)(mixed * 4095.0f + 0.5f);
    }
    attr.enable = TD_TRUE;
    attr.curve_type = OT_ISP_GAMMA_CURVE_USER_DEFINE;
    CHECK_RET_GOTO(ss_mpi_isp_set_gamma_attr(ISP_PIPE, &attr), fail);
    LOG_INFO("isp gamma model curve applied: nodes=%u strength=%.2f", nodes, s);
    return 0;
fail:
    return -1;
}

/* ---- ISP 参数 blob 加载（DRC/LDCI 完整参数验证用）---- */

#define ISP_BLOB_MAGIC       0x49535000u /* "ISP\0" */
#define ISP_BLOB_VERSION_MIN 1u
#define ISP_BLOB_VERSION_MAX 3u
/* v2: DRC 段末尾追加 manual strength (u16, 0-1023)
 * v3: DRC 段再追加 Filter 主通路 local_mixing_bright/dark[33]（v2 只写了 FilterX 通路，
 *     blend 偏向主通路时 X 通路 mixing 无效应）；新增可选段（按 flags 位序读）：
 *     bit2=GAMMA(u16 curve[64] 0-65535 + u16 strength 0-1024，经 isp_gamma_apply_curve)
 *     bit3=DRC_GUARD(u8 bright_gain_limit/step + u8 dark_gain_limit_luma/chroma)
 *     bit4=DRC_COLOR(u8 cc_ctrl/low_sat/high_sat + u16 color_correction_lut[33])
 *     bit3/bit4 属 DRC 属性子段，仅在 bit0 置位时有效。 */

/* 从已打开的 FILE*（文件或 fmemopen 的内存）解析并施加 blob；不关闭 fp。
 * 解析逻辑单一来源，文件入口与实时闭环的内存 buffer 入口共用。 */
static int isp_apply_blob_fp(FILE *fp)
{
    uint32_t magic, version, flags;
    int ret = -1;

    /* 读取头部 */
    if (fread(&magic,   4, 1, fp) != 1 ||
        fread(&version, 4, 1, fp) != 1 ||
        fread(&flags,   4, 1, fp) != 1) {
        LOG_ERR("isp_load_blob: header read failed");
        goto cleanup;
    }
    if (magic != ISP_BLOB_MAGIC || version < ISP_BLOB_VERSION_MIN ||
        version > ISP_BLOB_VERSION_MAX) {
        LOG_ERR("isp_load_blob: bad magic %#x or version %u", magic, version);
        goto cleanup;
    }

    /* DRC 参数 */
    if (flags & 1) {
        ot_isp_drc_attr drc;
        uint32_t drc_enable_raw;
        uint16_t tmv[200];
        uint8_t  bx[33], dx[33];

        if (fread(&drc_enable_raw, 4, 1, fp) != 1 ||
            fread(tmv, 2, 200, fp) != 200 ||
            fread(bx,  1,  33, fp) !=  33 ||
            fread(dx,  1,  33, fp) !=  33) {
            LOG_ERR("isp_load_blob: DRC data read failed");
            goto cleanup;
        }

        memset(&drc, 0, sizeof(drc));
        if (ss_mpi_isp_get_drc_attr(ISP_PIPE, &drc) != 0) {
            LOG_ERR("isp_load_blob: get drc attr failed");
            goto cleanup;
        }

        drc.enable = (drc_enable_raw != 0) ? TD_TRUE : TD_FALSE;
        drc.op_type = OT_OP_MODE_MANUAL;
        drc.curve_select = OT_ISP_DRC_CURVE_USER;
        memcpy(drc.tone_mapping_value, tmv, sizeof(tmv));
        memcpy(drc.local_mixing_bright_x, bx, sizeof(bx));
        memcpy(drc.local_mixing_dark_x, dx, sizeof(dx));

        /* 读取剩余标量参数 */
        if (fread(&drc.spatial_filter_coef, 1, 1, fp) != 1 ||
            fread(&drc.range_filter_coef,   1, 1, fp) != 1 ||
            fread(&drc.contrast_ctrl,       1, 1, fp) != 1 ||
            fread(&drc.blend_luma_max,      1, 1, fp) != 1) {
            LOG_ERR("isp_load_blob: DRC scalar read failed");
            goto cleanup;
        }
        /* manual strength 是独立于 tone 曲线的整体亮度维度（SDK：值越大越亮），
         * v1 无此字段时保持历史默认 512。 */
        drc.manual_attr.strength = 512;
        if (version >= 2) {
            uint16_t strength;
            if (fread(&strength, 2, 1, fp) != 1) {
                LOG_ERR("isp_load_blob: DRC strength read failed");
                goto cleanup;
            }
            drc.manual_attr.strength = (strength > 1023) ? 1023 : strength;
        }

        /* v3: Filter 主通路 local mixing（v2 只写 FilterX 通路） */
        if (version >= 3) {
            uint8_t lmb[33], lmd[33];
            if (fread(lmb, 1, 33, fp) != 33 || fread(lmd, 1, 33, fp) != 33) {
                LOG_ERR("isp_load_blob: DRC main-path mixing read failed");
                goto cleanup;
            }
            memcpy(drc.local_mixing_bright, lmb, sizeof(lmb));
            memcpy(drc.local_mixing_dark, lmd, sizeof(lmd));
        }

        /* v3 可选子段：DRC 护栏（亮区增益上限 = 高光抑制直控；暗区增益护栏） */
        if (version >= 3 && (flags & 8)) {
            uint8_t g[4];
            if (fread(g, 1, 4, fp) != 4) {
                LOG_ERR("isp_load_blob: DRC guard read failed");
                goto cleanup;
            }
            drc.bright_gain_limit = g[0];
            drc.bright_gain_limit_step = g[1];
            drc.dark_gain_limit_luma = g[2];
            drc.dark_gain_limit_chroma = g[3];
        }

        /* v3 可选子段：DRC 色彩补偿（提亮后饱和度） */
        if (version >= 3 && (flags & 16)) {
            uint8_t c[3];
            uint16_t cc_lut[33];
            if (fread(c, 1, 3, fp) != 3 || fread(cc_lut, 2, 33, fp) != 33) {
                LOG_ERR("isp_load_blob: DRC color read failed");
                goto cleanup;
            }
            drc.color_correction_ctrl = (c[0] != 0) ? TD_TRUE : TD_FALSE;
            drc.low_saturation_color_ctrl = c[1];
            drc.high_saturation_color_ctrl = c[2];
            memcpy(drc.color_correction_lut, cc_lut, sizeof(cc_lut));
        }

        if (ss_mpi_isp_set_drc_attr(ISP_PIPE, &drc) != 0) {
            LOG_ERR("isp_load_blob: set drc attr failed");
            goto cleanup;
        }
        LOG_INFO("isp_load_blob: DRC applied (enable=%d, strength=%u, guard=%d, color=%d)",
                 drc_enable_raw, drc.manual_attr.strength, (flags >> 3) & 1, (flags >> 4) & 1);
    }

    /* LDCI 参数 */
    if (flags & 2) {
        ot_isp_ldci_attr ldci;
        uint32_t ldci_enable_raw;
        uint8_t  hp[3], hn[3];
        uint16_t blc;
        uint8_t  sigma;

        if (fread(&ldci_enable_raw, 4, 1, fp) != 1 ||
            fread(hp, 1, 3, fp) != 3 ||
            fread(hn, 1, 3, fp) != 3 ||
            fread(&blc,   2, 1, fp) != 1 ||
            fread(&sigma, 1, 1, fp) != 1) {
            LOG_ERR("isp_load_blob: LDCI data read failed");
            goto cleanup;
        }

        memset(&ldci, 0, sizeof(ldci));
        if (ss_mpi_isp_get_ldci_attr(ISP_PIPE, &ldci) != 0) {
            LOG_ERR("isp_load_blob: get ldci attr failed");
            goto cleanup;
        }

        ldci.en = (ldci_enable_raw != 0) ? TD_TRUE : TD_FALSE;
        ldci.op_type = OT_OP_MODE_MANUAL;
        ldci.manual_attr.he_wgt.he_pos_wgt.wgt   = hp[0];
        ldci.manual_attr.he_wgt.he_pos_wgt.sigma = hp[1];
        ldci.manual_attr.he_wgt.he_pos_wgt.mean  = hp[2];
        ldci.manual_attr.he_wgt.he_neg_wgt.wgt   = hn[0];
        ldci.manual_attr.he_wgt.he_neg_wgt.sigma = hn[1];
        ldci.manual_attr.he_wgt.he_neg_wgt.mean  = hn[2];
        ldci.manual_attr.blc_ctrl = blc;
        ldci.gauss_lpf_sigma = sigma;

        if (ss_mpi_isp_set_ldci_attr(ISP_PIPE, &ldci) != 0) {
            LOG_ERR("isp_load_blob: set ldci attr failed");
            goto cleanup;
        }
        LOG_INFO("isp_load_blob: LDCI applied (enable=%d)", ldci_enable_raw);
    }

    /* v3: Gamma 段（64 节点归一化曲线，经 isp_gamma_apply_curve 叠加施加） */
    if (version >= 3 && (flags & 4)) {
        uint16_t curve_u16[64], strength_u16;
        float curve[64];
        int i;

        if (fread(curve_u16, 2, 64, fp) != 64 || fread(&strength_u16, 2, 1, fp) != 1) {
            LOG_ERR("isp_load_blob: gamma read failed");
            goto cleanup;
        }
        for (i = 0; i < 64; i++) {
            curve[i] = (float)curve_u16[i] / 65535.0f;
        }
        if (isp_gamma_apply_curve(curve, 64, (float)strength_u16 / 1024.0f) != 0) {
            LOG_ERR("isp_load_blob: gamma apply failed");
            goto cleanup;
        }
        LOG_INFO("isp_load_blob: gamma applied (strength=%u/1024)", strength_u16);
    }

    ret = 0;
cleanup:
    return ret;   /* fp 由调用方关闭 */
}

/* 从内存 buffer 施加 blob（实时闭环：paramnet_theta_to_blob 产物 → 此处，免落盘）。 */
int isp_apply_blob_buffer(const unsigned char *buf, unsigned long len)
{
    FILE *fp = fmemopen((void *)buf, (size_t)len, "rb");
    if (fp == NULL) {
        LOG_ERR("isp_apply_blob_buffer: fmemopen failed");
        return -1;
    }
    int rc = isp_apply_blob_fp(fp);
    fclose(fp);
    return rc;
}

/* 从文件加载并施加 blob（诊断/离线 load_isp 用）。 */
int isp_load_blob_and_apply(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        LOG_ERR("isp_load_blob: cannot open %s", path);
        return -1;
    }
    int rc = isp_apply_blob_fp(fp);
    fclose(fp);
    return rc;
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

int isp_gamma_apply_tone(isp_tone_t tone, float strength)
{
    (void)tone;
    (void)strength;
    return -1;
}

int isp_gamma_apply_curve(const float *curve, unsigned nodes, float strength)
{
    (void)curve;
    (void)nodes;
    (void)strength;
    return -1;
}

int isp_load_blob_and_apply(const char *path)
{
    (void)path;
    return -1;
}

int isp_apply_blob_buffer(const unsigned char *buf, unsigned long len)
{
    (void)buf;
    (void)len;
    return -1;
}

#endif /* WITH_SS928_SDK */
