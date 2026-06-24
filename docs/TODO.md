# TODO — 未完成部分与开发规划

> 状态快照日期：2026-06-21。阶段 A/B 已完成，硬件施加端与规则控制→Gamma 生产整链已板端
> 30fps 跑通；CLUT 17v2 几何已由厂商资料和板端 sweep 确认（17³、8 bank、4 路交织）。param-net 的训练、
> 推荐 YAML、ETA/断点恢复、LCDP 正式权重和 256x144+AIPP 板端推理已完成。当前主线剩余：
> NN→RGB CLUT 安全桥与生产 control worker 接线已完成；Gamma 由 AE 规则控制，DRC/LDCI
> 由 ISP 策略管理。该契约不再规划 NN 多头 Gamma/DRC 输出。20 秒板端生产冒烟：
> 3 次 NN+CLUT 更新、推理总耗时 2.40–2.71ms、RTSP 451 帧/0 drops、退出清理正常。剩长时 p95
> 和动态闭环画质验证。RTSP 的
> VENC/协议实现、生产线程接线、RTSP+HDMI 并行验收和 systemd 开机自启动均已完成：
> 1024x576 H.264 约 30.1fps/3Mbps，HDMI 并行显示约 30.1–30.5fps、stream drops=0。
> 数据通路与模块定义见 [architecture.md](architecture.md)；规范见 [development-guide.md](development-guide.md)。
> 阶段 C 的接口与协作契约见 [data-path-interface-design.md](data-path-interface-design.md)。

## 1. 当前已完成（基线）

- 仓库骨架、`.gitignore`、`.clang-format`、文档体系（开发规范 / 架构 / 板端操作 / AGENTS）。
- 统一构建闭环：CMake + `aarch64-mix210-linux` toolchain file，`scripts/build_board.sh`（SDK-free 与 `ENABLE_SDK` 两态）、`scripts/test_host.sh` 主机单测。
- `control` 模块：场景曝光模式判决（纯逻辑初版 + 主机单测）。
- `display` 模块：VO→HDMI 1024x600 DVI 驱动（数据通路第 10a 级），板端冒烟 `test_display` 三轮 PASS（2026-06-10），无 MPI 错误、退出干净。启动序含**黑场预帧**（先稳定黑场再使能 PHY），消除了实测发现的面板锁定瞬间杂线；实现细节与实测数据见 `board/README.md`。
- `capture` + `vpss` 模块：OS08A20(8M30 线性, sensor1/J4) → VI(online) → ISP → VPSS 多路缩放（数据通路第 1–5 级），封装厂商 sample_comm 层（CMake `vendor_comm` 静态库）。板端冒烟 `test_capture`（2026-06-10）：取帧 29.3 fps、落盘帧画面正常；`--display` 整链相机→VPSS→display 上屏 **30.2 fps 稳定**（阶段 A 最小直通链已在测试驱动中跑通）。
- capture 扩展 + `isp` 模块（2026-06-11，SDK 已支持功能全量封装）：WDR 2to1 模式（30fps 满帧，同场景高光/暗部裁剪大幅下降，初测正面）、运行时帧率/镜像/翻转、抗闪烁、AE 补偿/手动曝光、**AE 统计→`control_decide` 闭环**、dehaze/DRC/LDCI 三态控制；**CLUT 代码就绪**（`isp_set_clut`/`isp_load_clut_lut`，CoTF 路线，编译链接验证）。
- **阶段 B 模型去风险（2026-06-15，`models/`）**：曲线网络（参数化、仅绿名单算子）实现 + ONNX/FP16 OM 导出 + 算子探测（干净落 AICore）+ **板端实测耗时**；横评本网络/Zero-DCE Lite/SCI/MSEC/CoTF 五架构速率矩阵；AIPP 开销实测 ≈0ms；**CoTF 路线验证 + 代码就绪**（NN 出 LUT + ISP 硬件施加，唯一突破访存地板）。详见 `models/expo-curve-network.md`、`models/cotf-route-verification.md`。**M2 里程碑达成**（OM 板端出数，Config-R 可行性有结论）。

