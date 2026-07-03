/* test_raw_replay — RAW 定格回灌 θ-sweep 校准 harness（触硬件，交叉编译后上板手动运行）。
 *
 * 目的：同一帧 RAW 在不同 ISP 参数下确定性重放，为"解析模拟器 ↔ 硬件 ISP"保真度
 * 闸门与代理校准采集 (x, θ) → 硬件输出 数据（docs/isp-param-tuning-research.md §4）。
 *
 * 链路：OS08A20 → VI(OFFLINE) FE → [dump 定格一帧 RAW]
 *        → BE 回灌（frame_source=USER，ss_mpi_isp_run_once 逐帧驱动）
 *        → ISP(施加 θ) → VI chn0 → VPSS chn0 → CPU 取帧 NV21 落盘
 *
 * 用法：./test_raw_replay [选项]
 *   --sensor <0|1>       传感器位（默认 1，海鸥派 OS08A20 在 sensor1/J4）
 *   --warmup <n>         预热回灌次数，AE 在 run_once 环内收敛（默认 60，文件模式 16）
 *   --settle <n>         每组参数生效等待帧数，取最后一帧落盘（默认 2）
 *   --exptime <us> [--again <x1024>]  预热后锁定手动曝光（推荐，保证 sweep 可比）
 *   --out <WxH>          输出分辨率（默认 1024x576，与串流口径一致）
 *   --outdir <dir>       输出目录（默认 .）
 *   --save-raw           定格 RAW 同时落盘（供离线分析；含 stride 填充）
 *   --compress-none      起链后把 pipe RAW 压缩改为 NONE（裸 12bpp packed bayer；
 *                        dump 供主机解析布局，文件回灌必需）
 *   --raw-file <f>       文件回灌模式（可重复 ≤8）：跳过定格，从裸 bayer 文件构造
 *                        RAW 帧作输入（须与 --compress-none 采出的布局一致，
 *                        大小 = stride(=ALIGN(w*12/8)) × h）。每个文件跑完整 sweep，
 *                        输出 out_f<fi>_<idx>_<tag>.nv21
 *   --drc <s>            追加一组 DRC 手动强度 sweep 项（可重复，0-1023）
 *   --ldci <0|1>         追加一组 LDCI 开/关 sweep 项（可重复）
 *   --blob <file>        追加一组完整 DRC/LDCI 参数 blob（可重复，
 *                        models/isp_simulator/isp_blob.py 生成，isp_load_blob_and_apply 施加）
 *   基线（当前参数不动）总是第 0 项。输出 outdir/out_<idx>_<tag>.nv21。
 *
 * 已知约束（查证记录见 docs/isp-param-tuning-research.md §4.2）：
 *   - 仅线性模式（ss_mpi_isp_run_once 不支持帧合成 WDR）；
 *   - VI 必须离线（capture_cfg_t.raw_replay=1），与生产在线配置二选一启动；
 *   - 必须独占媒体链，不可与 socchina_app / 厂商 sample 同时运行。
 */

#include "capture.h"
#include "isp.h"
#include "log.h"
#include "vpss.h"

#ifndef WITH_SS928_SDK

int main(void)
{
    LOG_WARN("test_raw_replay skipped: built without SS928 SDK (host build)");
    return 0;
}

#else /* WITH_SS928_SDK */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ot_buffer.h"
#include "ot_common_isp.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_type.h"
#include "sample_comm.h"
#include "ss_mpi_isp.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_vi.h"

#define REPLAY_VI_PIPE 0
#define SWEEP_MAX      32
#define RAW_FILE_MAX   8
#define CYCLE_TIMEOUT  1000 /* ms：run_once/send/vd/取帧各步超时 */

typedef enum {
    SWEEP_BASELINE = 0,
    SWEEP_DRC,
    SWEEP_LDCI,
    SWEEP_BLOB,
} sweep_kind_t;

typedef struct {
    sweep_kind_t kind;
    unsigned value;      /* DRC 强度 / LDCI 开关 */
    const char *path;    /* blob 文件 */
    char tag[64];
} sweep_item_t;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 一帧 NV21 落盘（去 stride 填充），并算 Y 均值供肉眼 sanity。 */
static int dump_nv21_frame(const ot_video_frame_info *frame, const char *path, double *luma_mean)
{
    const ot_video_frame *vf = &frame->video_frame;
    td_u32 row, col;
    int rc = -1;
    unsigned long long luma_sum = 0;
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
        const td_u8 *line = virt + (size_t)row * vf->stride[0];
        fwrite(line, 1, vf->width, fp);
        for (col = 0; col < vf->width; col++) {
            luma_sum += line[col];
        }
    }
    for (row = 0; row < vf->height / 2; row++) {
        fwrite(virt + (size_t)vf->stride[0] * vf->height + (size_t)row * vf->stride[1], 1, vf->width, fp);
    }
    if (luma_mean != NULL) {
        *luma_mean = (double)luma_sum / ((double)vf->width * vf->height);
    }
    rc = 0;

    fclose(fp);
