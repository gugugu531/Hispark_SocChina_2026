#ifndef SOCCHINA_CTBG_UPSAMPLE_H
#define SOCCHINA_CTBG_UPSAMPLE_H

#include <stddef.h>

/* 系数预上采样：nearest-neighbor，fp16 直接复制，不做数值转换。
 *
 * v9 6ch apply OM 无内部 ConvTranspose，需要 host 侧先将
 * estimator 输出的低分辨率系数（coeff_ch×low_h×low_w fp16）
 * 上采样到全分辨率（coeff_ch×full_h×full_w fp16）再送入 apply。
 *
 * 输入：
 *   raw      — 低分辨率系数（coeff_ch × low_h × low_w 个 uint16_t）
 *   coeff_ch — 系数通道数（6 或 18）
 *   low_w/h  — 低分辨率尺寸（如 256×144）
 *   full_w/h — 全分辨率尺寸（如 1024×576）
 *   scale     — 上采样倍数（full_w/low_w，必须整除）
 *   out       — 输出缓冲区（coeff_ch × full_h × full_w 个 uint16_t），
 *               调用者分配，大小 = coeff_ch * full_w * full_h * 2 */
void ctbg_upsample_nearest(const void *raw, unsigned coeff_ch,
                           unsigned low_w, unsigned low_h,
                           unsigned full_w, unsigned full_h,
                           void *out);

/* 一次性内存屏障（写回后调用），确保上采样结果对所有 CPU 读者可见 */
void ctbg_upsample_flush(void *buf, size_t sz);

#endif /* SOCCHINA_CTBG_UPSAMPLE_H */