## 2. 未完成部分

### 2.1 板端模块（架构 §5 的模块映射，按数据通路级别）

| 通路级 | 模块文件 | 状态 | 说明 |
| --- | --- | --- | --- |
| 1 采集 | `capture.c` | ✅ 已完成 | linear 与 WDR 2to1 均板端 30fps 实测；运行时帧率/镜像/翻转可调 |
| 2–4 ISP | `isp.c` | ✅ 基本完成 | Gamma/CLUT 热刷新已板端联机；CLUT 17v2 identity 已通过。余：DRC/LDCI 局部块画质标定 |
| 5 分发缩放 | `vpss.c` | ✅ 已完成 | chn0 1024x600 已实测；chn2 256x144 已在正式 param-net 推理冒烟中启用；chn1 留给 RTSP |
| 6 预处理 | （融入 OM 的 AIPP） | ✅ 板端验证 | `aipp_nv21_256x144.cfg` 已随正式 OM 上板，输入 55296B NV21，30/30 成功 |
| 7 推理 | `infer.c` | ✅ 生产接线+冒烟 | LCDP best epoch 167 正式 OM；生产 control worker 20 秒冒烟 3/3 更新，推理总耗时 2.40–2.71ms；ACL context 跨线程绑定已修，运行时错误安全停链；余 p95 长测 |
| 8 参数桥 | `lut_bridge.c` | ✅ P0 安全桥 | 17v2 打包、有限值/范围、identity 偏移、高光增益、立方体端点、主轴单调性和逐次最大步长护栏已接生产线程；高裁剪场景旁路 CLUT。余动态画质标定 |
| 10a 显示 | `display.c` | ✅ 已完成 | 板端冒烟 PASS，启动杂线已修（黑场预帧）；flicker 观感需现场目视确认 |
| 10b 串流 | `stream.c` | ✅ 已完成 | chn1→VENC H.264 CBR、单客户端 RTSP/RTP over TCP、断线重连和生产 stream worker 已实现；实测 1024x576 约 30.1fps、2954.5kbps、两次 10s GStreamer 重连成功、VENC 零积压；RTSP+HDMI 并行时显示约 30.1–30.5fps、stream drops=0 |
| 11 控制 | `control.c` / `main.c` | ✅ P0 生产闭环 | AE→规则 Gamma 与 chn2→NN→bridge→CLUT 已接同一 worker；失败保旧 LUT，连续 3 次失败进入 sticky DEGRADED；post-CLUT 反馈加入 EMA、3 次确认和 10 周期冷却 |
| — 共享 | `pipeline.h` / `pipeline.c` | ✅ 契约完成 | 通道、尺寸、配置默认值/校验、状态、错误码和 `pipeline_metrics_t` 已固定；ACL/SMMU 致命退出码为 70 |
| — 入口 | `main.c` | ✅ 生产整链 | SYS/VB + capture/vpss/display/stream/control/NN 生命周期已接通；统一汇总帧数、drop/timeout、控制事务、瞬态/致命错误、降级与 p95；支持 linear/WDR 2to1 与目标帧率 |
| — Web socket | `app_control_sock.c` | ✅ 已完成 | 监听 `/tmp/socchina-app-control.sock`，解析 8 热参数，`params_dirty` 强制刷新；修复 `nn_high_clip_guard` 偏移 bug；NN 关闭清除 CLUT、重开重试（2026-06-24） |

### 2.2 模型侧（`models/`）

- ✅ 曲线预测网络实现（2026-06-14，`models/networks/expo_curve.py`，参数化 niter/filters/down/shared_curve，
  仅绿名单算子，pytest 11 过）+ 导出 ONNX（NCHW、opset13、单输出、无 Resize）+ ATC FP16 OM + 算子探测
  （干净落 AICore，无 AICPU/Cast）+ **板端实测耗时**（见 `models/expo-curve-network.md`）。
