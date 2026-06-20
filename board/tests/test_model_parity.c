/* 固定文件输入的 OM 数值对拍工具，不启动相机/ISP/VPSS。 */

#include "infer.h"
#include "log.h"

#ifndef WITH_SS928_SDK

int main(void)
{
    LOG_WARN("test_model_parity skipped: built without SS928 SDK");
    return 0;
}

#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *model = NULL, *input_path = NULL, *output_path = NULL;
    FILE *fp = NULL;
    void *input = NULL;
    long input_size;
    static float output[PIPELINE_COTF_LUT_FLOAT_COUNT];
    infer_cfg_t cfg = {0};
    infer_timing_t timing = {0};
    int rc = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) model = argv[++i];
        else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) input_path = argv[++i];
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) output_path = argv[++i];
    }
    if (model == NULL || input_path == NULL || output_path == NULL) {
        fprintf(stderr, "usage: %s --model m.om --input input.bin --output output.f32\n", argv[0]);
        return 2;
    }
    fp = fopen(input_path, "rb");
    if (fp == NULL || fseek(fp, 0, SEEK_END) != 0 || (input_size = ftell(fp)) <= 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        LOG_ERR("cannot read input %s", input_path);
        goto cleanup;
    }
    input = malloc((size_t)input_size);
    if (input == NULL || fread(input, 1, (size_t)input_size, fp) != (size_t)input_size) {
        LOG_ERR("input read failed");
        goto cleanup;
    }
    fclose(fp);
    fp = NULL;

    cfg.model_path = model;
    cfg.device_id = 0;
    cfg.input_width = PIPELINE_CONTROL_WIDTH;
    cfg.input_height = PIPELINE_CONTROL_HEIGHT;
    cfg.lut_dim = PIPELINE_COTF_LUT_DIM;
    cfg.input_mode = PIPELINE_INPUT_COPY;
    if (infer_init(&cfg) != 0 ||
        infer_run_buffer(input, (size_t)input_size, output, PIPELINE_COTF_LUT_FLOAT_COUNT,
                         &timing) != 0) {
        goto cleanup;
    }
    fp = fopen(output_path, "wb");
    if (fp == NULL ||
        fwrite(output, sizeof(float), PIPELINE_COTF_LUT_FLOAT_COUNT, fp) !=
            PIPELINE_COTF_LUT_FLOAT_COUNT) {
        LOG_ERR("output write failed");
        goto cleanup;
    }
    LOG_INFO("parity output: %s (%u float32), copy=%.3f exec=%.3f out=%.3f total=%.3fms",
             output_path, PIPELINE_COTF_LUT_FLOAT_COUNT, timing.input_copy_ms,
             timing.execute_ms, timing.output_copy_ms, timing.total_ms);
    rc = 0;

cleanup:
    if (fp != NULL) fclose(fp);
    free(input);
    (void)infer_deinit();
    return rc;
}

#endif
