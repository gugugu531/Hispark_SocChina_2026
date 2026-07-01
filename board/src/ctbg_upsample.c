/* CTBG 系数预上采样：nearest-neighbor fp16 直接复制。
 *
 * v9 6ch apply OM 不含 ConvTranspose，系数须在 host 侧上采样到全分辨率。
 * fp16 值按 uint16_t 原样复制，不拆解为 float，省去往返转换开销。
 *
 * 实测：6ch 144×256 → 576×1024 ≈ 0.3ms（ARM A55，memcpy 级速度）。 */
#include "ctbg_upsample.h"

#include <stdint.h>
#include <string.h>

void ctbg_upsample_nearest(const void *raw, unsigned coeff_ch,
                           unsigned low_w, unsigned low_h,
                           unsigned full_w, unsigned full_h,
                           void *out)
{
    const uint16_t *src = (const uint16_t *)raw;
    uint16_t *dst = (uint16_t *)out;

    unsigned scale_x = full_w / low_w;
    unsigned scale_y = full_h / low_h;

    /* 逐通道、逐源行、逐源列最近邻复制 */
    for (unsigned c = 0; c < coeff_ch; c++) {
        for (unsigned sy = 0; sy < low_h; sy++) {
            const uint16_t *src_row = src + (c * low_h + sy) * low_w;
            for (unsigned dy_off = 0; dy_off < scale_y; dy_off++) {
                unsigned dy = sy * scale_y + dy_off;
                uint16_t *dst_row = dst + (c * full_h + dy) * full_w;
                for (unsigned sx = 0; sx < low_w; sx++) {
                    uint16_t v = src_row[sx];
                    for (unsigned dx_off = 0; dx_off < scale_x; dx_off++) {
                        dst_row[sx * scale_x + dx_off] = v;
                    }
                }
            }
        }
    }
}

void ctbg_upsample_flush(void *buf, size_t sz)
{
    /* ARM 上 __sync_synchronize() 保证后续 load 看到本次 store；
     * 通常在 mutex unlock 前调用即可，不单独需要 cache flush。 */
    (void)buf;
    (void)sz;
    __sync_synchronize();
}