- ✅ AIPP 配置（`aipp_nv21_1024x576.cfg` 与 `aipp_nv21_256x144.cfg`，NV21/rbuv_swap//255）
  + ATC 转 FP16 OM；256x144 配置已随正式权重板端验证。
- ✅ **路线收敛**：1024x576 全分辨率输出受访存限制；ExpoCurveNet 整图路径、Config-Q、
  `postproc/compose` 和分屏旁路已舍弃。产品只保留 CoTF-inspired CLUT 路线。
- ✅ param-net 已用 LCDP 1415/100/218 对完成 200 epoch 正式训练；best epoch 167，
  val/test PSNR 为 19.7247/20.4813dB，输入 test 基线 14.0203dB。checkpoint→FP16 ONNX→
  256x144+AIPP OM→板端 30/30 推理完成，见 `models/param-net-training.md`。

### 2.3 脚本与工程

- ✅ `scripts/deploy_board.sh`（build 产物 + OM/LUT scp 到板）/ `scripts/run_board.sh`（板端运行，自动设
  `LD_LIBRARY_PATH=/opt/lib/npu`），2026-06-19。
- ✅ `socchina-stream.service` + runtime config + HDMI 控制命令已实现并完成服务启停、
  HDMI 双态和三轮重启排障（2026-06-20）。最终 unit 排序在厂商 `rc-local.service` 媒体模块
  加载之后，完整重启只启动一次、`NRestarts=0`，RTSP 自动恢复并通过 10 秒拉流。
- ✅ 设备完整性（2026-06-20）：runtime config schema v1、linear/WDR 2to1/目标帧率生产配置、
  严格参数校验、`socchina-health` 只读健康检查、ACL/SMMU 致命退出码 70 与 systemd
  `RestartPreventExitStatus` 已实现；旧版无 schema 配置保持兼容并提示迁移。
- ✅ **Web 控制台 W0–W4 全部完成并板端验收（2026-06-24）**：Go 单文件 ARM64 后端（REST +
  SSE + HLS 反向代理 + Unix socket 客户端）+ 原生深色仪表盘前端（8 参数热控制面板、3 预设、
  200ms 防抖、WebRTC/HLS/RTSP 视频三协议切换）+ socchina-admin 冷配置事务引擎（generation/ETag、
  原子提交、健康检查、自动回滚）+ 认证代理（密码登录、session cookie、CSRF、写限流）。
  API 全 13 端点、systemd 5 服务（stream / mediamtx / web / admin / authproxy）。详见
  `web/README.md` 和 `docs/web-console-architecture.md`。
- ✅ Web 构建与部署脚本（2026-06-24）：`scripts/build_web.sh`（交叉编译 Go→ARM64）、
  `scripts/install_board_web.sh`（部署二进制+静态资源+配置到板）、`scripts/validate_board_web.sh`
  （部署后自动验收）。
- ✅ 板端 **app-control Unix socket**（`board/src/app_control_sock.c`，2026-06-24）：监听
  `/tmp/socchina-app-control.sock`，解析 `tone_strength` / `nn_high_clip_guard` /
  `nn_clut_enabled` / `drc_mode` / `enhancement_enabled` / `tone_enabled` / `drc_strength` /
  `ldci_mode` 八个热参数；set 操作置 `params_dirty` 标志强制下一控制周期立即刷新 ISP。
  修复 `nn_high_clip_guard` 解析偏移量 bug（`p+20`→`p+21`，原始终解析为 0.0 导致 NN CLUT
  永远被高光保护旁路）。`main.c` 增加 `enhancement_enabled`/`tone_enabled` 门控、NN 关闭时
  清除 CLUT 为 identity、NN 重开时 10 次重试抓 VPSS chn2 帧。
- ❌ 板端测试自动化：目前 `test_display` 靠手工 scp + ssh，部署脚本应统一接管。
- ❌ `LICENSE` 内容待定（当前占位）。
- ✅ `feat/display-hdmi` 已合入 `main` 并推送（2026-06-10）。

