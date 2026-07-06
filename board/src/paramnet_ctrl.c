/* ParamNet 实时闭环一拍（路线 B2）。见 paramnet_ctrl.h。
 * 复用通用 infer.c（NV21 → float 输出）+ paramnet_map（u→θ→blob）+ isp buffer 施加。 */

#include "paramnet_ctrl.h"

#include "infer.h"
#include "isp.h"
#include "log.h"
#include "paramnet_map.h"

#include <stddef.h>

int paramnet_run_and_apply(const void *frame_info)
{
    float u[PARAMNET_U_DIM];
    float theta[PARAMNET_THETA_DIM];
    unsigned char blob[PARAMNET_BLOB_MAX];
    infer_timing_t timing;
    size_t n;
    int rc;

    if (frame_info == NULL) {
        return -1;
    }

    /* 1) chn2 NV21 → ParamNet OM → u[30]（AIPP 在 OM 前端做 NV21→RGB→/255→fp16）。
     *    infer 通用封装：out_size = 30×fp16 时自动 half→float 到 u[]。 */
    rc = infer_run_nv21(frame_info, u, PARAMNET_U_DIM, &timing);
    if (rc != 0) {
        LOG_ERR("[paramnet] infer failed rc=%d", rc);
        return rc;
    }

    /* 2) u → θ（与训练侧 u_to_theta 逐段同映射） */
    paramnet_u_to_theta(u, theta);

    /* 3) θ → v3 blob（内存，免落盘；gamma_on，DRC/LDCI 段 enable） */
    n = paramnet_theta_to_blob(theta, /*drc_on=*/1, /*ldci_on=*/1,
                               /*gamma_on=*/1, /*gamma_strength=*/1.0f,
                               blob, sizeof(blob));
    if (n == 0) {
        LOG_ERR("[paramnet] blob build failed");
        return -1;
    }

    /* 4) 施加到 ISP（DRC/LDCI/Gamma 热刷新） */
    rc = isp_apply_blob_buffer(blob, (unsigned long)n);
    if (rc != 0) {
        LOG_ERR("[paramnet] isp apply failed rc=%d", rc);
        return rc;
    }

    LOG_INFO("[paramnet] applied (infer %.2fms)", timing.total_ms);
    return 0;
}
