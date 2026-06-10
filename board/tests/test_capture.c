/* test_capture — 相机采集链板端冒烟测试（触硬件，交叉编译后上板手动运行）。
 *
 * 链路：OS08A20 → VI(online) → ISP → VPSS chn0 1024x600 NV21 → CPU 取帧
 *       （--display 时帧再经 display 模块零拷贝送 HDMI，即阶段 A 最小直通链）。
 *
 * 用法：./test_capture [秒数] [选项]
 *   --display              帧送 HDMI 上屏
 *   --dump <文件>          第一帧 NV21 落盘（Y + VU 平面，1024x600）
 *   --sensor <0|1>         传感器位（默认 1，海鸥派 OS08A20 在 sensor1/J4）
 *   --wdr                  WDR 2to1 模式（默认 linear；架构 §6 点 6 实测项）
 *   --fps <f>              运行时帧率（linear 1.06–30 / WDR 5–30）
 *   --mirror / --flip      VI 镜像 / 翻转
 *   --flicker <50|60>      抗闪烁频率
 *   --aecomp <0-255>       AE 曝光补偿（默认约 0x38；小→压高光）
 *   --dehaze <0|1|2> [强度] / --drc <0|1|2> [强度] / --ldci <0|1>
 *                          增强块：0 关 / 1 自动 / 2 手动(带强度)
 *   运行中每 2s 打印 fps 与 AE luma 统计（mean/clip%，§6 点 4 验证）。
 * 前置：板上接有 OS08A20，且无其他媒体进程争用 SYS/VB/VI/VPSS/VO。
 */

#include "capture.h"
#include "display.h"
#include "isp.h"
#include "log.h"
#include "vpss.h"

#ifndef WITH_SS928_SDK

int main(void)
{
    LOG_WARN("test_capture skipped: built without SS928 SDK (host build)");
    return 0;
}

#else /* WITH_SS928_SDK */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ot_buffer.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_type.h"
#include "sample_comm.h"
#include "ss_mpi_sys.h"

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 把一帧 NV21（Y + 交织 VU）按行落盘，去除 stride 填充。 */
static int dump_nv21_frame(const ot_video_frame_info *frame, const char *path)
{
    const ot_video_frame *vf = &frame->video_frame;
    td_u32 row;
    int rc = -1;
    FILE *fp = NULL;
    td_u32 map_size = vf->stride[0] * vf->height * 3 / 2;
    td_u8 *virt = (td_u8 *)ss_mpi_sys_mmap(vf->phys_addr[0], map_size);

    if (virt == TD_NULL) {
        LOG_ERR("dump: ss_mpi_sys_mmap failed");
        return -1;
    }
    fp = fopen(path, "wb");
    if (fp == NULL) {
        LOG_ERR("dump: cannot open %s", path);
        goto unmap;
    }
    for (row = 0; row < vf->height; row++) {
        fwrite(virt + (size_t)row * vf->stride[0], 1, vf->width, fp);
    }
    for (row = 0; row < vf->height / 2; row++) {
        fwrite(virt + (size_t)vf->stride[0] * vf->height + (size_t)row * vf->stride[1], 1, vf->width, fp);
    }
    LOG_INFO("dump: %ux%u NV21 -> %s", vf->width, vf->height, path);
    rc = 0;

    fclose(fp);
unmap:
    CHECK_RET(ss_mpi_sys_munmap(virt, map_size));
    return rc;
}

