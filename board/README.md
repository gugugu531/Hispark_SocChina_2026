# board — 板端应用

海思 SS928 交叉编译（`aarch64-mix210-linux`）的板端应用，CMake 构建。常规嵌入式 Linux 工程结构。

## 结构

```
board/
├── CMakeLists.txt
├── cmake/toolchain-aarch64-mix210-linux.cmake   # 交叉编译 toolchain file
├── include/     # 头文件: pipeline / capture / vpss / isp / infer / lut_bridge / control / display / stream
├── src/         # 源码, 按功能分文件: main / capture / vpss / isp / infer / lut_bridge / control / display
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

板端命令统一通过 `BOARD` 指定目标。受管网络推荐使用 `~/.ssh/config` 别名，电脑网线直连时
使用旧静态地址：

```sh
export BOARD=hispark-remote                 # 受管网络动态 IPv6
# export BOARD=root@192.168.1.168           # 电脑网线直连回退
```

网络模式、地址稳定性与 Clash TUN 注意事项见
[`docs/network-access.md`](../docs/network-access.md)。

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
scp build/test_display "${BOARD}:/root/socchina-2026/"
ssh "${BOARD}" '/root/socchina-2026/test_display 10'   # 10 秒, 彩条/灰阶渐变交替
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

### capture — 相机采集驱动（数据通路第 1–4 级）

接口见 `include/capture.h`：`capture_query_in_size` / `capture_init`（配置结构体：传感器位 +
线性/WDR 模式）/ `capture_set_fps` / `capture_set_mirror_flip` / `capture_bind_vpss` /
`capture_unbind_vpss` / `capture_deinit`。

- 传感器 **OS08A20 8M(3840x2160)**，SDK 已适配两种模式（均板端实测通过）：
  **linear 12bit**（1.06–30fps）与 **WDR 2to1 10bit**（5–30fps，交错 HDR，传感器 60fps
  读出→30fps 融合）。WDR 文档限制：短曝光变化受 vblanking 约束，切帧率收敛变慢。
  板上接在 **sensor1/J4**：I2C bus7、时钟/复位源 1、VI dev2，即 `sensor_index=1`（默认）。
- 起链序：sensor 时钟（`bspmm 0x11018460 0x4001`，不配会导致 VPSS 取帧超时）→
  VI/VPSS 模式 `OT_VI_ONLINE_VPSS_OFFLINE` → `ss_mpi_isp_exit(0)` 清残留（防
  `already inited`/`0xa01c800c`）→ 厂商 `sample_comm_vi_start_vi`（内含 MIPI 配置、
  sensor 驱动注册、ISP 3A 注册与 ISP run 线程）。
- **VI online 模式下 CPU 取帧必须经 VPSS**；capture 只管 VI/ISP 起停与绑定。
- 构建依赖厂商 sample_comm 层：CMake `ENABLE_SDK` 分支将 `${SS928_SDK_ROOT}/src/common`
  的 sys/isp/vi/vpss 四个源文件编为 `vendor_comm` 静态库（宏与传感器型号沿用已验证
  配方），并链接 ISP/3A/sensor 静态库组。

### isp — ISP 运行时参数面与统计（数据通路第 2–4 级 + 第 11 级控制接口）

接口见 `include/isp.h`：抗闪烁（50/60Hz）、AE 曝光补偿、AE 策略（高光/暗部优先）、
手动曝光（时间+模拟增益）、WDR 长短曝光比、**`isp_get_luma_stats`**（AE 1024-bin
直方图 → `luma_stats_t`，直接喂 `control_decide`，即架构 §6 点 4 的统计通路）、
dehaze / DRC / LDCI 增强块（关/自动/手动三态）。

- ISP 本体（3A 注册与 run 线程）由 capture 起链拉起；本模块只做运行时控制，
  要求 `capture_init` 已成功。
- **手动曝光坑**：四个增益分量必须全 MANUAL（数字增益固定 1x）——若 d_gain/isp_d_gain
  留 AUTO，AE 会用数字增益把亮度补回目标值，手动曝光形同虚设（板端实测踩坑）。
- 曝光/全局色调首选 Gamma，动态范围走 DRC，颜色相关变换才使用 CLUT。CLUT 为 17³ 逻辑节点，
  SS928 `17v2` 存储按三轴奇偶拆为 8 个 bank，再以 4 路交织写入 5508 个 u32；
  位域为 R 高/G 中/B 低。HNR/锐化/AWB 等纯 ISP 项留待专项调优。

#### CoTF CLUT 实时演示 `test_cotf_live`（2026-06-16，板端联机点亮）

CoTF 路线硬件施加端的端侧实时演示：相机 → ISP(+CLUT 3D-LUT 全分辨率施加) → VPSS chn0 → VO/HDMI，
**触摸屏点击切换 CLUT 开/关**（OFF=原始相机图，ON=CoTF 校正）。复用现成 `capture`/`isp`/`display` 模块，
`isp_load_clut_lut`/`isp_set_clut` 在**运行中的** ISP pipe0 即时生效（架构控制面设计：ISP 参数独立写入、与采集/显示解耦）。

```sh
scp build/test_cotf_live "${BOARD}:/root/socchina-2026/"
# 实时演示：点屏 toggle（gamma 0.45 暗部提亮，几何无关绕过法）
ssh "${BOARD}" '/root/socchina-2026/test_cotf_live 1800 --tone 0.45'
# 取证：每 3s 自动翻转并落盘 on/off 帧（NV21，1024x600）
ssh "${BOARD}" '/root/socchina-2026/test_cotf_live 9 --auto 3 --tone 0.5 --dumpdir /root/socchina-2026/dumps'
# 诊断：回读硬件默认 CLUT 表
ssh "${BOARD}" '/root/socchina-2026/test_cotf_live 4 --probe'
```

选项：`--lut <bin>`（主机打包 LUT）、`--tone <gamma>`（几何无关提亮，γ<1 越亮）、
`--probe`（回读默认表）、`--lockexp <us> <again>`（锁手动曝光排除 AE）、`--auto <秒>`、`--dumpdir <目录>`、
`--touch <dev>`（默认 `/dev/input/event0`）、`--sensor <0|1>`。

板端实测（2026-06-16，接 OS08A20 + Waveshare 7" 触摸屏）：

- **相机→ISP+CLUT→HDMI 实时整链 30.2fps 稳定**，CLUT 开关全程不掉帧、零 NPU；触摸点屏 toggle 连续 15+ 次稳定生效。
- **AE 在 CLUT 之前测光**：开关 CLUT 时 AE 直方图 luma 不变（恒 ~53），故 CLUT 提亮不被 AE 抵消，toggle 一眼可辨。
- 历史排查中，主机曾把 `17×18×18` 猜测为边界填充网格，导致 LUT（含恒等表）
  直接灌 `isp_load_clut_lut` → **出彩色乱码 / 不透传**。
  当前演示用**几何无关绕过法**：`ss_mpi_isp_get_clut_coeff` 回读硬件默认表（格式/几何天然正确），在每个节点
  **输出值**上叠 gamma 提亮 → 干净可见的全局曝光校正。后续结合 ReleaseDoc、PQTools `17v2`
  和板端 36 组 sweep，已确认 17³→8 bank→4 路交织、RGB 轴序和位序；C bridge 与 identity
  门禁均已完成。曝光生产路径仍优先原生 Gamma/DRC。详见
  [`../models/cotf-route-verification.md`](../models/cotf-route-verification.md)「板端联机点亮（2026-06-16）」。

#### param-net RGB CLUT 实验预览 `test_paramnet_live`（2026-06-20）

正式 LCDP param-net 读取 VPSS chn2 `256x144 NV21`，经 AIPP/ACL 输出 17³ RGB LUT，再由
`lut_bridge.c` 做有限值/范围检查、identity 强度混合和 17v2 打包后热刷 ISP CLUT。

- 20 秒板端冒烟：604 帧、约 30.2fps、20/20 更新成功、0 失败；
- `--dumpdir` 可一次性抓取 ON/OFF/ON NV21、原始模型 LUT 和 packed CLUT；
- 强光取证发现 CLUT 前已有明显裁剪，RGB 路径还存在中间调压暗和 post-CLUT chn2 反馈；
- 因此 RGB 预览目前仅作实验入口，长期展示继续使用稳定 Gamma 路径；程序退出会关闭 CLUT/HDMI。

### vpss — 多路硬件缩放分发（数据通路第 5 级）

接口见 `include/vpss.h`：`vpss_init` / `vpss_get_frame` / `vpss_release_frame` / `vpss_deinit`。
按 Config-R 约定 chn0=1024x600 显示 / chn1=1024x576 串流（与整图模型备选互斥）/
chn2=256x144 CoTF 控制缩略图，NV21 无压缩输出；`depth>0` 的通道可 CPU 取帧，帧结构与
`display_send_frame` 直接兼容。取帧方必须在同一线程向同一 grp/chn 恰好归还一次。
当前实现一次支持一个组（管线仅用 grp0）。

### stream — VENC H.264 + RTSP 远程串流（数据通路第 10b 级）

接口见 `include/stream.h`：`stream_init` / `stream_send_frame` / `stream_deinit`。生产程序加
`--stream` 后启用 VPSS chn1 `1024x576 NV21`，独立 stream worker 执行
`get → VENC send → release`，编码码流由模块内 drain 线程持续取出并送给 RTSP server。

RTSP 选型：

- 原厂 MPP sample 提供 VENC H.264/H.265 编码和落文件逻辑，但没有 RTSP server。
- 首版采用仓库内轻量单客户端 server，RTP/RTSP 均走 TCP interleaved；不引入 live555/GStreamer
  等第三方运行依赖，IPv4/IPv6 双栈监听。
- RTP 支持 H.264 单 NAL 和 FU-A 分片；客户端 `PLAY` 时请求即时 IDR。慢客户端或网络写阻塞会被
  主动断开，不能反压 VENC、display 或 control。
- 无客户端时编码流仍被持续 drain，避免 VENC 队列堆积；客户端断开后可重新连接。

生产运行：

```sh
BOARD=hispark-remote scripts/run_board.sh socchina_app --stream
# 不启用 VO/HDMI，仅做远程串流：
BOARD=hispark-remote scripts/run_board.sh socchina_app --stream --no-display
# 可选：--bitrate 3000 --rtsp-port 8554 --stream-path live

