/* CTBG 双 OM 推理实现（estimator + apply），支持 v8 18ch 与 v9 6ch。
 * 复用 ACL aclmdl* API，与 board/src/infer.c 同风格。 */
#include "infer_ctbg.h"

#include "log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "acl.h"

/* ---- 内部状态 ---- */
static int g_inited = 0;
static int g_acl_ready = 0;     /* aclInit / aclrtSetDevice / ctx 已完成 */
static int32_t g_device = 0;
static aclrtContext g_ctx = NULL;

/* estimator */
static uint32_t g_est_mid = 0;
static aclmdlDesc *g_est_desc = NULL;
static aclmdlDataset *g_est_in = NULL, *g_est_out = NULL;
static void *g_est_in_dev = NULL, *g_est_out_dev = NULL;
static size_t g_est_in_sz = 0, g_est_out_sz = 0;

/* apply */
static uint32_t g_app_mid = 0;
static aclmdlDesc *g_app_desc = NULL;
static aclmdlDataset *g_app_in = NULL, *g_app_out = NULL;
static void *g_app_in0_dev = NULL, *g_app_in1_dev = NULL, *g_app_out_dev = NULL;
static size_t g_app_in0_sz = 0, g_app_in1_sz = 0, g_app_out_sz = 0;

/* host-side coeff size */
static size_t g_coeff_raw_sz = 0;  /* estimator 输出大小 */
static size_t g_coeff_app_sz = 0;  /* apply 系数输入大小（v8 低分辨率 / v9 全分辨率） */

/* cfg 快照 */
static unsigned g_full_w = 1024, g_full_h = 576;
static unsigned g_low_w = 256, g_low_h = 144;

static long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000000L + ts.tv_nsec / 1000L);
}

static int acl_ok(aclError ret, const char *what)
{
    if (ret == ACL_SUCCESS) return 1;
    LOG_ERR("ctbg: %s failed aclError=%d", what, (int)ret);
    return 0;
}

static int alloc_dev_buf(void **ptr, size_t sz) {
    aclError ret = aclrtMallocCached(ptr, sz, ACL_MEM_MALLOC_NORMAL_ONLY);
    return acl_ok(ret, "aclrtMallocCached");
}
static int alloc_host_buf(void **ptr, size_t sz) {
    /* MallocHost：NPU 直写 host 内存，完全消除 D2H */
    aclError ret = aclrtMallocHost(ptr, sz);
    return acl_ok(ret, "aclrtMallocHost");
}

static int add_input(aclmdlDataset *ds, void *dev_ptr, size_t sz)
{
    aclDataBuffer *db = aclCreateDataBuffer(dev_ptr, sz);
    if (!db) { LOG_ERR("ctbg: aclCreateDataBuffer failed"); return 0; }
    aclError ret = aclmdlAddDatasetBuffer(ds, db);
    return acl_ok(ret, "aclmdlAddDatasetBuffer");
}

/* ---- public ---- */

size_t ctbg_coeff_size(void) { return g_coeff_raw_sz; }
size_t ctbg_coeff_app_size(void) { return g_coeff_app_sz; }

