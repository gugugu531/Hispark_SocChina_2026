# board — 板端应用

海思 SS928 交叉编译（`aarch64-mix210-linux`）的板端应用，CMake 构建。常规嵌入式 Linux 工程结构。

## 结构

```
board/
├── CMakeLists.txt
├── cmake/toolchain-aarch64-mix210-linux.cmake   # 交叉编译 toolchain file
├── include/     # 头文件: log.h / version.h / control.h / display.h / pipeline.h(后续共享帧结构)
├── src/         # 源码, 按功能分文件: main.c / control.c / display.c (+ capture.c isp.c infer.c ... 后续)
└── tests/       # 单元/驱动测试: test_<名字>.c, 各编一个可执行 + ctest
```

- `src/` 平铺，每个功能一个 `.c`（+ `include/` 里对应 `.h`）；`main.c` 是主程序入口。
- `src/*.c`（除 `main.c`）编成静态库 `socchina`，主程序与测试共用。
- `tests/test_*.c` 每个自动编成可执行并注册到 `ctest`。
- 新增功能：往 `src/` 加 `.c`、`include/` 加 `.h`、（可选）`tests/` 加 `test_*.c`，无需改 CMake（glob 自动收集）。

## 构建

```sh
export CROSS_COMPILE_ROOT=/path/to/aarch64-mix210-linux
export ISL_LIB_DIR=/path/to/libisl          # 提示缺 libisl.so.19 时设置
# export SS928_SDK_ROOT=/path/to/ss928_sdk  # 接入硬件后再设
scripts/build_board.sh                      # 产物 build/socchina_app (aarch64)
```

## 模块

### display — VO→HDMI 本地显示驱动（数据通路第 10a 级）

接口见 `include/display.h`：`display_init` / `display_send_frame` / `display_deinit`。

- 固定输出 Waveshare 7 寸 **1024x600@60 类 DVI 面板**（HDMI0）：原生时序 + DVI 模式（`hdmi_en=FALSE`）+
  音频禁用 + RGB full CSC（`BT709FULL_TO_RGBFULL`）。该组合是此面板已验证可点亮的唯一稳定路径。
- 时序为面板 EDID 原生 Modeline `49.00 MHz, 1024 1072 1168 1312, 600 603 613 624, -hsync +vsync`；
  `ot_vo_sync_info.hbb/vbb` 按 SDK sample 约定 = 同步脉冲 + 后廊（`hbb=240`、`vbb=21`）；
  VO PLL `fb_div=49 / ref_div=1 / post_div1=6 / post_div2=4`。来源：前期点亮实验
  （工作区 `experiments/display-hdmi/README.md`，含失败参数与排查记录）。
- 视频层 chn0 接收 NV21（`YVU_SEMIPLANAR_420`）帧，零拷贝送显；上层（VGS 合成输出）经
  `display_send_frame` 直接送 VB 帧。
- 模块只管 VO/HDMI；**SYS/VB 由上层先初始化**。`display_init` 启动前会尽力清理残留 VO/HDMI 状态。
- **黑场预帧**：`display_init` 先把 VO 通道稳定在一帧全黑 NV21 上、等 2 个 vsync，再
  `ss_mpi_hdmi_start` 使能 PHY——板端实测若直接启动，面板锁定瞬间会显示无信号样杂线。
  预帧需公共 VB 池有一块 ≥1024x600 NV21 空闲块，取不到时退化为直接启动（仅告警）。
- 链接依赖：`libss_mpi.a + libss_hdmi.a + libsecurec.a`（音频 VQE/AAC 库为 `libss_mpi.a`
  的被动依赖，本应用不用音频），已在 CMake `ENABLE_SDK` 分支配置。

板端冒烟测试 `test_display`（验证架构 §6 待验证点 5——VO 视频层送帧替代 GFBG 是否无 flicker）：

```sh
# 前置: 板上无其他媒体进程争用 SYS/VB/VO(勿与厂商 sample_hdmi 并跑)
scp build/test_display root@192.168.1.168:/root/socchina-2026/
ssh root@192.168.1.168 '/root/socchina-2026/test_display 10'   # 10 秒, 彩条/灰阶渐变交替
# 观察面板画面; 健康判据见 docs/board-operations.md §3 (/proc/umap/hdmi0)
```

板端实测（2026-06-10，板卡刚重启、媒体状态干净）：

- 命令：`/root/socchina-2026/test_display 12` 与 `test_display 20` 各一次。
- 结果：两次均 PASS（240 帧/12s、501 帧/20s），全程无 MPI 错误；`ss_mpi_vo_send_frame`
  阻塞送帧单次 0–16 ms。稳态 `/proc/umap/hdmi0`：`tmds mode: DVI`、`rsen/phy output enable: YES`、
  `hactive/vactive: 1024/600`、`hsync/vsync total: 1312/624`、HSync 负/VSync 正；`/proc/umap/vo`：
  `intf_sync USER`、`pixel_clk 49000`、layer 1024x600 YVU-SP420——与前期实验已验证状态一致。
  退出后 `run status: CLOSE`、计数器归零、无残留进程，dmesg 无 HDMI/VO 报错。
- 注意：`hdmi_start` 后约 4s 内 `/proc/umap/hdmi0` 时序计数器可能短暂显示驱动默认 1080p
  （瞬态，未刷新），以稳态读数为准。
- 待人工确认：面板画面内容（彩条/渐变交替）与 flicker 观感（架构 §6 待验证点 5）需现场目视。

## 测试

- **主机单元测试**（SDK-free 纯逻辑，如 control）：`scripts/test_host.sh`（本机 cc 直接编译运行）。
- **板端测试**（触硬件）：交叉编译出的 `test_<名字>` 部署到板上手动跑。

详见 `../docs/development-guide.md`。
