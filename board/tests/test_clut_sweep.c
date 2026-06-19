/* 在一次锁曝光相机运行中，对 CLUT identity 候选计算相对 OFF 基线的 NV21 MAE。 */

#include "capture.h"
#include "isp.h"
#include "log.h"
#include "vpss.h"

#ifndef WITH_SS928_SDK
int main(void) { return 0; }
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ot_buffer.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_type.h"
#include "sample_comm.h"
#include "ss_mpi_sys.h"

#define CANDIDATE_BYTES (PIPELINE_ISP_CLUT_NODE_COUNT * sizeof(unsigned int))

static int copy_nv21(const ot_video_frame_info *frame, unsigned char *dst, size_t dst_size)
{
    const ot_video_frame *vf = &frame->video_frame;
    td_u32 row, map_size = vf->stride[0] * vf->height * 3 / 2;
    td_u8 *src;
    if (dst_size != (size_t)vf->width * vf->height * 3 / 2) return -1;
    src = (td_u8 *)ss_mpi_sys_mmap(vf->phys_addr[0], map_size);
    if (src == TD_NULL) return -1;
    for (row = 0; row < vf->height; row++)
        memcpy(dst + (size_t)row * vf->width, src + (size_t)row * vf->stride[0], vf->width);
    for (row = 0; row < vf->height / 2; row++)
        memcpy(dst + (size_t)vf->width * vf->height + (size_t)row * vf->width,
               src + (size_t)vf->stride[0] * vf->height + (size_t)row * vf->stride[1],
               vf->width);
    (void)ss_mpi_sys_munmap(src, map_size);
    return 0;
}

static double mae(const unsigned char *a, const unsigned char *b, size_t n)
{
    unsigned long long sum = 0;
    size_t i;
    for (i = 0; i < n; i++) sum += (a[i] > b[i]) ? a[i] - b[i] : b[i] - a[i];
    return (double)sum / n;
}

int main(int argc, char **argv)
{
    const char *bundle = NULL;
    int count = 36, sensor = CAPTURE_SENSOR_INDEX_DEFAULT, rc = 1;
    FILE *fp = NULL;
    unsigned *table = NULL;
    unsigned char *baseline = NULL, *current = NULL;
    size_t frame_bytes = PIPELINE_DISPLAY_WIDTH * PIPELINE_DISPLAY_HEIGHT * 3 / 2;
    int sys_up = 0, cap_up = 0, vpss_up = 0, bound = 0;
    unsigned in_w = 0, in_h = 0;
    ot_vb_cfg vb_cfg = {0};
    ot_pic_buf_attr buf_attr = {0};
    ot_vb_calc_cfg calc_cfg = {0};
    ot_video_frame_info frame;
    vpss_chn_cfg_t cfg[VPSS_CHN_COUNT] = {
        {1, PIPELINE_DISPLAY_WIDTH, PIPELINE_DISPLAY_HEIGHT, 2},
        {0, 0, 0, 0}, {0, 0, 0, 0},
    };
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bundle") == 0 && i + 1 < argc) bundle = argv[++i];
        else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) count = atoi(argv[++i]);
        else if (strcmp(argv[i], "--sensor") == 0 && i + 1 < argc) sensor = atoi(argv[++i]);
    }
    if (bundle == NULL || count <= 0) return 2;
    fp = fopen(bundle, "rb");
    table = malloc(CANDIDATE_BYTES);
    baseline = malloc(frame_bytes);
    current = malloc(frame_bytes);
    if (fp == NULL || table == NULL || baseline == NULL || current == NULL ||
        capture_query_in_size(&in_w, &in_h) != 0) goto cleanup;
    buf_attr.width = in_w; buf_attr.height = in_h; buf_attr.align = OT_DEFAULT_ALIGN;
    buf_attr.bit_width = OT_DATA_BIT_WIDTH_8;
    buf_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    buf_attr.compress_mode = OT_COMPRESS_MODE_SEG;
    ot_common_get_pic_buf_cfg(&buf_attr, &calc_cfg);
    vb_cfg.max_pool_cnt = 2; vb_cfg.common_pool[0].blk_size = calc_cfg.vb_size;
    vb_cfg.common_pool[0].blk_cnt = 10;
    CHECK_RET_GOTO(sample_comm_sys_init_with_vb_supplement(&vb_cfg, OT_VB_SUPPLEMENT_BNR_MOT_MASK),
                   cleanup);
    sys_up = 1;
    {
        capture_cfg_t cap = {sensor, CAPTURE_MODE_LINEAR};
        if (capture_init(&cap) != 0) goto cleanup;
    }
    cap_up = 1;
    if (isp_set_clut(ISP_BLOCK_OFF, 1024, 1024, 1024) != 0) goto cleanup;
    (void)isp_set_exposure_manual(10000, 1024);
    if (vpss_init(0, in_w, in_h, cfg) != 0) goto cleanup;
    vpss_up = 1;
    if (capture_bind_vpss(0) != 0) goto cleanup;
    bound = 1;
    /* 先让 AE/AWB/管线稳定；每个候选再抓相邻 OFF 基线，降低 AWB/场景漂移误差。 */
    for (int i = 0; i < 45; i++) {
        if (vpss_get_frame(0, 0, &frame, 1000) != 0) goto cleanup;
        (void)vpss_release_frame(0, 0, &frame);
    }
    for (int c = 0; c < count; c++) {
        if (isp_set_clut(ISP_BLOCK_OFF, 1024, 1024, 1024) != 0) goto cleanup;
        for (int i = 0; i < 3; i++) {
            if (vpss_get_frame(0, 0, &frame, 1000) != 0) goto cleanup;
            if (i == 2 && copy_nv21(&frame, baseline, frame_bytes) != 0) goto cleanup;
            (void)vpss_release_frame(0, 0, &frame);
        }
        if (fread(table, 1, CANDIDATE_BYTES, fp) != CANDIDATE_BYTES ||
            isp_load_clut_lut(table, PIPELINE_ISP_CLUT_NODE_COUNT) != 0 ||
            isp_set_clut(ISP_BLOCK_AUTO, 1024, 1024, 1024) != 0) goto cleanup;
        for (int i = 0; i < 3; i++) {
            if (vpss_get_frame(0, 0, &frame, 1000) != 0) goto cleanup;
            if (i == 2 && copy_nv21(&frame, current, frame_bytes) != 0) goto cleanup;
            (void)vpss_release_frame(0, 0, &frame);
        }
        {
            size_t y_bytes = PIPELINE_DISPLAY_WIDTH * PIPELINE_DISPLAY_HEIGHT;
            LOG_INFO("CLUT_SWEEP index=%d mae=%.6f y_mae=%.6f uv_mae=%.6f", c,
                     mae(baseline, current, frame_bytes),
                     mae(baseline, current, y_bytes),
                     mae(baseline + y_bytes, current + y_bytes, frame_bytes - y_bytes));
        }
    }
    rc = 0;
cleanup:
    if (cap_up) {
        (void)isp_set_clut(ISP_BLOCK_OFF, 1024, 1024, 1024);
        (void)isp_set_exposure_manual(0, 0);
    }
    if (bound) (void)capture_unbind_vpss(0);
    if (vpss_up) (void)vpss_deinit(0);
    if (cap_up) (void)capture_deinit();
    if (sys_up) sample_comm_sys_exit();
    if (fp) fclose(fp);
    free(table); free(baseline); free(current);
    return rc;
}
#endif
