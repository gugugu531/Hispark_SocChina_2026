#ifndef SOCCHINA_CTBG_ISP_MAP_H
#define SOCCHINA_CTBG_ISP_MAP_H

#include <stdint.h>

/* CTBG 系数图 → ISP DRC/LDCI 参数映射。
 *
 * 原理：estimator 输出的 6ch 逐像素系数图虽然无法用 NPU 实时施加（89ms），
 * 但其空间分布信息可以聚合为 ISP 硬件能直接消费的局部参数。
 * SS928 的 DRC 模块内部就是空间自适应的——17×15 分块，33 档亮度索引，
 * 200 节点自定义色调曲线。本模块将 CTBG 的空间意图翻译为 DRC/LDCI 参数。
 *
 * 管线角色：control_worker → estimator → ctbg_isp_map_apply() → ISP 30fps。 */

/* 分块统计：从 576×1024 fp16 系数图聚合 17×15 块均值 */
#define CTBG_ISP_MAP_ROWS 15
#define CTBG_ISP_MAP_COLS 17

typedef struct {
    float a_dark_mean;    /* 暗部增益 a 的块均值（>1 表示需要提亮） */
    float a_bright_mean;  /* 亮部增益 a 的块均值（<1 表示需要压暗） */
    float g_dark_mean;    /* 暗部 gamma（<1 拉暗部） */
    float block_luma;     /* 块内 a_dark 均值归一化后的亮度代理（0=最暗, 1=最亮） */
} ctbg_block_stat_t;

/* 聚合系数图为 17×15 块统计。
 * coeff_up: 预上采样的 6ch fp16 系数 (6×576×1024)
 * full_w/full_h: 全分辨率尺寸
 * blocks: 输出 17×15 块统计数组 (调用者分配) */
void ctbg_isp_map_blocks(const uint16_t *coeff_up,
                         unsigned full_w, unsigned full_h,
                         ctbg_block_stat_t blocks[CTBG_ISP_MAP_ROWS][CTBG_ISP_MAP_COLS]);

/* 将块统计映射为 DRC tone_mapping_value[200] 自定义色调曲线。
 * 根据暗块/亮块比例，构造上凸（提暗部）或下凹（压亮部）的色调曲线。
 * blocks: 17×15 块统计
 * tmv: 输出 200 节点色调曲线（调用者分配，[0,65535] 范围）
 * strength: 全局强度系数 (0.0-1.0) */
void ctbg_isp_map_drc_tone(const ctbg_block_stat_t blocks[CTBG_ISP_MAP_ROWS][CTBG_ISP_MAP_COLS],
                           uint16_t tmv[200], float strength);

/* 将块统计映射为 DRC local_mixing 细节增强系数。
 * 暗块增强正细节（提亮暗部纹理），亮块适度降低防止 halo。
 * blocks: 17×15 块统计
 * bright_lut: 输出 33 节点 local_mixing_bright (FilterX)
 * dark_lut:  输出 33 节点 local_mixing_dark (FilterX) */
void ctbg_isp_map_drc_mixing(const ctbg_block_stat_t blocks[CTBG_ISP_MAP_ROWS][CTBG_ISP_MAP_COLS],
                             uint8_t bright_lut[33], uint8_t dark_lut[33]);

/* 一键应用：从系数图映射所有 DRC/LDCI 参数并刷新 ISP。
 * 在 control_worker 中 estimator 成功刷新后调用。
 * 返回 0 成功，-1 失败。 */
int ctbg_isp_map_apply(const uint16_t *coeff_up,
                       unsigned full_w, unsigned full_h,
                       float strength);

#endif /* SOCCHINA_CTBG_ISP_MAP_H */