int ctbg_init(const ctbg_cfg_t *cfg)
{
    if (g_inited) return 0;

    g_full_w = cfg->full_w; g_full_h = cfg->full_h;
    g_low_w  = cfg->low_w;  g_low_h  = cfg->low_h;

    /* ACL 一次性初始化（与 infer.c 共享全局 aclInit，已 init 则可跳过） */
    if (!g_acl_ready) {
        aclError ret = aclInit(NULL);
        if (ret != ACL_SUCCESS && ret != ACL_ERROR_REPEAT_INITIALIZE) {
            LOG_ERR("ctbg: aclInit failed %d", (int)ret);
            goto fail;
        }
        if (!acl_ok(aclrtSetDevice(cfg->device_id), "aclrtSetDevice")) goto fail;
        if (!acl_ok(aclrtCreateContext(&g_ctx, cfg->device_id), "aclrtCreateContext")) goto fail;
        g_device = cfg->device_id;
        g_acl_ready = 1;
    }

    /* ---- load estimator ---- */
    if (!acl_ok(aclmdlLoadFromFile(cfg->est_om_path, &g_est_mid), "est load")) goto fail;
    g_est_desc = aclmdlCreateDesc();
    if (!g_est_desc || !acl_ok(aclmdlGetDesc(g_est_desc, g_est_mid), "est getDesc")) goto fail;
    if (aclmdlGetNumInputs(g_est_desc) != 1 || aclmdlGetNumOutputs(g_est_desc) != 1) {
        LOG_ERR("ctbg: estimator expect 1in/1out"); goto fail;
    }
    g_est_in_sz  = aclmdlGetInputSizeByIndex(g_est_desc, 0);
    g_est_out_sz = aclmdlGetOutputSizeByIndex(g_est_desc, 0);
    if (!alloc_dev_buf(&g_est_in_dev, g_est_in_sz)) goto fail;
    if (!alloc_host_buf(&g_est_out_dev, g_est_out_sz)) goto fail;
    g_est_in  = aclmdlCreateDataset();
    g_est_out = aclmdlCreateDataset();
    if (!g_est_in || !g_est_out) { LOG_ERR("ctbg: est dataset"); goto fail; }
    if (!add_input(g_est_in, g_est_in_dev, g_est_in_sz)) goto fail;
    if (!add_input(g_est_out, g_est_out_dev, g_est_out_sz)) goto fail;

    LOG_INFO("ctbg: estimator loaded (%zub in, %zub out)", g_est_in_sz, g_est_out_sz);

    /* ---- load apply ---- */
    if (!acl_ok(aclmdlLoadFromFile(cfg->app_om_path, &g_app_mid), "app load")) goto fail;
    g_app_desc = aclmdlCreateDesc();
    if (!g_app_desc || !acl_ok(aclmdlGetDesc(g_app_desc, g_app_mid), "app getDesc")) goto fail;
    if (aclmdlGetNumInputs(g_app_desc) != 2 || aclmdlGetNumOutputs(g_app_desc) != 1) {
        LOG_ERR("ctbg: apply expect 2in/1out (got %zuin/%zuout)",
                aclmdlGetNumInputs(g_app_desc), aclmdlGetNumOutputs(g_app_desc));
        goto fail;
    }
    g_app_in0_sz = aclmdlGetInputSizeByIndex(g_app_desc, 0);
    g_app_in1_sz = aclmdlGetInputSizeByIndex(g_app_desc, 1);
    g_app_out_sz = aclmdlGetOutputSizeByIndex(g_app_desc, 0);
    if (!alloc_dev_buf(&g_app_in0_dev, g_app_in0_sz)) goto fail;
    if (!alloc_dev_buf(&g_app_in1_dev, g_app_in1_sz)) goto fail;
    if (!alloc_host_buf(&g_app_out_dev, g_app_out_sz)) goto fail;
    g_app_in  = aclmdlCreateDataset();
    g_app_out = aclmdlCreateDataset();
    if (!g_app_in || !g_app_out) { LOG_ERR("ctbg: app dataset"); goto fail; }
    if (!add_input(g_app_in, g_app_in0_dev, g_app_in0_sz)) goto fail;
    if (!add_input(g_app_in, g_app_in1_dev, g_app_in1_sz)) goto fail;
    if (!add_input(g_app_out, g_app_out_dev, g_app_out_sz)) goto fail;

    /* 系数大小：从实际 OM descriptor 推导 */
    g_coeff_raw_sz = g_est_out_sz;      /* estimator 输出 = coeff_ch × low_h × low_w × 2 */
    g_coeff_app_sz = g_app_in1_sz;      /* apply 系数输入 */

    LOG_INFO("ctbg: apply loaded (%zub + %zub in, %zub out)", g_app_in0_sz, g_app_in1_sz, g_app_out_sz);
    LOG_INFO("ctbg: coeff raw=%zub app=%zub", g_coeff_raw_sz, g_coeff_app_sz);

    g_inited = 1;
    return 0;

fail:
    ctbg_deinit();
    return -1;
}

int ctbg_estimator_run(const void *low_fp16, void *raw_coeff_out,
                       ctbg_timing_t *timing)
{
    long t1, t2;
    if (!g_inited) { LOG_ERR("ctbg: estimator not inited"); return -1; }

    /* ACL context 绑定当前线程（控制线程与主线程不同） */
    if (!acl_ok(aclrtSetCurrentContext(g_ctx), "est setCtx")) return -1;

    /* H2D */
    (void)now_us();
    if (!acl_ok(aclrtMemcpy(g_est_in_dev, g_est_in_sz, low_fp16, g_est_in_sz,
                            ACL_MEMCPY_HOST_TO_DEVICE), "est H2D")) return -1;
    /* execute */
    t1 = now_us();
    if (!acl_ok(aclmdlExecute(g_est_mid, g_est_in, g_est_out), "est exec")) return -1;
    t2 = now_us();
    /* output is MallocHost：NPU 直写 host 内存，无需显式 D2H */
    memcpy(raw_coeff_out, g_est_out_dev, g_est_out_sz);
    if (timing) {
        timing->est_ms  = (float)(t2 - t1) / 1000.0f;
        timing->app_ms  = 0.0f;
    }
    return 0;
}

