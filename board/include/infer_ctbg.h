#ifndef SOCCHINA_INFER_CTBG_H
#define SOCCHINA_INFER_CTBG_H

#include <stddef.h>

/* CTBG 双 OM 推理模块（estimator + apply 串联），支持 v8 18ch 与 v9 6ch。
 *
 * 管线角色（替代 CoTF param-net → ISP CLUT 旁路）：
 *   estimator — chn2 256×144 缩略图 → raw 系数（异步低频）
 *   apply    — 全分辨率 1024×576 → 增强 RGB fp16（每帧同步）
 *
 * v8 18ch（已验证 29.8fps）：
 *   estimator 输出 18ch raw logits（144×256 NCHW fp16）
 *   apply OM 内部做 group ConvTranspose×2 上采样 + tanh/exp decode + luma 插值 + gamma 施加
 *
 * v9 6ch（推荐，~48fps）：
 *   estimator 输出 6ch scalar raw（144×256 NCHW fp16）
 *   host 侧 nearest-neighbor 预上采样到全分辨率（576×1024）
 *   apply OM 纯 elementwise（tanh/exp/luma blend/gamma/clip），无 ConvTranspose
 *
 * 两个 OM 的输入/输出均为 NCHW fp16；NV21→RGB 转换由 AIPP inserted op 完成。 */

typedef struct {
    const char *est_om_path;   /* estimator OM 路径 */
    const char *app_om_path;   /* apply OM 路径 */
    int device_id;
    unsigned full_w;           /* 全分辨率宽（如 1024） */
    unsigned full_h;           /* 全分辨率高（如 576） */
    unsigned low_w;            /* 低分辨率宽（如 256） */
    unsigned low_h;            /* 低分辨率高（如 144） */
    unsigned coeff_ch;         /* 系数通道数（v8=18, v9=6） */
} ctbg_cfg_t;

typedef struct {
    float est_ms;              /* estimator 推理耗时 */
    float app_ms;              /* apply 推理耗时 */
} ctbg_timing_t;

/* 初始化：加载两个 OM，分配 device 内存。成功返回 0。 */
int ctbg_init(const ctbg_cfg_t *cfg);

/* estimator 推理：low_fp16 是 NCHW fp16 输入（3×low_h×low_w 个 uint16_t），
 * raw_coeff_out 由调用者分配（coeff_ch×low_h×low_w 个 uint16_t）。
 * 调用频率：场景变化时或首次。 */
int ctbg_estimator_run(const void *low_fp16, void *raw_coeff_out,
                       ctbg_timing_t *timing);

/* apply 推理（NCHW fp16 输入版）：full_rgb_fp16 是 NCHW fp16（3×full_h×full_w），
 * coeff 是预上采样后的系数（coeff_ch×full_h×full_w 个 uint16_t）。 */
int ctbg_apply_run(const void *full_rgb_fp16, const void *coeff,
                   void *out_rgb_fp16, ctbg_timing_t *timing);

/* apply 推理（NV21 输入版）：nv21_frame 是 YUV420SP_U8（W×H×3/2 字节），
 * OM 必须挂 AIPP（NV21→RGB NCHW fp16）作为 inserted op。
 * coeff 是预上采样后的系数（coeff_ch×full_h×full_w 个 uint16_t）。 */
int ctbg_apply_run_nv21(const void *nv21_frame, const void *coeff,
                        void *out_rgb_fp16, ctbg_timing_t *timing);

/* 取 estimator 输出的 raw 系数长度（字节） */
size_t ctbg_coeff_size(void);

/* 取 apply 期望的系数输入长度（字节），
 * v8：raw 低分辨率 = coeff_ch×low_h×low_w×2
 * v9：预上采样全分辨率 = coeff_ch×full_h×full_w×2 */
size_t ctbg_coeff_app_size(void);

/* 释放两个 OM 及所有 device 资源 */
void ctbg_deinit(void);

#endif /* SOCCHINA_INFER_CTBG_H */
