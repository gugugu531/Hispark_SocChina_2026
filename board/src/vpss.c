#include "vpss.h"

#include "log.h"

#ifdef WITH_SS928_SDK

#include <string.h>

#include "sample_comm.h"
#include "ss_mpi_vpss.h"

static td_bool g_chn_enable[OT_VPSS_MAX_PHYS_CHN_NUM];
static int g_vpss_grp = -1;

int vpss_init(int grp, unsigned max_width, unsigned max_height,
              const vpss_chn_cfg_t chn_cfg[VPSS_CHN_COUNT])
{
    ot_vpss_grp_attr grp_attr;
    ot_vpss_chn_attr chn_attr[OT_VPSS_MAX_PHYS_CHN_NUM];
    int i;

    if (g_vpss_grp >= 0) {
        LOG_WARN("vpss grp %d already inited", g_vpss_grp);
        return 0;
    }
    if (chn_cfg == NULL) {
        return -1;
    }

    memset(&grp_attr, 0, sizeof(grp_attr));
    memset(chn_attr, 0, sizeof(chn_attr));
    memset(g_chn_enable, 0, sizeof(g_chn_enable));

    sample_comm_vpss_get_default_grp_attr(&grp_attr);
    grp_attr.max_width = max_width;
    grp_attr.max_height = max_height;

    for (i = 0; i < VPSS_CHN_COUNT; i++) {
        if (!chn_cfg[i].enable) {
            continue;
        }
        g_chn_enable[i] = TD_TRUE;
        sample_comm_vpss_get_default_chn_attr(&chn_attr[i]);
        chn_attr[i].width = chn_cfg[i].width;
        chn_attr[i].height = chn_cfg[i].height;
        chn_attr[i].depth = chn_cfg[i].depth;
        chn_attr[i].compress_mode = OT_COMPRESS_MODE_NONE;
        chn_attr[i].pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    }

    CHECK_RET_GOTO(
        sample_common_vpss_start(grp, g_chn_enable, &grp_attr, chn_attr, OT_VPSS_MAX_PHYS_CHN_NUM),
        fail);
    g_vpss_grp = grp;
    LOG_INFO("vpss up: grp%d in=%ux%u chn0=%s chn1=%s chn2=%s", grp, max_width, max_height,
             g_chn_enable[0] ? "on" : "off", g_chn_enable[1] ? "on" : "off",
             g_chn_enable[2] ? "on" : "off");
    return 0;

fail:
    return -1;
}

int vpss_get_frame(int grp, int chn, void *frame_info, int timeout_ms)
{
    td_s32 ret;

    if (frame_info == NULL) {
        return -1;
    }
    ret = ss_mpi_vpss_get_chn_frame(grp, chn, (ot_video_frame_info *)frame_info, timeout_ms);
    if (ret != TD_SUCCESS) {
        LOG_ERR("ss_mpi_vpss_get_chn_frame(grp%d chn%d) failed: %#x", grp, chn, (unsigned)ret);
        return -1;
    }
    return 0;
}

int vpss_release_frame(int grp, int chn, const void *frame_info)
{
    td_s32 ret;

    if (frame_info == NULL) {
        return -1;
    }
    ret = ss_mpi_vpss_release_chn_frame(grp, chn, (const ot_video_frame_info *)frame_info);
    if (ret != TD_SUCCESS) {
        LOG_ERR("ss_mpi_vpss_release_chn_frame(grp%d chn%d) failed: %#x", grp, chn, (unsigned)ret);
        return -1;
    }
    return 0;
}

int vpss_deinit(int grp)
{
    if (g_vpss_grp < 0) {
        return 0;
    }
    if (grp != g_vpss_grp) {
        LOG_ERR("vpss_deinit: grp %d not inited (current %d)", grp, g_vpss_grp);
        return -1;
    }
    CHECK_RET(sample_common_vpss_stop(grp, g_chn_enable, OT_VPSS_MAX_PHYS_CHN_NUM));
    g_vpss_grp = -1;
    LOG_INFO("vpss down: grp%d", grp);
    return 0;
}

#else /* !WITH_SS928_SDK */

/* SDK-free 构建桩。 */

int vpss_init(int grp, unsigned max_width, unsigned max_height,
              const vpss_chn_cfg_t chn_cfg[VPSS_CHN_COUNT])
{
    (void)grp;
    (void)max_width;
    (void)max_height;
    (void)chn_cfg;
    LOG_ERR("vpss_init: built without SS928 SDK");
    return -1;
}

int vpss_get_frame(int grp, int chn, void *frame_info, int timeout_ms)
{
    (void)grp;
    (void)chn;
    (void)frame_info;
    (void)timeout_ms;
    return -1;
}

int vpss_release_frame(int grp, int chn, const void *frame_info)
{
    (void)grp;
    (void)chn;
    (void)frame_info;
    return -1;
}

int vpss_deinit(int grp)
{
    (void)grp;
    return 0;
}

#endif /* WITH_SS928_SDK */