int main(int argc, char **argv)
{
    int run_sec = 10;
    int use_display = 0;
    capture_cfg_t cap_cfg = {CAPTURE_SENSOR_INDEX_DEFAULT, CAPTURE_MODE_LINEAR};
    float fps = 0.0f;
    int mirror = 0, flip = 0, flicker_hz = 0, ae_comp = -1;
    int dehaze_mode = -1, drc_mode = -1, ldci_mode = -1;
    unsigned dehaze_strength = 0, drc_strength = 0;
    const char *dump_path = NULL;
    int rc = 1;
    int sys_up = 0, cap_up = 0, vpss_up = 0, bound = 0, disp_up = 0;
    unsigned in_w = 0, in_h = 0;
    int frames = 0, dumped = 0;
    long t0, t_last_log, t_get;
    long get_ms_max = 0;
    ot_vb_cfg vb_cfg = {0};
    ot_pic_buf_attr buf_attr = {0};
    ot_vb_calc_cfg calc_cfg = {0};
    ot_video_frame_info frame;
    vpss_chn_cfg_t chn_cfg[VPSS_CHN_COUNT] = {
        {1, DISPLAY_WIDTH, DISPLAY_HEIGHT, 2}, /* chn0: 显示底图 */
        {0, 0, 0, 0},
        {0, 0, 0, 0},
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--display") == 0) {
            use_display = 1;
        } else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dump_path = argv[++i];
        } else if (strcmp(argv[i], "--sensor") == 0 && i + 1 < argc) {
            cap_cfg.sensor_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--wdr") == 0) {
            cap_cfg.mode = CAPTURE_MODE_WDR_2TO1;
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            fps = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--mirror") == 0) {
            mirror = 1;
        } else if (strcmp(argv[i], "--flip") == 0) {
            flip = 1;
        } else if (strcmp(argv[i], "--flicker") == 0 && i + 1 < argc) {
            flicker_hz = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--aecomp") == 0 && i + 1 < argc) {
            ae_comp = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dehaze") == 0 && i + 1 < argc) {
            dehaze_mode = atoi(argv[++i]);
            if (dehaze_mode == 2 && i + 1 < argc) {
                dehaze_strength = (unsigned)atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "--drc") == 0 && i + 1 < argc) {
            drc_mode = atoi(argv[++i]);
            if (drc_mode == 2 && i + 1 < argc) {
                drc_strength = (unsigned)atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "--ldci") == 0 && i + 1 < argc) {
            ldci_mode = atoi(argv[++i]);
        } else {
            run_sec = atoi(argv[i]);
        }
    }

    if (capture_query_in_size(&in_w, &in_h) != 0) {
        return 1;
    }
    LOG_INFO("test_capture: %ds sensor=%d mode=%s in=%ux%u display=%d dump=%s", run_sec,
             cap_cfg.sensor_index, (cap_cfg.mode == CAPTURE_MODE_WDR_2TO1) ? "wdr2to1" : "linear",
             in_w, in_h, use_display, dump_path ? dump_path : "off");

    /* VB 公共池按传感器全幅 NV21（SEG 压缩）配置，与已验证相机流程一致。 */
    buf_attr.width = in_w;
    buf_attr.height = in_h;
    buf_attr.align = OT_DEFAULT_ALIGN;
    buf_attr.bit_width = OT_DATA_BIT_WIDTH_8;
    buf_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    buf_attr.compress_mode = OT_COMPRESS_MODE_SEG;
    ot_common_get_pic_buf_cfg(&buf_attr, &calc_cfg);
    vb_cfg.max_pool_cnt = 2;
    vb_cfg.common_pool[0].blk_size = calc_cfg.vb_size;
    vb_cfg.common_pool[0].blk_cnt = 10;
    CHECK_RET_GOTO(sample_comm_sys_init_with_vb_supplement(&vb_cfg, OT_VB_SUPPLEMENT_BNR_MOT_MASK),
                   cleanup);
    sys_up = 1;

    if (capture_init(&cap_cfg) != 0) {
        goto cleanup;
    }
    cap_up = 1;

    /* 运行时控制项（任一失败仅报错，不中断链路冒烟） */
    if (fps > 0.0f) {
        (void)capture_set_fps(fps);
    }
    if (mirror || flip) {
        (void)capture_set_mirror_flip(mirror, flip);
    }
    if (flicker_hz != 0) {
        (void)isp_set_antiflicker(flicker_hz);
    }
    if (ae_comp >= 0) {
        (void)isp_set_ae_compensation((unsigned char)ae_comp);
    }
    if (dehaze_mode >= 0) {
        (void)isp_set_dehaze((isp_block_mode_t)dehaze_mode, (unsigned char)dehaze_strength);
    }
    if (drc_mode >= 0) {
        (void)isp_set_drc((isp_block_mode_t)drc_mode, drc_strength);
    }
    if (ldci_mode >= 0) {
        (void)isp_set_ldci((isp_block_mode_t)ldci_mode);
    }

    if (vpss_init(0, in_w, in_h, chn_cfg) != 0) {
        goto cleanup;
    }
    vpss_up = 1;
    if (capture_bind_vpss(0) != 0) {
        goto cleanup;
    }
    bound = 1;
    if (use_display) {
        if (display_init() != 0) {
            goto cleanup;
        }
        disp_up = 1;
    }

    t0 = now_ms();
    t_last_log = t0;
    while (now_ms() - t0 < run_sec * 1000L) {
        t_get = now_ms();
        if (vpss_get_frame(0, 0, &frame, 1000) != 0) {
            goto cleanup;
        }
        if (now_ms() - t_get > get_ms_max) {
            get_ms_max = now_ms() - t_get;
        }
        if (dump_path != NULL && !dumped && now_ms() - t0 >= 2000) { /* 等 AE 收敛再落盘 */
            (void)dump_nv21_frame(&frame, dump_path);
            dumped = 1;
        }
        if (use_display && display_send_frame(&frame, -1) != 0) {
            (void)vpss_release_frame(0, 0, &frame);
            goto cleanup;
        }
        (void)vpss_release_frame(0, 0, &frame);
        frames++;
        if (now_ms() - t_last_log >= 2000) {
            luma_stats_t luma = {0};
            if (isp_get_luma_stats(&luma) == 0) {
                LOG_INFO("frames=%d avg_fps=%.1f get_max=%ldms | luma mean=%.1f low=%.1f%% high=%.1f%% mode=%d",
                         frames, frames * 1000.0 / (now_ms() - t0), get_ms_max, luma.mean_luma,
                         luma.clip_low_pct, luma.clip_high_pct, control_decide(&luma));
            } else {
                LOG_INFO("frames=%d avg_fps=%.1f get_max=%ldms", frames,
                         frames * 1000.0 / (now_ms() - t0), get_ms_max);
            }
            t_last_log = now_ms();
        }
    }
    rc = 0;
    LOG_INFO("test_capture PASS: %d frames in %ds (%.1f fps)", frames, run_sec,
             frames * 1000.0 / (now_ms() - t0));

cleanup:
    if (disp_up) {
        CHECK_RET(display_deinit());
    }
    if (bound) {
        CHECK_RET(capture_unbind_vpss(0));
    }
    if (vpss_up) {
        CHECK_RET(vpss_deinit(0));
    }
    if (cap_up) {
        CHECK_RET(capture_deinit());
    }
    if (sys_up) {
        sample_comm_sys_exit();
    }
    return rc;
}

#endif /* WITH_SS928_SDK */