# IPv6 URL 的地址必须使用方括号；强制 TCP 与服务端实现一致
ffplay -rtsp_transport tcp 'rtsp://[<BOARD_IPV6>]:8554/live'
vlc 'rtsp://[<BOARD_IPV6>]:8554/live'
```

## 2026-06-20 RTSP 纯串流板端验收

目标：

- 不开启 VO/HDMI，验证 1024x576@30 H.264 RTSP、无客户端 drain、断开重连和退出清理。

命令/路径：

```sh
/root/socchina-2026/socchina_app --stream --no-display
gst-launch-1.0 -q rtspsrc \
  location='rtsp://[<BOARD_IPV6>]:8554/live' protocols=tcp latency=100 \
  ! rtph264depay ! fakesink sync=false
```

结果：

- `scripts/test_host.sh`、ASan/UBSan RTSP 测试和 SDK Release 交叉编译通过。
- GStreamer TCP depay 连续运行 10 秒后由测试超时主动结束，重复连接第二次同样通过；媒体主链无需重启。
- 独立 10 秒 RTP 统计：301 个 access unit（约 30.1fps），H.264 payload 约
  2954.5kbps；NAL 包含 SPS/PPS/IDR/SEI/P slice，证明客户端拿到的是完整编码流。
- 无客户端的启动阶段仍持续编码/drain；42 秒运行 VENC sequence=1254、send=1255、错误 0、
  `left_bytes/left_frm/cur_packs=0`，应用日志 stream drops=0（另一次启动首帧曾有 1 次 VPSS timeout）。
- 整个应用进程（含 VI/ISP/VPSS/VENC/RTSP/control）当时约 9.6% CPU、RSS/HWM 38,252kB、
  8 threads；不是 RTSP 模块的独立增量开销。
- SIGTERM 后 8554 监听消失，VENC 通道清空，VPSS/capture 逆序退出。全程
  `/proc/umap/hdmi0` 为 `hdmi enable: NO`。

解读：

- VENC→RTSP 最小闭环、断线重连、无客户端不堵塞和资源释放已通过纯串流门禁。
- 按现场明确要求未开启 HDMI，因此 Issue #5 中“串流故障期间 HDMI ≥29.5fps”未执行，
  不能据本次数据声称显示与串流并行验收通过。
- 当前限制：单客户端、仅 RTP over RTSP/TCP，不提供鉴权、UDP transport 或音频。

### Config-R 接口契约（阶段 C）

`include/pipeline.h`、`infer.h`、`lut_bridge.h`、`stream.h` 已固定通道、同步调用、帧所有权和输出布局。
完整线程模型、LUT 刷新事务、错误降级与并行开发验收见
[`docs/data-path-interface-design.md`](../docs/data-path-interface-design.md)。

板端冒烟测试 `test_capture`（相机 → VI → ISP → VPSS chn0 → 可选 display 上屏；
完整选项见文件头注释：`--wdr/--fps/--mirror/--flip/--flicker/--aecomp/--dehaze/--drc/--ldci`）：

```sh
scp build/test_capture "${BOARD}:/root/socchina-2026/"
ssh "${BOARD}" '/root/socchina-2026/test_capture 6 --dump /root/socchina-2026/cap.nv21'
ssh "${BOARD}" '/root/socchina-2026/test_capture 15 --display'           # 实时相机画面上屏
ssh "${BOARD}" '/root/socchina-2026/test_capture 10 --display --wdr'     # WDR 2to1 模式
# 运行中每 2s 打印 avg_fps 与 AE luma 统计(mean/clip%/judged mode)
```

capture/vpss 板端实测（2026-06-10，板卡刚重启、接 OS08A20）：

- 不带显示 6s：176 帧、**29.3 fps**（首帧 AE 收敛 get_max 188ms），落盘帧经 ffmpeg 转 PNG
  确认为正常曝光的真实场景（传感器/ISP/AWB/VPSS 缩放全部正确）。
- 带显示 15s：453 帧、**30.2 fps** 稳定（get_max 35ms），相机 → VPSS → display 整链
  （阶段 A 最小直通链）跑通；退出后 HDMI `CLOSE`、无残留进程。

capture 扩展功能与 isp 模块板端实测（2026-06-11，同场景=暗房间+亮台灯）：

- **AE 统计→控制闭环**（§6 点 4 ✅）：`isp_get_luma_stats` 低频读取正常，linear 模式下
  mean=53.5、低裁剪 36.5%、高裁剪 5.7%，`control_decide`=双向校正，与场景相符。
- **运行时控制项**：`--fps 15` 实测 15.3fps 生效；`--aecomp 40`（默认 0x38=56 调低）使
  mean luma 53→28，AE 补偿旋钮方向正确；mirror/flip、抗闪烁 50Hz、dehaze/DRC/LDCI
  自动模式均无错误。
- **WDR 2to1**（§6 点 6 初步 ✅）：`10bit vc-wdr init success`，**30.3 fps 满帧稳定**；
  同场景统计 low 36.5%→5.3%、high 5.7%→1.6%（暗部与高光同时找回）；落盘帧对比：
  linear 下灯管为过曝白斑，WDR 下灯管轮廓清晰可辨。已知限制（切帧率收敛变慢）与
  WDR 画质细调待后续针对性测试。
- 注意：`--dump` 在开跑 2s（AE 收敛）后落盘；首帧曝光不可作画质对比。

曝光类功能板端实测（2026-06-11，同场景=暗房间+亮台灯，基线 linear mean≈54/low 34%/high 5.6%）：

- **手动曝光**（`--exptime --again`）：2ms@1x → mean 4.9（近全黑，判决"提亮"）；
  33ms@8x → mean 135、高光裁剪 30.7%（判决"压高光"）——两方向确定性生效，
  顺带覆盖 `control_decide` 全部分支。
- **WDR 长短曝光比**（`--wdr --wdrratio`，×64）：auto → mean 40.3/low 3.2%/high 1.4%；
  4x(256) → mean 75.7/high 3.7%（更亮但高光开始裁剪）；32x(2048) → mean 28.8/high 0.8%
  （高光保护最强、暗部变暗）。旋钮方向正确，auto 固件值居中可用。
- **WDR 运行时切帧率**：切 10fps 生效（瞬时 ~10.5fps），全程 luma 稳定无振荡；
  切 5fps 收敛偏慢（8s 末瞬时 ~7fps），与《Sensor support list》"切换变慢"警示一致，
  无画质副作用。
- **AE 策略**（`--hlprior`）：接口生效无错误，但当前场景统计无可测差异
  （高光占比未触发策略权衡），待强逆光场景再验证效果。

## 测试

- **主机单元测试**（SDK-free 纯逻辑，如 control）：`scripts/test_host.sh`（本机 cc 直接编译运行）。
- **板端测试**（触硬件）：交叉编译出的 `test_<名字>` 部署到板上手动跑。

详见 `../docs/development-guide.md`。
