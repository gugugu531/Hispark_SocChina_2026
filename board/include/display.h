#ifndef SOCCHINA_DISPLAY_H
#define SOCCHINA_DISPLAY_H

/* display — VO→HDMI 本地显示驱动（数据通路第 10a 级）。
 *
 * 目标面板：Waveshare 7 寸 1024x600@60 类 DVI 面板（海鸥派 HDMI0）。
 * 已验证可点亮的组合：原生 1024x600 时序 + DVI 模式 + 音频禁用 + RGB full CSC。
 * 时序/PLL 参数来源见 board/README.md「display 模块」一节。
 *
 * 本模块只管 VO 设备/视频层/通道与 HDMI；SYS/VB 的初始化由上层
 * （主程序或测试驱动）负责，且必须先于 display_init 完成。
 */

#define DISPLAY_WIDTH      1024u
#define DISPLAY_HEIGHT     600u
#define DISPLAY_FRAME_RATE 60u

/* 启动 VO dev0/layer0/chn0 + HDMI0（DVI 1024x600@60）。成功返回 0。 */
int display_init(void);

/* 送一帧到显示通道（零拷贝，帧仍在 VB 池中）。
 * frame_info 实际为 const ot_video_frame_info*；用 void* 保持本头文件 SDK-free。
 * timeout_ms：<0 阻塞等待，0 立即返回，>0 毫秒超时。成功返回 0。 */
int display_send_frame(const void *frame_info, int timeout_ms);

/* 按创建逆序停止 HDMI 与 VO。成功返回 0。可重复调用。 */
int display_deinit(void);

#endif /* SOCCHINA_DISPLAY_H */
