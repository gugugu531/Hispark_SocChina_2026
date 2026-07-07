# Agent Prompt: LVGL 板端 UI 上板收尾

## 任务
修复 LVGL 板端触摸 UI 的白线/显示问题，完成上板验证。

## 背景
- SS928/SD3403 板端，OS08A20 sensor，VO→HDMI 1024×600 DVI 面板(Waveshare 7 寸触摸)。
- LVGL v9.5.0 交叉编译已通(`--DENABLE_LVGL=ON`)，产出 `socchina_app_lvgl`（含 UI 静态库 `socchina_ui`）。
- 方案 A:GFBG G0 图形层叠加。**G0 固定绑定 DHD0**（SS928 规格），不可改为其他层；G0 预期 ARGB8888。
- UI 代码:`board/ui/ui_lvgl.c/.h`(可移植 UI:顶栏 + 视频占位区 + 右栏 STATUS/PRESETS/REALTIME CONTROL)、`board/ui/ui_port_board.c/.h`(板端 port:GFBG/fbdev/evdev/worker)、`board/ui/lv_conf_board.h`(32bpp, LINUX_FBDEV+EVDEV on, SDL off)。
- 主机模拟器:`board/sim/`(SDL,build via `cmake -S board/sim -B build-sim && cmake --build build-sim`)，**仅验证 UI 布局与交互逻辑，不涉及 GFBG**。

## 已知问题:白线
### 现象
HDMI 屏幕下半部出现大量白色水平线条。

### 根因
`ui_port_board.c` 的 `gfbg_setup()` ioctl 序列(FBIOPUT_VSCREENINFO/FBIOPUT_LAYER_INFO/FBIOPUT_ALPHA_GFBG/FBIOPUT_SHOW_GFBG)**silently 不生效**:
- fbdev 实际保持在 VO 初始化后的 **3840×2160 ARGB1555 16bpp**(由 `display_init`/厂商 sample 设为 sensor native)
- GFBG ioctl 的正确时序依赖早于 `lv_linux_fbdev_create`（设备已 open 但无冲突）
- 当 ioctl 静默失败时，LVGL 仍以 1024×600 32bpp 读写 fb，实际 fb 是 3840×2160 16bpp → **行步长/像素格式全错 → 大面积白线**

### 修复方向(方案 A)
把 fbdev **真的改为 1024×600 ARGB8888**，在**每次 ioctl 后立即 readback 验证**：
1. `FBIOPUT_VSCREENINFO` → `FBIOGET_VSCREENINFO` 回读确认 xres/yres/bpp/ARGB 偏移
2. `FBIOPUT_LAYER_INFO` → `FBIOPUT_LAYER_INFO`(GFBG ioctl，用 `FBIOPUT_GET_LAYER_INFO` 回读确认 canvas/display/screen size)
3. `FBIOPUT_ALPHA_GFBG` → 回读确认 alpha_en 位
4. `FBIOPUT_SHOW_GFBG` → 回读确认 show 位
5. 任一 ioctl 失败或回读不匹配 → **报错退出而非静默继续**
6. 如果 GFBG ioctl 始终不生效，备选:不调 gfbg_setup，直接用 `display_init` 后的 VO fb 属性匹配 LVGL(32bpp @ 1024×600 或降为 16bpp ARGB1555)——改 `lv_conf_board.h` 的 `LV_COLOR_DEPTH` + `lv_linux_fbdev` 的像素格式协商

## 其他上板验证项
- **透明叠加**:修复后运行 `ui_lvgl_set_overlay_mode()`(屏 bg+视频占位区置 `LV_OPA_TRANSP`)，确认 GFBG G0 alpha 通道使 VO 视频层从下方透出(顶栏/侧边栏仍不透明)
- **触摸标定**:`/dev/input/event0` 已开箱即用(Waveshare USB 触摸,1:1 像素映射,无需额外校准)
- **flicker 目视**:确认无屏幕撕裂或闪烁(双缓冲+抗闪烁 auto 预期消除)
- **首次上板预期**:屏幕不透明 → 满屏 LVGL UI chrome(覆盖视频)，属预期；切 overlay 模式后应透出视频

## 构建与部署
```sh
# 构建(需设置交叉编译环境变量,见 AGENTS.md/项目记忆 hispark-build-env)
export CROSS_COMPILE_ROOT=...
export SS928_SDK_ROOT=...
export ISL_LIB_DIR=...
cd board && cmake -B ../build -S . -DCMAKE_BUILD_TYPE=Release -DENABLE_SDK=ON -DENABLE_LVGL=ON
cmake --build ../build --target socchina_app -j4
# 产物: build/socchina_app

# 部署(直连以太网)
scp build/socchina_app hispark-direct:/root/socchina-2026/socchina_app_lvgl
ssh hispark-direct '
  systemctl stop socchina-stream
  # 运行 LVGL UI(注意: --no-display 关 VO→HDMI,有冲突;LVGL 需要 display)
  /root/socchina-2026/socchina_app_lvgl --stream --sensor 1 --fps 30 ...
  # 或直接 systemctl start socchina-stream(如果 /usr/local/sbin/socchina-start 指向 socchina_app_lvgl)
'
```

## 验收标准
- [ ] HDMI 屏幕 1024×600 无白线/花屏
- [ ] UI chrome(顶栏+侧边栏卡片)正常渲染(琥珀/金色主题)
- [ ] 透明叠加模式:视频占位区透出 VO 视频画面
- [ ] 触摸可用(event0),滑块/预设按钮响应
- [ ] UI 操作参数经 `app_ctrl_push` → 控制线程生效(与 web 同一队列)
- [ ] host sim `build-sim/sim_ui` 布局与板端一致(用于离线迭代)

## 关键约束
- ⚠️ LVGL 与 VO 显示共用 GFBG G0——不能 `--no-display`，否则 G0 不使能/LVGL 画到不存在的层
- ⚠️ 板端首次上板可能满屏 LVGL(盖住视频)，需切 `ui_lvgl_set_overlay_mode()` 才透出视频
- ⚠️ 停/启 socchina-stream 时 media pipeline 可能残留 VB 状态(`ss_mpi_vb_set_conf failed`);恢复:`cd /opt/ko && ./load_ss928v100 -a`
- LVGL 不依赖 NPU，可 `--no-nn` 跑;`--paramnet` 选项与 LVGL 独立