### 2.4 架构 §6 待验证点（决定方案成立与否的实测）

1. ✅ OM 结构验证（2026-06-14，`models/expo-curve-network.md`）：曲线网络干净落 AICore（无 AICPU/Cast）。
   但 **1024x576 全分辨率超 33ms 预算**（niter=8 实测 94.9ms，即便 niter=1 仍 38.9ms）；瓶颈是全分辨率
   访存（曲线施加 + ConvTranspose + TransData），非 backbone（砍 filters 反而变慢）。实时档建议降到
   `640x360`/`768x432` + 共享曲线才可能进入预算。该结果已用于关闭整图产品路线。
2. ✅ AIPP 全分辨率 CSC/归一开销（2026-06-14）：实测 **≈0ms**（1024x576 挂/不挂 AIPP 同速，差值噪声内），
   预处理可零开销融入 OM 前端，直接吃 VPSS chn1 的 NV21。
3. ✅ CoTF CLUT/Gamma 相机链加载和热刷新：**相机→ISP+CLUT→HDMI 实时整链 30fps 联机点亮、
   触摸点屏 toggle CLUT 开/关、零 NPU、运行中即时生效**（`board/tests/test_cotf_live.c`）。
   AE 在 CLUT 之前测光，故效果不被 AE 抵消。CLUT 几何已由厂商文档确认；Gamma 原生色调路径已
   31.2fps 实测。仍开放：动态 NN 参数刷新率、p95、闭环稳定性和 flicker 现场目视。
4. ✅ `ss_mpi_isp_get_ae_stats` 低频读取验证（2026-06-11）：`isp_get_luma_stats` 归约
   1024-bin 直方图为 mean/clip%，直接喂 `control_decide`，判决与实际场景相符。
5. 🟡 VO 视频层链路冒烟 PASS，**面板 flicker 观感需现场目视确认**。
6. 🟡 OS08A20 WDR 2to1 实测（2026-06-11）初步**正面**：30fps 满帧稳定，同场景暗部裁剪
   36.5%→5.3%、高光 5.7%→1.6%，灯管轮廓 linear 过曝白斑→WDR 清晰可辨。
   长短曝光比旋钮已调通（auto/4x/32x 趋势正确，32x 高光裁剪 0.8%）；运行时切帧率
   生效且 luma 稳定，仅向低帧率收敛偏慢（与文档警示一致，无画质副作用）。
   余项：强逆光场景画质细调、运动鬼影、长时稳定性。

### 2.5 辅助外设（非图像数据通路）

- ✅ **触摸输入开箱即用**（板端实测 2026-06-14，`feat/touch-usb`）：Waveshare 7 寸面板 USB 触摸
  经内核 `usbhid → hid-generic → evdev` 已识别为标准绝对触摸屏 `/dev/input/event0`
  （`0eef:0005`，`ABS_X 0..1024 / ABS_Y 0..600` = 面板像素 1:1，带 `BTN_TOUCH`）。
  **无需自定义驱动 / 标定 / 缩放**，应用读 `event0` 即可。本板无 `/dev/hidraw`、`/dev/uinput`
  存在。细节与核对命令见 [board-operations.md](board-operations.md) §6。
  （曾评估用户态 HID→uinput 驱动，因内核已正确处理且无 hidraw，结论为不需要。）

### 2.6 CoTF 路线（已验证的全分辨率实时技术路线）

横评 5 个架构（本曲线网络 / Zero-DCE Lite / SCI / MSEC / CoTF）后确认：**凡"输出整图"的模型在 1024x576
都撞全分辨率访存的像素线性地板**（与参数量无关），唯一突破口是 **CoTF 式「NN 低分辨率出 3D-LUT（NPU）+
ISP 硬件 CLUT 全分辨率施加（零 NPU）」**。完整验证与代码见 [../models/cotf-route-verification.md](../models/cotf-route-verification.md)。

已完成（2026-06-15）：

