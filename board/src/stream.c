#include "stream.h"

#include "log.h"
#include "rtsp_server.h"

#ifdef WITH_SS928_SDK

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ot_buffer.h"
#include "ot_common_venc.h"
#include "ot_common_video.h"
#include "ss_mpi_venc.h"

#define STREAM_VENC_CHN  0
#define STREAM_MAX_PACKS 64u
#define STREAM_AU_CAP    (2u * 1024u * 1024u)

typedef struct {
    int initialized;
    int stop;
    int venc_created;
    int venc_started;
    int drain_started;
    pthread_t drain_thread;
    ot_venc_pack* packs;
    uint8_t* access_unit;
} stream_state_t;

static stream_state_t g_stream;

static void request_idr(void* opaque) {
    (void) opaque;
    td_s32 ret = ss_mpi_venc_request_idr(STREAM_VENC_CHN, TD_TRUE);
    if (ret != TD_SUCCESS) {
        LOG_ERR("ss_mpi_venc_request_idr failed: %#x", (unsigned) ret);
    }
}

static int venc_create(const stream_cfg_t* cfg) {
    ot_venc_chn_attr attr;
    ot_venc_start_param start;
    ot_venc_buf_attr buf_attr;
    td_s32 ret;

    memset(&attr, 0, sizeof(attr));
    memset(&buf_attr, 0, sizeof(buf_attr));
    attr.venc_attr.type = OT_PT_H264;
    attr.venc_attr.max_pic_width = cfg->width;
    attr.venc_attr.max_pic_height = cfg->height;
    attr.venc_attr.pic_width = cfg->width;
    attr.venc_attr.pic_height = cfg->height;
    attr.venc_attr.profile = 0;
    attr.venc_attr.is_by_frame = TD_TRUE;
    attr.venc_attr.h264_attr.rcn_ref_share_buf_en = TD_TRUE;
    attr.venc_attr.h264_attr.frame_buf_ratio = 100;
    buf_attr.share_buf_en = TD_TRUE;
    buf_attr.frame_buf_ratio = 100;
    buf_attr.pic_buf_attr.width = cfg->width;
    buf_attr.pic_buf_attr.height = cfg->height;
    buf_attr.pic_buf_attr.align = OT_DEFAULT_ALIGN;
    buf_attr.pic_buf_attr.bit_width = OT_DATA_BIT_WIDTH_8;
    buf_attr.pic_buf_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    buf_attr.pic_buf_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    attr.venc_attr.buf_size = ot_venc_get_pic_buf_size(OT_PT_H264, &buf_attr);
    if (attr.venc_attr.buf_size == 0) {
        LOG_ERR("ot_venc_get_pic_buf_size failed for %ux%u", cfg->width, cfg->height);
        return -1;
    }

    attr.rc_attr.rc_mode = OT_VENC_RC_MODE_H264_CBR;
    attr.rc_attr.h264_cbr.gop = cfg->fps;
    attr.rc_attr.h264_cbr.stats_time = 1;
    attr.rc_attr.h264_cbr.src_frame_rate = cfg->fps;
    attr.rc_attr.h264_cbr.dst_frame_rate = cfg->fps;
    attr.rc_attr.h264_cbr.bit_rate = cfg->bitrate_kbps;
    attr.gop_attr.gop_mode = OT_VENC_GOP_MODE_NORMAL_P;
    attr.gop_attr.normal_p.ip_qp_delta = 2;

    ret = ss_mpi_venc_create_chn(STREAM_VENC_CHN, &attr);
    if (ret != TD_SUCCESS) {
        LOG_ERR("ss_mpi_venc_create_chn failed: %#x", (unsigned) ret);
        return -1;
    }
    g_stream.venc_created = 1;
    start.recv_pic_num = -1;
    ret = ss_mpi_venc_start_chn(STREAM_VENC_CHN, &start);
    if (ret != TD_SUCCESS) {
        LOG_ERR("ss_mpi_venc_start_chn failed: %#x", (unsigned) ret);
        return -1;
    }
    g_stream.venc_started = 1;
    return 0;
}

