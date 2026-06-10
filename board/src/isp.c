#include "isp.h"

#include "log.h"

#ifdef WITH_SS928_SDK

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

#endif /* WITH_SS928_SDK */