unmap:
    CHECK_RET(ss_mpi_sys_munmap(virt, map_size));
    return rc;
}

/* 定格 RAW 落盘（含 stride 填充，按帧内存原样；头信息打印到日志供离线解读）。 */
static int dump_raw_frame(const ot_video_frame_info *frame, const char *path)
{
    const ot_video_frame *vf = &frame->video_frame;
    int rc = -1;
    FILE *fp = NULL;
    td_u32 map_size = vf->stride[0] * vf->height;
    td_u8 *virt = (td_u8 *)ss_mpi_sys_mmap(vf->phys_addr[0], map_size);

    if (virt == TD_NULL) {
        LOG_ERR("raw dump: ss_mpi_sys_mmap failed");
        return -1;
    }
    fp = fopen(path, "wb");
    if (fp == NULL) {
        LOG_ERR("raw dump: cannot open %s", path);
        goto unmap;
    }
    fwrite(virt, 1, map_size, fp);
    LOG_INFO("raw dump: %ux%u stride=%u pixel_format=%d compress=%d -> %s (%u bytes)",
             vf->width, vf->height, vf->stride[0], vf->pixel_format, vf->compress_mode, path,
             map_size);
    rc = 0;
    fclose(fp);
unmap:
    CHECK_RET(ss_mpi_sys_munmap(virt, map_size));
    return rc;
}

/* 把 pipe RAW 压缩改为 NONE（裸 12bpp packed bayer）。dump 出的 RAW 才能被主机
 * 直接解析/合成；文件回灌要求 RAW 与 pipe 属性一致，是文件模式的前置。 */
static int set_pipe_compress_none(void)
{
    ot_vi_pipe_attr attr;

    CHECK_RET_GOTO(ss_mpi_vi_get_pipe_attr(REPLAY_VI_PIPE, &attr), fail);
    attr.compress_mode = OT_COMPRESS_MODE_NONE;
    CHECK_RET_GOTO(ss_mpi_vi_set_pipe_attr(REPLAY_VI_PIPE, &attr), fail);
    LOG_INFO("pipe compress -> NONE");
    return 0;
fail:
    return -1;
}

/* 从裸 bayer 文件构造 RAW 回灌帧：公共 VB 池取块 + mmap 写入 + 手填 frame_info
 * （字段填法照厂商 sample_comm_vi_malloc_frame_blk）。 */
typedef struct {
    ot_vb_blk vb_blk;
    td_u32 blk_size;
    td_u8 *virt;
    ot_video_frame_info frame;
} raw_file_frame_t;