static void* drain_worker(void* arg) {
    (void) arg;
    while (!g_stream.stop) {
        ot_venc_chn_status status;
        ot_venc_stream stream;
        size_t used = 0;
        td_s32 ret;

        ret = ss_mpi_venc_query_status(STREAM_VENC_CHN, &status);
        if (ret != TD_SUCCESS) {
            LOG_ERR("ss_mpi_venc_query_status failed: %#x", (unsigned) ret);
            usleep(10000);
            continue;
        }
        if (status.cur_packs == 0) {
            usleep(2000);
            continue;
        }
        if (status.cur_packs > STREAM_MAX_PACKS) {
            LOG_WARN("drop encoded frame: %u packs exceeds cap %u", status.cur_packs, STREAM_MAX_PACKS);
            usleep(1000);
            continue;
        }
        memset(&stream, 0, sizeof(stream));
        stream.pack = g_stream.packs;
        stream.pack_cnt = status.cur_packs;
        ret = ss_mpi_venc_get_stream(STREAM_VENC_CHN, &stream, 20);
        if (ret != TD_SUCCESS) {
            continue;
        }
        for (td_u32 i = 0; i < stream.pack_cnt; i++) {
            const ot_venc_pack* pack = &stream.pack[i];
            size_t bytes = pack->len > pack->offset ? pack->len - pack->offset : 0;
            if (used + bytes > STREAM_AU_CAP) {
                used = 0;
                LOG_WARN("drop encoded frame: access unit exceeds %u bytes", STREAM_AU_CAP);
                break;
            }
            memcpy(g_stream.access_unit + used, pack->addr + pack->offset, bytes);
            used += bytes;
        }
        if (used > 0) {
            (void) rtsp_server_publish_h264(g_stream.access_unit, used);
        }
        ret = ss_mpi_venc_release_stream(STREAM_VENC_CHN, &stream);
        if (ret != TD_SUCCESS) {
            LOG_ERR("ss_mpi_venc_release_stream failed: %#x", (unsigned) ret);
        }
    }
    return NULL;
}

int stream_init(const stream_cfg_t* cfg) {
    rtsp_server_cfg_t rtsp_cfg;

    if (cfg == NULL || cfg->width == 0 || cfg->height == 0 || cfg->fps == 0 || cfg->bitrate_kbps == 0 ||
        cfg->rtsp_port == 0 || cfg->stream_path == NULL) {
        return -1;
    }
    if (g_stream.initialized) {
        return 0;
    }
    memset(&g_stream, 0, sizeof(g_stream));
    g_stream.packs = calloc(STREAM_MAX_PACKS, sizeof(*g_stream.packs));
    g_stream.access_unit = malloc(STREAM_AU_CAP);
    if (g_stream.packs == NULL || g_stream.access_unit == NULL) {
        goto cleanup;
    }
    if (venc_create(cfg) != 0) {
        goto cleanup;
    }

    memset(&rtsp_cfg, 0, sizeof(rtsp_cfg));
    rtsp_cfg.port = cfg->rtsp_port;
    rtsp_cfg.fps = cfg->fps;
    rtsp_cfg.stream_path = cfg->stream_path;
    rtsp_cfg.bind_addr = cfg->rtsp_bind_addr;
    rtsp_cfg.request_idr = request_idr;
    if (rtsp_server_start(&rtsp_cfg) != 0) {
        LOG_ERR("rtsp_server_start failed");
        goto cleanup;
    }
    if (pthread_create(&g_stream.drain_thread, NULL, drain_worker, NULL) != 0) {
        LOG_ERR("stream drain thread create failed");
        goto cleanup;
    }
    g_stream.drain_started = 1;
    g_stream.initialized = 1;
    LOG_INFO("stream up: H.264 %ux%u@%u %ukbps rtsp://%s:%u/%s (RTP over TCP)", cfg->width, cfg->height,
             cfg->fps, cfg->bitrate_kbps,
             (cfg->rtsp_bind_addr != NULL && cfg->rtsp_bind_addr[0] != '\0') ? cfg->rtsp_bind_addr : "<board>",
             cfg->rtsp_port, cfg->stream_path);
    return 0;

cleanup:
    (void) stream_deinit();
    return -1;
}

int stream_send_frame(const void* frame_info, int timeout_ms) {
    td_s32 ret;
    if (!g_stream.initialized || frame_info == NULL) {
        return -1;
    }
    ret = ss_mpi_venc_send_frame(STREAM_VENC_CHN, (const ot_video_frame_info*) frame_info, timeout_ms);
    if (ret != TD_SUCCESS) {
        LOG_ERR("ss_mpi_venc_send_frame failed: %#x", (unsigned) ret);
        return -1;
    }
    return 0;
}

int stream_deinit(void) {
    g_stream.stop = 1;
    if (g_stream.drain_started) {
        pthread_join(g_stream.drain_thread, NULL);
        g_stream.drain_started = 0;
    }
    (void) rtsp_server_stop();
    if (g_stream.venc_started) {
        CHECK_RET(ss_mpi_venc_stop_chn(STREAM_VENC_CHN));
        g_stream.venc_started = 0;
    }
    if (g_stream.venc_created) {
        CHECK_RET(ss_mpi_venc_destroy_chn(STREAM_VENC_CHN));
        g_stream.venc_created = 0;
    }
    free(g_stream.access_unit);
    free(g_stream.packs);
    memset(&g_stream, 0, sizeof(g_stream));
    return 0;
}

#else /* !WITH_SS928_SDK */

int stream_init(const stream_cfg_t* cfg) {
    (void) cfg;
    LOG_ERR("stream_init: built without SS928 SDK");
    return -1;
}

int stream_send_frame(const void* frame_info, int timeout_ms) {
    (void) frame_info;
    (void) timeout_ms;
    return -1;
}

int stream_deinit(void) {
    return 0;
}

#endif /* WITH_SS928_SDK */