- ✅ NN 端实测：param-net 出 LUT，板端 ~1ms（缩略图）/ ~5ms（全分辨率），干净落 AICore。
- ✅ "NPU 做 LUT 施加"排除：3D-LUT 三线性 = grid_sample，连 ONNX 都导不出 → 必须走 ISP CLUT 硬件。
- ✅ 硬件 CLUT 实证：`ss_mpi_isp_set_clut_coeff`，5508 节点（u32=3×10bit），板端 libss_isp.so 真实导出。
- ✅ 代码集成：host 桥（`models/tools/cotf_lut_pack.py` / `models/tools/cotf_make_lut.py`，10 单测）+
  板端 API（`isp_set_clut`/
  `isp_load_clut_lut`，编译链接验证）+ 控制策略（`control_should_refresh_lut`，主机单测）。
- ✅ 流水线优势：NN 与施加在不同硬件块，LUT 低频刷新，**模型移出每帧关键路径**，NPU 占用 ~3–15%。

板端联机（2026-06-16）：

- ✅ **CLUT 联机点亮**：相机链（capture_init→ISP pipe0）上 `isp_load_clut_lut(5508)` + `isp_set_clut(en)`
  在运行中 ISP 即时生效，**相机→ISP+CLUT→HDMI 30fps、触摸点屏 toggle 开/关、零 NPU、零掉帧**
  （`board/tests/test_cotf_live.c`，交叉编译复用 capture/isp/display）。AE 在 CLUT 之前测光，效果不被 AE 抵消。
- ✅ **施加端选块与布局已厘清（2026-06-20）**：ReleaseDoc 确认 CLUT 是 17×17×17 线性 RGB
  3D-LUT；PQTools `17v2` 确认 5508 项为 8 个奇偶 bank 的 4 路交织。36 组板端 sweep 确认
  RGB 轴序、R高/G中/B低位域，identity 相邻 OFF/ON MAE 1.025、UV MAE 0.116。
  但**曝光/色调的原生块仍是 Gamma（§4.11，1D 1025 节点）/ DRC（§4.6）,不是 CLUT**。
  → 施加端按用途分块：**全局色调/曝光走 Gamma**（`isp_gamma_apply_tone`，**已板端 31fps 实测、出图干净无伪色**，
  OFF luma 89.5→BRIGHTEN 149.0）；双向动态范围走 DRC；颜色相关 3D 才用 CLUT(17³)。
- ❌ **LUT 刷新率标定**：`control_should_refresh_lut` 的间隔/阈值（初版经验值）按实测场景适应延迟标定。
- 🟡 **flicker 观感**：整链稳定无 MPI 错，热刷 LUT 的 flicker 观感需现场目视终判。
- ❌ **画质补偿（ISP 局部块）**：用 LDCI/DRC/SHARPEN/Dither 补全局 LUT 的局部对比/细节损失（**不**加全分辨率细节 NN，会撞访存地板）。
- ⏸ **余量 NPU 感知→控制**：场景分类、人脸测光等属于后续增强，不影响当前功能完整性。

产品路线、已关闭整图路线和 Zero-DCE 兜底说明详见
[model-route-summary.md](model-route-summary.md)。

## 3. 开发规划

原则：**先打通最小闭环、再补画质与控制**；每个模块按"实现 → 板端冒烟 → README 记录"推进（开发规范 §10）。

### 阶段 A — 采集到显示的最小直通链（不含 NN）

目标：`OS08A20 → VI → ISP(默认参数) → VPSS chn0 → display` 实时上屏。

1. ✅ `main.c` 已接管 SYS/VB 初始化与整链编排，迁出测试驱动：**socchina_app 板端跑通**
   （显示线程 30.5fps + 控制线程，SIGINT 优雅退出，2026-06-19）。
2. ✅ `capture.c`：线性模式和 WDR 2to1 均已点亮，板端 30fps。
3. ✅ `vpss.c`：chn0 直送 display 已实测；chn2 已用于正式 param-net+AIPP 推理和 RGB CLUT
   实验预览；chn1 留给 RTSP。