static int load_raw_file_frame(const char *path, unsigned w, unsigned h, raw_file_frame_t *rf)
{
    const td_u32 stride = OT_ALIGN_UP(OT_ALIGN_UP(w * 12, 8) / 8, OT_DEFAULT_ALIGN);
    const td_u32 size = stride * h;
    td_phys_addr_t phys;
    FILE *fp = NULL;
    ot_video_frame *vf;

    memset(rf, 0, sizeof(*rf));
    rf->vb_blk = ss_mpi_vb_get_blk(OT_VB_INVALID_POOL_ID, size, TD_NULL);
    if (rf->vb_blk == OT_VB_INVALID_HANDLE) {
        LOG_ERR("raw file: vb_get_blk(%u) failed", size);
        return -1;
    }
    phys = ss_mpi_vb_handle_to_phys_addr(rf->vb_blk);
    rf->virt = (td_u8 *)ss_mpi_sys_mmap(phys, size);
    if (rf->virt == TD_NULL) {
        LOG_ERR("raw file: mmap failed");
        goto fail_blk;
    }
    rf->blk_size = size;

    fp = fopen(path, "rb");
    if (fp == NULL || fread(rf->virt, 1, size, fp) != size) {
        LOG_ERR("raw file: read %s failed (expect %u bytes = stride %u x %u)", path, size,
                stride, h);
        goto fail_map;
    }
    fclose(fp);
    fp = NULL;

    vf = &rf->frame.video_frame;
    rf->frame.pool_id = ss_mpi_vb_handle_to_pool_id(rf->vb_blk);
    rf->frame.mod_id = OT_ID_VI;
    vf->phys_addr[0] = phys;
    vf->phys_addr[1] = phys + size;
    vf->virt_addr[0] = (td_void *)rf->virt;
    vf->virt_addr[1] = (td_void *)(rf->virt + size);
    vf->stride[0] = stride;
    vf->stride[1] = stride;
    vf->width = w;
    vf->height = h;
    vf->pixel_format = OT_PIXEL_FORMAT_RGB_BAYER_12BPP;
    vf->video_format = OT_VIDEO_FORMAT_LINEAR;
    vf->compress_mode = OT_COMPRESS_MODE_NONE;
    vf->dynamic_range = OT_DYNAMIC_RANGE_SDR8;
    vf->field = OT_VIDEO_FIELD_FRAME;
    vf->color_gamut = OT_COLOR_GAMUT_BT601;
    vf->pts = 0;
    LOG_INFO("raw file loaded: %s (%ux%u stride=%u)", path, w, h, stride);
    return 0;

fail_map:
    if (fp != NULL) {
        fclose(fp);
    }
    CHECK_RET(ss_mpi_sys_munmap(rf->virt, size));
fail_blk:
    CHECK_RET(ss_mpi_vb_release_blk(rf->vb_blk));
    memset(rf, 0, sizeof(*rf));
    return -1;
}

static void free_raw_file_frame(raw_file_frame_t *rf)
{
    if (rf->virt != NULL) {
        CHECK_RET(ss_mpi_sys_munmap(rf->virt, rf->blk_size));
        CHECK_RET(ss_mpi_vb_release_blk(rf->vb_blk));
        memset(rf, 0, sizeof(*rf));
    }
}

/* 一次回灌循环：run_once 驱动固件 → 送 RAW → 等 BE 处理完。厂商顺序
 * （sample_comm_vi.c sample_vi_send_pipe_wdr_frame）。 */
static int replay_cycle(const ot_video_frame_info *raw)
{
    const ot_video_frame_info *frames[1] = {raw};

    CHECK_RET_GOTO(ss_mpi_isp_run_once(REPLAY_VI_PIPE), fail);
    CHECK_RET_GOTO(ss_mpi_vi_send_pipe_raw(REPLAY_VI_PIPE, frames, 1, CYCLE_TIMEOUT), fail);
    /* BE 处理完成信号；个别帧超时不致命，输出侧取帧还有一层超时。 */
    CHECK_RET(ss_mpi_isp_get_vd_time_out(REPLAY_VI_PIPE, OT_ISP_VD_BE_END, CYCLE_TIMEOUT));
    return 0;
fail:
    return -1;
}

/* 回灌一轮并从 VPSS 取输出；dump_path 非空时落盘。 */
static int replay_and_fetch(const ot_video_frame_info *raw, const char *dump_path)
{
    ot_video_frame_info out;
    double luma = 0.0;

    if (replay_cycle(raw) != 0) {
        return -1;
    }
    if (vpss_get_frame(0, 0, &out, CYCLE_TIMEOUT) != 0) {
        return -1;
    }
    if (dump_path != NULL) {
        if (dump_nv21_frame(&out, dump_path, &luma) == 0) {
            LOG_INFO("out: %s (Y mean=%.1f)", dump_path, luma);
        }
    }
    (void)vpss_release_frame(0, 0, &out);
    return 0;
}

static int apply_sweep_item(const sweep_item_t *item)
{
    switch (item->kind) {
    case SWEEP_BASELINE:
        return 0;
    case SWEEP_DRC:
        return isp_set_drc(ISP_BLOCK_MANUAL, item->value);
    case SWEEP_LDCI:
        return isp_set_ldci(item->value ? ISP_BLOCK_AUTO : ISP_BLOCK_OFF);
    case SWEEP_BLOB:
        return isp_load_blob_and_apply(item->path);
    default:
        return -1;
    }
}

