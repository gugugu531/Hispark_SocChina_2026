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
 * 3D-LUT（CLUT）需配合标定的 LUT 表加载，待色调映射方案确定后再接入。
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

/* 读 AE 1024-bin 直方图统计并归约为 luma_stats_t（mean 0..255 与黑/白场裁剪比例），
 * 直接供 control_decide 使用。低频调用（控制环周期），不必逐帧。 */
int isp_get_luma_stats(luma_stats_t *stats);

/* 增强块：strength 仅 MANUAL 模式生效。
 * dehaze strength 0–255；DRC strength 0–1023；LDCI 无手动强度（MANUAL 等同 AUTO）。 */
int isp_set_dehaze(isp_block_mode_t mode, unsigned char strength);
int isp_set_drc(isp_block_mode_t mode, unsigned strength);
int isp_set_ldci(isp_block_mode_t mode);

#endif /* SOCCHINA_ISP_H */