4. ✅ 冒烟：实时相机画面上屏 30.5 fps（`socchina_app` / `test_capture --display`）；§6 点 5 flicker 观感待目视。
5. ✅ 收尾：`deploy_board.sh` / `run_board.sh` 已补（2026-06-19）。

### 阶段 B — 模型最小可用 + 推理上板（与 A 可并行）✅ 已完成（2026-06-15）

目标：拿到一个能跑的 FP16 OM 并测出真实耗时,尽早回答 §6 点 1。

1. ✅ 实现曲线预测网络（参数化、随机权重，结构正确优先）。
2. ✅ 导出 ONNX → ATC → OM,算子探测确认全落 AICore（AIPP 开销另测 ≈0ms）。
3. ✅ 板端独立 benchmark 实测耗时——**结论：1024x576 超预算，实时档取 768x432+共享曲线（27ms）；不投 1024x576 训练**。
4. ✅ §6 点 2（AIPP 开销）实测 ≈0ms。
5. ✅ 额外：五架构速率横评 + **CoTF 路线验证与代码就绪**（§2.6，唯一突破访存地板的全分辨率实时路线）。

### 阶段 C — 全链路打通（A+B 汇合）

目标：相机 → ISP CLUT 增强 → 全屏显示的实时演示；严格同帧原图/增强分屏已明确不在产品范围。

接口、所有权和验收条件统一按 [data-path-interface-design.md](data-path-interface-design.md) 执行。

1. ✅ 相机链硬件施加端已完成（CLUT 30fps + Gamma 31.2fps）；CLUT 17v2 identity 已通过。
2. ✅ `infer.c`：正式 LCDP best OM 已用 VPSS chn2 256x144 NV21+AIPP 板端 30/30，
   exec 平均 1.13ms；固定输入 checkpoint→ONNX→裸 OM→AIPP OM 数值对拍通过。余项归入第 3 项安全桥。
3. ✅ `lut_bridge.c`：17v2 RGB 打包、有限值/范围、identity 强度、高光、端点、单调性和
   最大变化量护栏已实现并接入生产刷新事务。
4. ✅ AE 统计 + `control_should_refresh_lut` + 色调热刷新已接入低频 control worker（`main.c` 控制线程，2026-06-19）。
5. ✅ `main.c` 已建立 display + control + 可选 stream 线程、逆序回滚与 SIGINT/SIGTERM 优雅退出。
6. 🟡 显示主链与控制线程短测已通过；10 分钟正式稳定性和 flicker 目视待补。

### 阶段 D — 控制大脑与画质

1. ✅ 已把 chn2 infer + bridge 接入生产 control worker，完成失败保旧参数、
   sticky `PIPELINE_DEGRADED`、规则 Gamma 回退和 ACL 致命错误停链。
2. 🟡 已完成 NN→CLUT、AE 规则→Gamma、ISP 策略→DRC/LDCI 的职责分流，以及
   高光/端点/单调/最大步长基础护栏；
   post-CLUT 反馈已加入 EMA/持续确认/冷却。当前场景强制 RGB CLUT 画质失败，生产高光门控必须保留。
3. 🟡 已完成单场景 21 帧 param-net + 7 组 Gamma 公平 A/B：Gamma 0.25 当前场景通过，
   强制 RGB CLUT 不通过；余 ≥5 类、20–50 张独立场景最终验收与 OS08A20 微调。
4. 完成 OS08A20 WDR 的强逆光、运动鬼影和长时稳定性验证。
5. 阈值标定（control.c 注释已标注初版经验值待标定）。

### 阶段 E — 串流与收尾

1. ✅ `stream.c`：VENC H.264 + 自带轻量 RTSP/RTP over TCP 已完成纯串流与 HDMI 并行验收；
   无客户端持续 drain、连续接收、断线重连、退出释放和 systemd 开机恢复均通过。
2. 文档收尾、LICENSE、演示脚本。

### 后续交互与 Web 控制台（代码完成，板端已验收 2026-06-24）

