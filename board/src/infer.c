#include "infer.h"

#include "log.h"

#ifdef WITH_SS928_SDK

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "acl.h"
#include "ot_common_video.h"
#include "ss_mpi_sys.h"

/* CoTF param-net 的 ACL 推理封装（全 ACL aclmdl* API，与板端 acl_om_benchmark 一致）。
 * 同步、单实例、单线程。device 内存仅在 init 分配一次，run 复用。 */

#define INFER_INPUT_IDX  0
#define INFER_OUTPUT_IDX 0

static int g_acl_inited = 0;
static int g_inited = 0;
static int g_device_set = 0;
static int g_model_loaded = 0;
static int32_t g_device = 0;
static aclrtContext g_ctx = NULL;
static uint32_t g_model_id = 0;
static aclmdlDesc *g_desc = NULL;
static aclmdlDataset *g_in_ds = NULL;
static aclmdlDataset *g_out_ds = NULL;
static void *g_in_dev = NULL;
static void *g_out_dev = NULL;
static size_t g_in_size = 0;
static size_t g_out_size = 0;

static long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

/* IEEE half → float。 */
static float half_to_float(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else { /* subnormal */
            exp = 127 - 15 + 1;
            while ((man & 0x400) == 0) { man <<= 1; exp--; }
            man &= 0x3FF;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static int execute_and_copy(float *lut_out, size_t lut_count, long t0, long t1,
                            infer_timing_t *timing)
{
    long t2, t3;
    size_t i, n;
    static uint8_t host_out[4 * PIPELINE_COTF_LUT_FLOAT_COUNT];

    if (aclmdlExecute(g_model_id, g_in_ds, g_out_ds) != ACL_SUCCESS) {
        LOG_ERR("infer: aclmdlExecute failed");
        return PIPELINE_ERR_RUNTIME;
    }
    t2 = now_us();
    if (g_out_size > sizeof(host_out) ||
        aclrtMemcpy(host_out, sizeof(host_out), g_out_dev, g_out_size,
                    ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
        LOG_ERR("infer: D2H memcpy failed/out too large (%zu)", g_out_size);
        return PIPELINE_ERR_RUNTIME;
    }
    n = lut_count;
    if (g_out_size == lut_count * sizeof(float)) {
        const float *src = (const float *)host_out;
        for (i = 0; i < n; i++) lut_out[i] = src[i];
    } else if (g_out_size == lut_count * sizeof(uint16_t)) {
        const uint16_t *src = (const uint16_t *)host_out;
        for (i = 0; i < n; i++) lut_out[i] = half_to_float(src[i]);
    } else {
        LOG_ERR("infer: unexpected output size %zu for %zu floats", g_out_size, lut_count);
        return PIPELINE_ERR_INVALID;
    }
    t3 = now_us();
    if (timing != NULL) {
        timing->input_copy_ms = (t1 - t0) / 1000.0f;
        timing->execute_ms = (t2 - t1) / 1000.0f;
        timing->output_copy_ms = (t3 - t2) / 1000.0f;
        timing->total_ms = (t3 - t0) / 1000.0f;
    }
    return 0;
}

int infer_init(const infer_cfg_t *cfg)
{
    aclDataBuffer *in_buf = NULL;
    aclDataBuffer *out_buf = NULL;

    if (cfg == NULL || cfg->model_path == NULL) {
        LOG_ERR("infer_init: bad cfg");
        return -1;
    }
    if (g_inited) {
        LOG_WARN("infer already inited");
        return 0;
    }
    g_device = cfg->device_id;

    if (!g_acl_inited) {
        if (aclInit(NULL) != ACL_SUCCESS) {
            LOG_ERR("aclInit failed");
            return PIPELINE_ERR_RUNTIME;
        }
        g_acl_inited = 1;
    }
    if (aclrtSetDevice(g_device) != ACL_SUCCESS) {
        LOG_ERR("aclrtSetDevice(%d) failed", g_device);
        goto fail;
    }
    g_device_set = 1;
    if (aclrtCreateContext(&g_ctx, g_device) != ACL_SUCCESS) {
        LOG_ERR("aclrtCreateContext failed");
        goto fail;
    }
    if (aclmdlLoadFromFile(cfg->model_path, &g_model_id) != ACL_SUCCESS) {
        LOG_ERR("aclmdlLoadFromFile(%s) failed", cfg->model_path);
        goto fail;
    }
    g_model_loaded = 1;
    g_desc = aclmdlCreateDesc();
    if (g_desc == NULL || aclmdlGetDesc(g_desc, g_model_id) != ACL_SUCCESS) {
        LOG_ERR("aclmdlGetDesc failed");
        goto fail;
    }
    g_in_size = aclmdlGetInputSizeByIndex(g_desc, INFER_INPUT_IDX);
    g_out_size = aclmdlGetOutputSizeByIndex(g_desc, INFER_OUTPUT_IDX);
    if (g_in_size == 0 || g_out_size == 0) {
        LOG_ERR("infer: zero io size (in=%zu out=%zu)", g_in_size, g_out_size);
        goto fail;
    }

    if (aclrtMalloc(&g_in_dev, g_in_size, ACL_MEM_MALLOC_NORMAL_ONLY) != ACL_SUCCESS ||
        aclrtMalloc(&g_out_dev, g_out_size, ACL_MEM_MALLOC_NORMAL_ONLY) != ACL_SUCCESS) {
        LOG_ERR("infer: aclrtMalloc failed");
        goto fail;
    }
    g_in_ds = aclmdlCreateDataset();
    g_out_ds = aclmdlCreateDataset();
    if (g_in_ds == NULL || g_out_ds == NULL) {
        LOG_ERR("infer: create dataset failed");
        goto fail;
    }
    in_buf = aclCreateDataBuffer(g_in_dev, g_in_size);
    out_buf = aclCreateDataBuffer(g_out_dev, g_out_size);
    if (in_buf == NULL || out_buf == NULL ||
        aclmdlAddDatasetBuffer(g_in_ds, in_buf) != ACL_SUCCESS ||
        aclmdlAddDatasetBuffer(g_out_ds, out_buf) != ACL_SUCCESS) {
        LOG_ERR("infer: add dataset buffer failed");
        if (in_buf != NULL && aclmdlGetDatasetNumBuffers(g_in_ds) == 0) {
            aclDestroyDataBuffer(in_buf);
        }
        if (out_buf != NULL && aclmdlGetDatasetNumBuffers(g_out_ds) == 0) {
            aclDestroyDataBuffer(out_buf);
        }
        goto fail;
    }
    g_inited = 1;
    LOG_INFO("infer up: model=%s in=%zuB out=%zuB (inputs=%u outputs=%u)", cfg->model_path,
             g_in_size, g_out_size, (unsigned)aclmdlGetNumInputs(g_desc),
             (unsigned)aclmdlGetNumOutputs(g_desc));
    return 0;
fail:
    (void)infer_deinit();
    return PIPELINE_ERR_RUNTIME;
}

int infer_run_nv21(const void *frame_info, float *lut_out, size_t lut_count, infer_timing_t *timing)
{
    const ot_video_frame_info *fi = (const ot_video_frame_info *)frame_info;
    long t0, t1;
    size_t copy_len;
    td_u8 *virt;
    td_u32 map_size;

    if (!g_inited || fi == NULL || lut_out == NULL) {
        return PIPELINE_ERR_STATE;
    }
    /* ACL context 是线程相关状态：生产程序在 main 线程 init、control worker 中 run。 */
    if (aclrtSetCurrentContext(g_ctx) != ACL_SUCCESS) {
        LOG_ERR("infer: aclrtSetCurrentContext failed");
        return PIPELINE_ERR_RUNTIME;
    }
    /* 1) 输入：把 NV21 帧映射后复制进 device 输入缓冲（PIPELINE_INPUT_COPY；多退少补到 in_size）。 */
    t0 = now_us();
    map_size = fi->video_frame.stride[0] * fi->video_frame.height * 3 / 2;
    virt = (td_u8 *)ss_mpi_sys_mmap(fi->video_frame.phys_addr[0], map_size);
    if (virt == TD_NULL) {
        LOG_ERR("infer: mmap frame failed");
        return PIPELINE_ERR_IO;
    }
    copy_len = (map_size < g_in_size) ? map_size : g_in_size;
    if (aclrtMemcpy(g_in_dev, g_in_size, virt, copy_len, ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
        (void)ss_mpi_sys_munmap(virt, map_size);
        LOG_ERR("infer: H2D memcpy failed");
        return PIPELINE_ERR_RUNTIME;
    }
    (void)ss_mpi_sys_munmap(virt, map_size);
    t1 = now_us();

    return execute_and_copy(lut_out, lut_count, t0, t1, timing);
}

int infer_run_buffer(const void *input, size_t input_size, float *lut_out, size_t lut_count,
                     infer_timing_t *timing)
{
    long t0, t1;

    if (!g_inited || input == NULL || lut_out == NULL || input_size != g_in_size) {
        LOG_ERR("infer_run_buffer: input=%p size=%zu expected=%zu", input, input_size, g_in_size);
        return PIPELINE_ERR_INVALID;
    }
    if (aclrtSetCurrentContext(g_ctx) != ACL_SUCCESS) {
        LOG_ERR("infer: aclrtSetCurrentContext failed");
        return PIPELINE_ERR_RUNTIME;
    }
    t0 = now_us();
    if (aclrtMemcpy(g_in_dev, g_in_size, input, input_size, ACL_MEMCPY_HOST_TO_DEVICE) !=
        ACL_SUCCESS) {
        LOG_ERR("infer: buffer H2D memcpy failed");
        return PIPELINE_ERR_RUNTIME;
    }
    t1 = now_us();
    return execute_and_copy(lut_out, lut_count, t0, t1, timing);
}

int infer_deinit(void)
{
    if (g_in_ds != NULL) {
        aclDataBuffer *b = aclmdlGetDatasetBuffer(g_in_ds, 0);
        if (b != NULL) aclDestroyDataBuffer(b);
        aclmdlDestroyDataset(g_in_ds);
        g_in_ds = NULL;
    }
    if (g_out_ds != NULL) {
        aclDataBuffer *b = aclmdlGetDatasetBuffer(g_out_ds, 0);
        if (b != NULL) aclDestroyDataBuffer(b);
        aclmdlDestroyDataset(g_out_ds);
        g_out_ds = NULL;
    }
    if (g_in_dev != NULL) { aclrtFree(g_in_dev); g_in_dev = NULL; }
    if (g_out_dev != NULL) { aclrtFree(g_out_dev); g_out_dev = NULL; }
    if (g_desc != NULL) { aclmdlDestroyDesc(g_desc); g_desc = NULL; }
    if (g_model_loaded) {
        aclmdlUnload(g_model_id);
        g_model_loaded = 0;
    }
    if (g_ctx != NULL) { aclrtDestroyContext(g_ctx); g_ctx = NULL; }
    if (g_device_set) {
        aclrtResetDevice(g_device);
        g_device_set = 0;
    }
    if (g_acl_inited) { aclFinalize(); g_acl_inited = 0; }
    g_inited = 0;
    g_in_size = 0;
    g_out_size = 0;
    return 0;
}

#else /* !WITH_SS928_SDK */

int infer_init(const infer_cfg_t *cfg)
{
    (void)cfg;
    LOG_ERR("infer_init: built without SS928 SDK");
    return -1;
}

int infer_run_nv21(const void *frame_info, float *lut_out, size_t lut_count, infer_timing_t *timing)
{
    (void)frame_info;
    (void)lut_out;
    (void)lut_count;
    (void)timing;
    return -1;
}

int infer_run_buffer(const void *input, size_t input_size, float *lut_out, size_t lut_count,
                     infer_timing_t *timing)
{
    (void)input;
    (void)input_size;
    (void)lut_out;
    (void)lut_count;
    (void)timing;
    return -1;
}

int infer_deinit(void)
{
    return 0;
}

#endif /* WITH_SS928_SDK */
