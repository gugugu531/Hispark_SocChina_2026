#ifndef SOCCHINA_ISP_H
#define SOCCHINA_ISP_H

#include "control.h"

/* isp — ISP 运行时参数面与统计读取（数据通路第 2–4 级与第 11 级控制的接口）。
 *
 * ISP 本体（3A 注册与 run 线程）由 capture 模块随 VI 起链拉起；本模块只做
 * 运行时控制：抗闪烁/AE 曝光（过曝防线之一）、AE 统计（场景自适应控制的
 * 输入，架构 §6 点 4）、dehaze/DRC/LDCI 增强块（双向曝光校正的 ISP 底座）。
 * 所有接口要求 capture_init 已成功（ISP pipe0 在跑）。
 *
 * 3D-LUT（CLUT）：CoTF 路线的硬件施加端——NN 在低分辨率出 LUT（实测 ~5ms），由 ISP 硬件
 * 做全分辨率三线性施加（零 NPU；NPU 无法跑 3D-LUT 三线性算子）。LUT 由
 * models/tools/cotf_lut_pack.py 从 NN 立方 LUT 打包成 5508 节点 u32 格式。
 */

/* 三态开关：增强块统一用法。 */
typedef enum {
    ISP_BLOCK_OFF = 0,  /* 关闭 */
    ISP_BLOCK_AUTO,     /* 启用，自动强度（SDK 按 ISO 内插） */
    ISP_BLOCK_MANUAL,   /* 启用，手动强度 */
} isp_block_mode_t;

/* 抗闪烁：freq_hz 取 50/60 启用，0 关闭。室内灯光下建议 50。 */
int isp_set_antiflicker(int freq_hz);

/* AE 曝光补偿（0–255，默认 0x38；调小压高光、调大提暗部——过曝防线的主旋钮）。 */
int isp_set_ae_compensation(unsigned char comp);

/* 手动曝光：exp_time_us 曝光时间(µs)、again_x1024 模拟增益(1024=1x)。
 * 两参均为 0 时恢复全自动 AE。 */
int isp_set_exposure_manual(unsigned exp_time_us, unsigned again_x1024);

/* AE 曝光策略：高光优先（压高光，过曝防线之一）或暗部优先。
 * highlight_prior 非 0 = 高光优先；0 = 暗部优先（SDK 默认行为按 PQ 表）。 */
int isp_set_ae_strategy(int highlight_prior);

/* WDR 长短曝光比（仅 WDR 模式有效；staggered HDR 的主画质旋钮）。
 * ratio_x64：长曝光/短曝光 ×64（0x40=1 倍，范围 0x40–0x4000）；0 = 回固件自动。 */
int isp_set_wdr_exposure_ratio(unsigned ratio_x64);

/* 读 AE 1024-bin 直方图统计并归约为 luma_stats_t（mean 0..255 与黑/白场裁剪比例），
 * 直接供 control_decide 使用。低频调用（控制环周期），不必逐帧。 */
int isp_get_luma_stats(luma_stats_t *stats);

/* 增强块：strength 仅 MANUAL 模式生效。
 * dehaze strength 0–255；DRC strength 0–1023；LDCI 无手动强度（MANUAL 等同 AUTO）。 */
int isp_set_dehaze(isp_block_mode_t mode, unsigned char strength);
int isp_set_drc(isp_block_mode_t mode, unsigned strength);
int isp_set_ldci(isp_block_mode_t mode);

/* CLUT（3D-LUT）开关 + 全局 RGB 增益（gain 0–4095，1024≈1x）。CoTF 路线硬件施加端。 */
int isp_set_clut(isp_block_mode_t mode, unsigned gain_r, unsigned gain_g, unsigned gain_b);
/* 加载 3D-LUT 系数：lut 为 5508 个 u32（每节点 3×10bit RGB），由 tools/cotf_lut_pack.py 生成。 */
int isp_load_clut_lut(const unsigned int *lut, unsigned len);

/* 回读当前 CLUT 系数（硬件默认表基线，几何天然正确）。len 须 == OT_ISP_CLUT_LUT_LENGTH(5508)。 */
int isp_clut_get_coeff(unsigned int *lut, unsigned len);

/* 几何无关的色调校正（CoTF 曝光用例）：把 tone 曲线施加在**节点输出值**上（与 mesh 几何无关，
 * 绕开未标定的 index↔(r,g,b) 映射），再加载到 ISP CLUT 并启用。tone=BYPASS 时关闭 CLUT。
 *   - BRIGHTEN  提亮暗部（gamma<1）           - COMPRESS  压制高光（gamma>1）
 *   - BIDIR     双向：log 曲线压缩动态范围（提暗部+压高光）
 * base_lut = isp_clut_get_coeff 读回的默认表基线；strength∈[0,1] 校正强度。 */
typedef enum {
    ISP_TONE_BYPASS = 0,
    ISP_TONE_BRIGHTEN,
    ISP_TONE_COMPRESS,
    ISP_TONE_BIDIR,
} isp_tone_t;
int isp_clut_apply_tone(const unsigned int *base_lut, unsigned len, isp_tone_t tone, float strength);

#endif /* SOCCHINA_ISP_H */