int main(int argc, char **argv)
{
    capture_cfg_t cap_cfg = {CAPTURE_SENSOR_INDEX_DEFAULT, CAPTURE_MODE_LINEAR, 1 /* raw_replay */};
    int warmup = -1, settle = 2;
    unsigned exp_time_us = 0, again_x1024 = 0;
    unsigned out_w = 1024, out_h = 576;
    const char *outdir = ".";
    int save_raw = 0, compress_none = 0;
    const char *raw_files[RAW_FILE_MAX];
    int raw_file_cnt = 0;
    raw_file_frame_t rf = {0};
    sweep_item_t sweep[SWEEP_MAX];
    int sweep_cnt = 0;
    int rc = 1;
    int sys_up = 0, cap_up = 0, vpss_up = 0, bound = 0, source_user = 0, raw_held = 0;
    unsigned in_w = 0, in_h = 0;
    int i, k, fi;
    char path[512];
    ot_vb_cfg vb_cfg = {0};
    ot_pic_buf_attr buf_attr = {0};
    ot_vb_calc_cfg calc_cfg = {0};
    td_u64 raw_blk_size = 0;
    ot_vi_frame_dump_attr dump_attr = {TD_TRUE, 2};
    ot_video_frame_info raw_frame;
    vpss_chn_cfg_t chn_cfg[VPSS_CHN_COUNT] = {
        {1, 0, 0, 2}, /* chn0：回灌输出取帧口；宽高由 --out 决定 */
        {0, 0, 0, 0},
        {0, 0, 0, 0},
    };

    /* 第 0 项固定为基线 */
    sweep[0].kind = SWEEP_BASELINE;
    snprintf(sweep[0].tag, sizeof(sweep[0].tag), "base");
    sweep_cnt = 1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sensor") == 0 && i + 1 < argc) {
            cap_cfg.sensor_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            warmup = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--settle") == 0 && i + 1 < argc) {
            settle = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--exptime") == 0 && i + 1 < argc) {
            exp_time_us = (unsigned)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--again") == 0 && i + 1 < argc) {
            again_x1024 = (unsigned)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &out_w, &out_h) != 2) {
                LOG_ERR("bad --out, expect WxH");
                return 1;
            }
        } else if (strcmp(argv[i], "--outdir") == 0 && i + 1 < argc) {
            outdir = argv[++i];
        } else if (strcmp(argv[i], "--save-raw") == 0) {
            save_raw = 1;
        } else if (strcmp(argv[i], "--compress-none") == 0) {
            compress_none = 1;
        } else if (strcmp(argv[i], "--raw-file") == 0 && i + 1 < argc &&
                   raw_file_cnt < RAW_FILE_MAX) {
            raw_files[raw_file_cnt++] = argv[++i];
        } else if (strcmp(argv[i], "--drc") == 0 && i + 1 < argc && sweep_cnt < SWEEP_MAX) {
            sweep[sweep_cnt].kind = SWEEP_DRC;
            sweep[sweep_cnt].value = (unsigned)atoi(argv[++i]);
            snprintf(sweep[sweep_cnt].tag, sizeof(sweep[0].tag), "drc%u", sweep[sweep_cnt].value);
            sweep_cnt++;
        } else if (strcmp(argv[i], "--ldci") == 0 && i + 1 < argc && sweep_cnt < SWEEP_MAX) {
            sweep[sweep_cnt].kind = SWEEP_LDCI;
            sweep[sweep_cnt].value = (unsigned)atoi(argv[++i]);
            snprintf(sweep[sweep_cnt].tag, sizeof(sweep[0].tag), "ldci%u", sweep[sweep_cnt].value);
            sweep_cnt++;
        } else if (strcmp(argv[i], "--blob") == 0 && i + 1 < argc && sweep_cnt < SWEEP_MAX) {
            const char *p = argv[++i];
            const char *base = strrchr(p, '/');
            sweep[sweep_cnt].kind = SWEEP_BLOB;
            sweep[sweep_cnt].path = p;
            snprintf(sweep[sweep_cnt].tag, sizeof(sweep[0].tag), "blob_%s", base ? base + 1 : p);
            sweep_cnt++;
        } else {
            LOG_ERR("unknown arg: %s", argv[i]);
            return 1;
        }
    }

    if (warmup < 0) {
        warmup = (raw_file_cnt > 0) ? 16 : 60; /* 文件模式输入固定，只需 3A/统计稳定 */
    }
    if (raw_file_cnt > 0) {
        compress_none = 1; /* 文件回灌前置：pipe RAW 必须为裸 bayer */
    }
    if (capture_query_in_size(&in_w, &in_h) != 0) {
        return 1;
    }
    chn_cfg[0].width = out_w;
    chn_cfg[0].height = out_h;
    LOG_INFO("test_raw_replay: sensor=%d in=%ux%u out=%ux%u warmup=%d settle=%d sweep=%d "
             "raw_files=%d compress_none=%d",
             cap_cfg.sensor_index, in_w, in_h, out_w, out_h, warmup, settle, sweep_cnt,
             raw_file_cnt, compress_none);

    /* VB：池 0 = NV21 全幅（VI chn/VPSS 输出）；池 1 = RAW（VI 离线 FE 写 DDR）。
     * RAW 块按 16bpp 无压缩计算，保守覆盖 12bpp 及 LINE 压缩两种实际形态。 */
    buf_attr.width = in_w;
    buf_attr.height = in_h;
    buf_attr.align = OT_DEFAULT_ALIGN;
    buf_attr.bit_width = OT_DATA_BIT_WIDTH_8;
    buf_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    buf_attr.compress_mode = OT_COMPRESS_MODE_SEG;
    ot_common_get_pic_buf_cfg(&buf_attr, &calc_cfg);
    vb_cfg.max_pool_cnt = 2;
    vb_cfg.common_pool[0].blk_size = calc_cfg.vb_size;
    vb_cfg.common_pool[0].blk_cnt = 8;

    memset(&buf_attr, 0, sizeof(buf_attr));
    memset(&calc_cfg, 0, sizeof(calc_cfg));
    buf_attr.width = in_w;
    buf_attr.height = in_h;
    buf_attr.align = OT_DEFAULT_ALIGN;
    buf_attr.bit_width = OT_DATA_BIT_WIDTH_16;
    buf_attr.pixel_format = OT_PIXEL_FORMAT_RGB_BAYER_16BPP;
    buf_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    ot_common_get_pic_buf_cfg(&buf_attr, &calc_cfg);
    raw_blk_size = calc_cfg.vb_size;
    vb_cfg.common_pool[1].blk_size = raw_blk_size;
    vb_cfg.common_pool[1].blk_cnt = 5;
    LOG_INFO("VB: nv21 blk=%llu x8, raw blk=%llu x5",
             (unsigned long long)vb_cfg.common_pool[0].blk_size, (unsigned long long)raw_blk_size);

    CHECK_RET_GOTO(sample_comm_sys_init_with_vb_supplement(&vb_cfg, OT_VB_SUPPLEMENT_BNR_MOT_MASK),
                   cleanup);
    sys_up = 1;

    if (capture_init(&cap_cfg) != 0) {
        goto cleanup;
    }
    cap_up = 1;

    if (compress_none && set_pipe_compress_none() != 0) {
        goto cleanup;
    }

    if (vpss_init(0, in_w, in_h, chn_cfg) != 0) {
        goto cleanup;
    }
    vpss_up = 1;
    if (capture_bind_vpss(0) != 0) {
        goto cleanup;
    }
    bound = 1;

    /* FE dump 队列 + BE 输入切用户态（此后 FE 仍持续从 sensor 收帧供 dump）。 */
    CHECK_RET_GOTO(ss_mpi_vi_set_pipe_frame_dump_attr(REPLAY_VI_PIPE, &dump_attr), cleanup);
    CHECK_RET_GOTO(ss_mpi_vi_set_pipe_frame_source(REPLAY_VI_PIPE, OT_VI_PIPE_FRAME_SOURCE_USER),
                   cleanup);
    source_user = 1;

    if (raw_file_cnt > 0 && load_raw_file_frame(raw_files[0], in_w, in_h, &rf) != 0) {
        goto cleanup;
    }

    /* 预热：AE/3A 在 run_once 环内收敛。dump 模式用最新 FE 帧；
     * 文件模式直接用文件帧（3A 统计收敛到实际回放内容）。 */
    {
        long t0 = now_ms();
        int ok = 0;
        for (k = 0; k < warmup; k++) {
            if (raw_file_cnt > 0) {
                if (replay_and_fetch(&rf.frame, NULL) == 0) {
                    ok++;
                }
            } else {
                ot_video_frame_info fe_frame;
                if (ss_mpi_vi_get_pipe_frame(REPLAY_VI_PIPE, &fe_frame, CYCLE_TIMEOUT) != 0) {
                    LOG_ERR("warmup: get_pipe_frame failed at %d", k);
                    goto cleanup;
                }
                if (replay_and_fetch(&fe_frame, NULL) == 0) {
                    ok++;
                }
                CHECK_RET(ss_mpi_vi_release_pipe_frame(REPLAY_VI_PIPE, &fe_frame));
            }
        }
        LOG_INFO("warmup: %d/%d cycles ok, %ldms", ok, warmup, now_ms() - t0);
        if (ok == 0) {
            LOG_ERR("warmup: all cycles failed, abort");
            goto cleanup;
        }
    }

    /* 锁定曝光（dump 模式；文件模式输入来自文件，sensor 曝光不影响内容）。 */
    if (raw_file_cnt == 0 && (exp_time_us != 0 || again_x1024 != 0)) {
        (void)isp_set_exposure_manual(exp_time_us, again_x1024);
        /* 生效等待：再回灌几帧让 sensor/固件采用手动值 */
        for (k = 0; k < 5; k++) {
            ot_video_frame_info fe_frame;
            if (ss_mpi_vi_get_pipe_frame(REPLAY_VI_PIPE, &fe_frame, CYCLE_TIMEOUT) != 0) {
                break;
            }
            (void)replay_and_fetch(&fe_frame, NULL);
            CHECK_RET(ss_mpi_vi_release_pipe_frame(REPLAY_VI_PIPE, &fe_frame));
        }
    }

    if (raw_file_cnt == 0) {
        /* 定格：抓一帧 RAW 持有到 sweep 结束（该 VB 即回灌输入，全程只读）。 */
        CHECK_RET_GOTO(ss_mpi_vi_get_pipe_frame(REPLAY_VI_PIPE, &raw_frame, CYCLE_TIMEOUT),
                       cleanup);
        raw_held = 1;
        LOG_INFO("raw held: %ux%u pixel_format=%d compress=%d", raw_frame.video_frame.width,
                 raw_frame.video_frame.height, raw_frame.video_frame.pixel_format,
                 raw_frame.video_frame.compress_mode);
        if (save_raw) {
            snprintf(path, sizeof(path), "%s/raw_ref.raw", outdir);
            (void)dump_raw_frame(&raw_frame, path);
        }
    }

    /* θ-sweep：每个输入（定格帧或各 RAW 文件）× 各参数组，settle 帧后取输出落盘。 */
    for (fi = 0; fi < ((raw_file_cnt > 0) ? raw_file_cnt : 1); fi++) {
        const ot_video_frame_info *input;

        if (raw_file_cnt > 0) {
            if (fi > 0) {
                free_raw_file_frame(&rf);
                if (load_raw_file_frame(raw_files[fi], in_w, in_h, &rf) != 0) {
                    goto cleanup;
                }
            }
            input = &rf.frame;
        } else {
            input = &raw_frame;
        }

        for (i = 0; i < sweep_cnt; i++) {
            long t0 = now_ms();
            if (apply_sweep_item(&sweep[i]) != 0) {
                LOG_ERR("sweep[%d] %s: apply failed, skip", i, sweep[i].tag);
                continue;
            }
            for (k = 0; k < settle - 1; k++) {
                if (replay_and_fetch(input, NULL) != 0) {
                    LOG_ERR("f%d sweep[%d] %s: settle cycle %d failed", fi, i, sweep[i].tag, k);
                    goto cleanup;
                }
            }
            if (raw_file_cnt > 0) {
                snprintf(path, sizeof(path), "%s/out_f%02d_%02d_%s.nv21", outdir, fi, i,
                         sweep[i].tag);
            } else {
                snprintf(path, sizeof(path), "%s/out_%02d_%s.nv21", outdir, i, sweep[i].tag);
            }
            if (replay_and_fetch(input, path) != 0) {
                LOG_ERR("f%d sweep[%d] %s: final cycle failed", fi, i, sweep[i].tag);
                goto cleanup;
            }
            LOG_INFO("f%d sweep[%d/%d] %s done, %ldms", fi, i + 1, sweep_cnt, sweep[i].tag,
                     now_ms() - t0);
        }
    }
    rc = 0;
    LOG_INFO("test_raw_replay PASS: %d input(s) x %d sweep items, out=%s",
             (raw_file_cnt > 0) ? raw_file_cnt : 1, sweep_cnt, outdir);

cleanup:
    free_raw_file_frame(&rf);
    if (raw_held) {
        CHECK_RET(ss_mpi_vi_release_pipe_frame(REPLAY_VI_PIPE, &raw_frame));
    }
    if (source_user) {
        CHECK_RET(ss_mpi_vi_set_pipe_frame_source(REPLAY_VI_PIPE, OT_VI_PIPE_FRAME_SOURCE_FE));
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