int ctbg_apply_run(const void *full_rgb_fp16, const void *coeff,
                   void *out_rgb_fp16, ctbg_timing_t *timing)
{
    long t1, t2;
    if (!g_inited) { LOG_ERR("ctbg: apply not inited"); return -1; }

    /* ACL context 绑定当前线程 */
    if (!acl_ok(aclrtSetCurrentContext(g_ctx), "app setCtx")) return -1;

    /* H2D：全分辨率帧 + 系数（已预上采样到全分辨率） */
    (void)now_us();
    if (!acl_ok(aclrtMemcpy(g_app_in0_dev, g_app_in0_sz, full_rgb_fp16, g_app_in0_sz,
                            ACL_MEMCPY_HOST_TO_DEVICE), "app H2D in0")) return -1;
    if (!acl_ok(aclrtMemcpy(g_app_in1_dev, g_app_in1_sz, coeff, g_app_in1_sz,
                            ACL_MEMCPY_HOST_TO_DEVICE), "app H2D in1")) return -1;
    /* execute */
    t1 = now_us();
    if (!acl_ok(aclmdlExecute(g_app_mid, g_app_in, g_app_out), "app exec")) return -1;
    t2 = now_us();
    /* output is MallocHost */
    memcpy(out_rgb_fp16, g_app_out_dev, g_app_out_sz);
    if (timing) {
        timing->app_ms = (float)(t2 - t1) / 1000.0f;
        timing->est_ms = 0.0f;
    }
    return 0;
}

int ctbg_apply_run_nv21(const void *nv21_frame, const void *coeff,
                        void *out_rgb_fp16, ctbg_timing_t *timing)
{
    /* 实现与 ctbg_apply_run 相同，第一参数语义是 NV21（AIPP OM 内部转换） */
    return ctbg_apply_run(nv21_frame, coeff, out_rgb_fp16, timing);
}

void ctbg_deinit(void)
{
    auto void free_ds_dev(aclmdlDataset **dp, void **p1, void **p2, void **p3) {
        if (*dp) {
            size_t n = aclmdlGetDatasetNumBuffers(*dp);
            for (size_t i = 0; i < n; i++) {
                aclDataBuffer *db = aclmdlGetDatasetBuffer(*dp, i);
                if (db) aclDestroyDataBuffer(db);
            }
            aclmdlDestroyDataset(*dp); *dp = NULL;
        }
        if (p1 && *p1) { aclrtFree(*p1); *p1 = NULL; }
        if (p2 && *p2) { aclrtFree(*p2); *p2 = NULL; }
        if (p3 && *p3) { aclrtFree(*p3); *p3 = NULL; }
    }
    void free_host(void **p) { if (p && *p) { aclrtFreeHost(*p); *p = NULL; } }
    /* apply */
    if (g_app_desc) { aclmdlDestroyDesc(g_app_desc); g_app_desc = NULL; }
    if (g_app_mid)  { aclmdlUnload(g_app_mid); g_app_mid = 0; }
    free_ds_dev(&g_app_in,  &g_app_in0_dev, &g_app_in1_dev, NULL);
    free_host(&g_app_out_dev);
    if (g_app_out) { aclmdlDestroyDataset(g_app_out); g_app_out = NULL; }
    /* estimator */
    if (g_est_desc) { aclmdlDestroyDesc(g_est_desc); g_est_desc = NULL; }
    if (g_est_mid)  { aclmdlUnload(g_est_mid); g_est_mid = 0; }
    free_ds_dev(&g_est_in,  &g_est_in_dev, NULL, NULL);
    free_host(&g_est_out_dev);
    if (g_est_out) { aclmdlDestroyDataset(g_est_out); g_est_out = NULL; }
    /* ACL — 不调 aclFinalize，与 infer.c 共享全局 ACL lifecycle */
    if (g_ctx)  { aclrtDestroyContext(g_ctx); g_ctx = NULL; }
    if (g_acl_ready) { aclrtResetDevice(g_device); g_acl_ready = 0; }
    g_inited = 0;
}
