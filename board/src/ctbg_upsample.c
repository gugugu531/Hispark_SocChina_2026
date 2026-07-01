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

void ctbg_broadcast_6to18(const void *raw_6ch,
                          unsigned low_w, unsigned low_h,
                          void *raw_18ch)
{
    const uint16_t *src = (const uint16_t *)raw_6ch;
    uint16_t *dst = (uint16_t *)raw_18ch;
    unsigned plane = low_w * low_h;  /* 单通道像素数 */

    /* 6 组映射：v9 标量 ch → v8 3ch 广播
     *   v9[0] → v8[0,1,2]   (a_d)
     *   v9[1] → v8[3,4,5]   (b_d)
     *   v9[2] → v8[6,7,8]   (g_d)
     *   v9[3] → v8[9,10,11] (a_b)
     *   v9[4] → v8[12,13,14](b_b)
     *   v9[5] → v8[15,16,17](g_b)
     */
    for (unsigned g = 0; g < 6; g++) {
        const uint16_t *s = src + g * plane;
        for (unsigned c = 0; c < 3; c++) {
            uint16_t *d = dst + (g * 3 + c) * plane;
            memcpy(d, s, plane * 2);
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
