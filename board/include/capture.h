#ifndef SOCCHINA_CAPTURE_H
#define SOCCHINA_CAPTURE_H

/* capture — 相机采集驱动（数据通路第 1–4 级：MIPI/VI/sensor/ISP 起链）。
 *
 * 传感器：OS08A20（8M 30fps 12bit 线性模式；WDR 留待架构 §6 点 6 验证）。
 * 板上 OS08A20 接在 sensor1/J4（I2C bus7、时钟/复位源 1、VI dev2），即
 * sensor_index=1；sensor_index=0 对应 sensor0 位（bus5、VI dev0）。
 *
 * 实现封装厂商 sample_comm 层（MIPI 配置、sensor 驱动注册、ISP 3A 注册与
 * ISP run 线程均在其内），编译需 SS928_SDK_ROOT 指向含 src/common 的
 * MPP sample 包（见 board/CMakeLists.txt ENABLE_SDK 分支）。
 *
 * 前置：SYS/VB 已由上层初始化（VB 公共池须按传感器全幅尺寸配置，
 * 用 capture_query_in_size 取尺寸）。VI 工作在 ONLINE 模式，CPU 取帧
 * 必须经 VPSS（见 vpss.h）。
 */

#define CAPTURE_SENSOR_INDEX_DEFAULT 1 /* 海鸥派 OS08A20 在 sensor1/J4 */

/* 查询传感器输出尺寸（如 3840x2160），可在 capture_init 之前调用，
 * 供上层计算 VB 公共池块大小。成功返回 0。 */
int capture_query_in_size(unsigned *width, unsigned *height);

/* 起整条采集链：sensor 时钟 → VI/VPSS 工作模式 → MIPI/sensor/VI/ISP。
 * 启动前清理残留 ISP0 状态（防 already inited / 0xa01c800c）。成功返回 0。 */
int capture_init(int sensor_index);

/* 把 VI pipe0/chn0 绑定到 VPSS 组（VI ONLINE 下数据走硬件直通）。 */
int capture_bind_vpss(int vpss_grp);
int capture_unbind_vpss(int vpss_grp);

/* 停 VI/ISP/sensor。可重复调用。 */
int capture_deinit(void);

#endif /* SOCCHINA_CAPTURE_H */