正式路线见 [web-console-architecture.md](web-console-architecture.md)，四个阶段全部代码完成并板端跑通。

1. ✅ **W0 视频 PoC** — MediaMTX ARM64 部署，WebRTC/HLS/RTSP 三协议输出。
2. ✅ **W1 只读控制台** — Go 后端（REST + SSE + HLS 代理），原生前端深色仪表盘。
3. ✅ **W2 冷配置事务** — socchina-admin Unix socket 服务，generation/ETag 乐观锁，
   atomic commit + 健康检查 + 自动回滚。
4. ✅ **W3 热控制** — 8 参数实时调节面板，3 预设，200ms 防抖，PATCH /api/v1/control。
5. ✅ **W4 安全交付** — 密码登录 + session cookie + CSRF token + 写操作限流，
   authproxy 全量代理到后端（UI 单一来源），systemd sandbox。
6. ⏸ **本地触摸适配** — `/dev/input/event0` 可复用同一控制 API，未实现。

**板端配套 C 代码改动**（`board/`）：
- 新增 `app_control_sock.h/c`：Unix socket 服务，解析 8 个热参数，`params_dirty`
  标志在 socket 写入后强制下一控制周期跳过反馈观察器立即刷新 ISP。
- `main.c`：`enhancement_enabled`/`tone_enabled` 门控逻辑；用户手动调节后在正常光照
  下也应用 Gamma（BYPASS→BRIGHTEN）；NN 关闭时清除 ISP CLUT 为 identity；
  NN 重开时 10 次重试抓 VPSS chn2 帧直到 LUT 生成成功。
- 修复 `nn_high_clip_guard` 解析偏移量 bug（`p+20`→`p+21`，原始终解析为 0.0）。

**关键配置**：`/etc/socchina/runtime.conf` 必须含 `ENABLE_NN_CONTROL=1`，
否则启动脚本传 `--no-nn`，VPSS chn2 不会创建，NN 推理无法运行。

明确不提供 Config-Q 抓拍、整图模型、严格同帧分屏、MJPEG 或 CPU 视频转码。

### 里程碑判据

| 里程碑 | 判据 |
| --- | --- |
| M1（阶段 A） | ✅ 相机实时画面上屏 30.5fps；部署/启动脚本已补（2026-06-19） |
| M2（阶段 B） | OM 板端实测耗时出数,Config-R 可行性有结论 |
| M3（阶段 C） | 🟡 生产闭环、降级回退与反馈抑制已完成并短测通过；余 10 分钟正式验收、动态画质和现场 flicker |
| M4（阶段 D） | 场景自适应生效,画质验证通过 |
| M5（阶段 E） | RTSP 远程可看，配置/健康检查完整，可交付全屏增强演示 |

## 4. 风险与依赖提示

- 运行中 Gamma/CLUT 热刷新已板端跑通，CLUT 几何与 identity 已厘清。当前最大风险转为**正式训练后的画质泛化、
  NN→ISP 参数安全约束与 post-ISP 闭环稳定性**。整图 OM 的性能风险已在阶段 B 定量关闭。
- **AE 统计单消费者（已知限制）**：cross-process `get_ae_stats` 与占用通路进程的 3A 冲突（0xa01c8045）⇒
  独立控制面无法读 AE 做场景自适应（改手动循环 tone）；AE 驱动的场景自适应走集成式路径（已实测 OK）。
- 排查留痕：开发期曾遇相机不出帧（VPSS 0xa0078016），实测定位为 **sensor MIPI/上电瞬态故障**
  （断电重上电+重插排线后恢复，集成式整链随即 30fps 跑通），非软件问题。判断出流看 `VI int_cnt`/`MIPI freq`。
- 板端媒体状态易残留：所有新模块冒烟前确认无其他媒体进程,失败后优先干净重启（见 [board-operations.md](board-operations.md)）。
- WDR 限制（§7）可能改变第 1–2 级通路形态,阶段 A 先用线性模式规避阻塞。
