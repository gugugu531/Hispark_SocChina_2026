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

int infer_init(const infer_cfg_t *cfg)
{
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
            return -1;
        }
        g_acl_inited = 1;
    }
    if (aclrtSetDevice(g_device) != ACL_SUCCESS) {
        LOG_ERR("aclrtSetDevice(%d) failed", g_device);
        return -1;
    }
    if (aclrtCreateContext(&g_ctx, g_device) != ACL_SUCCESS) {
        LOG_ERR("aclrtCreateContext failed");
        return -1;
    }
    if (aclmdlLoadFromFile(cfg->model_path, &g_model_id) != ACL_SUCCESS) {
        LOG_ERR("aclmdlLoadFromFile(%s) failed", cfg->model_path);
        return -1;
    }
    g_desc = aclmdlCreateDesc();
    if (g_desc == NULL || aclmdlGetDesc(g_desc, g_model_id) != ACL_SUCCESS) {
        LOG_ERR("aclmdlGetDesc failed");
        return -1;
    }
    g_in_size = aclmdlGetInputSizeByIndex(g_desc, INFER_INPUT_IDX);
    g_out_size = aclmdlGetOutputSizeByIndex(g_desc, INFER_OUTPUT_IDX);
    if (g_in_size == 0 || g_out_size == 0) {
        LOG_ERR("infer: zero io size (in=%zu out=%zu)", g_in_size, g_out_size);
        return -1;
    }

    if (aclrtMalloc(&g_in_dev, g_in_size, ACL_MEM_MALLOC_NORMAL_ONLY) != ACL_SUCCESS ||
        aclrtMalloc(&g_out_dev, g_out_size, ACL_MEM_MALLOC_NORMAL_ONLY) != ACL_SUCCESS) {
        LOG_ERR("infer: aclrtMalloc failed");
        return -1;
    }
    g_in_ds = aclmdlCreateDataset();
    g_out_ds = aclmdlCreateDataset();
    if (g_in_ds == NULL || g_out_ds == NULL) {
        LOG_ERR("infer: create dataset failed");
        return -1;
    }
    if (aclmdlAddDatasetBuffer(g_in_ds, aclCreateDataBuffer(g_in_dev, g_in_size)) != ACL_SUCCESS ||
        aclmdlAddDatasetBuffer(g_out_ds, aclCreateDataBuffer(g_out_dev, g_out_size)) != ACL_SUCCESS) {
        LOG_ERR("infer: add dataset buffer failed");
        return -1;
    }
    g_inited = 1;
    LOG_INFO("infer up: model=%s in=%zuB out=%zuB (inputs=%u outputs=%u)", cfg->model_path,
             g_in_size, g_out_size, (unsigned)aclmdlGetNumInputs(g_desc),
             (unsigned)aclmdlGetNumOutputs(g_desc));
    return 0;
}

int infer_run_nv21(const void *frame_info, float *lut_out, size_t lut_count, infer_timing_t *timing)
{
    const ot_video_frame_info *fi = (const ot_video_frame_info *)frame_info;
    long t0, t1, t2, t3;
    size_t copy_len, i, n;
    td_u8 *virt;
    td_u32 map_size;

    if (!g_inited || fi == NULL || lut_out == NULL) {
        return -1;
    }
    /* 1) 输入：把 NV21 帧映射后复制进 device 输入缓冲（PIPELINE_INPUT_COPY；多退少补到 in_size）。 */
    t0 = now_us();
    map_size = fi->video_frame.stride[0] * fi->video_frame.height * 3 / 2;
    virt = (td_u8 *)ss_mpi_sys_mmap(fi->video_frame.phys_addr[0], map_size);
    if (virt == TD_NULL) {
        LOG_ERR("infer: mmap frame failed");
        return -1;
    }
    copy_len = (map_size < g_in_size) ? map_size : g_in_size;
    if (aclrtMemcpy(g_in_dev, g_in_size, virt, copy_len, ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
        (void)ss_mpi_sys_munmap(virt, map_size);
        LOG_ERR("infer: H2D memcpy failed");
        return -1;
    }
    (void)ss_mpi_sys_munmap(virt, map_size);
    t1 = now_us();

    /* 2) 执行（NPU）。 */
    if (aclmdlExecute(g_model_id, g_in_ds, g_out_ds) != ACL_SUCCESS) {
        LOG_ERR("infer: aclmdlExecute failed");
        return -1;
    }
    t2 = now_us();

    /* 3) 输出：D2H 后按 fp16/fp32 转 float 写入 lut_out。 */
    {
        static uint8_t host_out[2 * PIPELINE_COTF_LUT_FLOAT_COUNT]; /* 容纳 fp16 输出 */
        size_t want = (g_out_size < sizeof(host_out)) ? g_out_size : sizeof(host_out);
        if (aclrtMemcpy(host_out, sizeof(host_out), g_out_dev, want, ACL_MEMCPY_DEVICE_TO_HOST) !=
            ACL_SUCCESS) {
            LOG_ERR("infer: D2H memcpy failed");
            return -1;
        }
        n = lut_count;
        if (g_out_size == lut_count * sizeof(float)) { /* fp32 */
            const float *src = (const float *)host_out;
            if (want >= lut_count * sizeof(float)) {
                for (i = 0; i < n; i++) lut_out[i] = src[i];
            }
        } else { /* 默认按 fp16 */
            const uint16_t *src = (const uint16_t *)host_out;
            size_t avail = want / sizeof(uint16_t);
            if (n > avail) n = avail;
            for (i = 0; i < n; i++) lut_out[i] = half_to_float(src[i]);
        }
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
    if (g_inited) { aclmdlUnload(g_model_id); }
    if (g_ctx != NULL) { aclrtDestroyContext(g_ctx); g_ctx = NULL; }
    if (g_inited) { aclrtResetDevice(g_device); }
    if (g_acl_inited) { aclFinalize(); g_acl_inited = 0; }
    g_inited = 0;
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

int infer_deinit(void)
{
    return 0;
}

#endif /* WITH_SS928_SDK */
