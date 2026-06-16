# TODO — 未完成部分与开发规划

> 状态快照日期：2026-06-16。阶段 A 硬件直通链和阶段 B 模型去风险已完成，Config-R 主线已转为
> CoTF 参数网络 + ISP CLUT。**CoTF 硬件施加端已板端联机点亮**：相机→ISP+CLUT→HDMI 实时整链 30fps、
> 触摸点屏 toggle CLUT 开/关、零 NPU 跑通（`board/tests/test_cotf_live.c`）；唯一仍开放的是 **CLUT mesh
> 几何标定**（主机打包/恒等 LUT 均不透传出伪色，当前用几何无关的 gamma 绕过法拿到可见校正）。
> 详见 [../models/cotf-route-verification.md](../models/cotf-route-verification.md)「板端联机点亮（2026-06-16）」。
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
| 2–4 ISP | `isp.c` | ✅ 基本完成 | 抗闪烁/AE 补偿/手动曝光/AE 统计/dehaze/DRC/LDCI 已实测；**CLUT 已板端联机点亮**（`isp_set_clut`/`isp_load_clut_lut` 在运行中 ISP pipe0 即时生效，相机→ISP+CLUT→HDMI 30fps、触摸 toggle，见 `test_cotf_live`）；**仅剩 mesh 几何标定**（任意 NN-LUT 精确落地，见 `models/cotf-route-verification.md`） |
| 5 分发缩放 | `vpss.c` | ✅ 已完成 | chn0 1024x600 已实测；接口已固定 chn1 1024x576、chn2 256x144，待整链启用 |
| 6 预处理 | （融入 OM 的 AIPP） | 🟡 模型侧已验证 | 配置和开销已验证；待 `infer.c` 对接 chn2 NV21 |
| 7 推理 | `infer.c` | 🟡 接口已固定 | `infer.h` 已定义同步单实例、输入帧不接管、调用者输出缓冲契约；待 ACL 实现 |
| 8 LUT 桥 | `lut_bridge.c` | 🟡 接口已固定 | `lut_bridge.h` 已隔离 mesh/轴序/位序；待实现并完成诊断 LUT 标定 |
| 9 后处理/合成 | `postproc.c` / `compose.c` | ⏸ 备选路线 | 仅整图 ExpoCurveNet 分屏演示需要，非 CoTF 主路径依赖 |
| 10a 显示 | `display.c` | ✅ 已完成 | 板端冒烟 PASS，启动杂线已修（黑场预帧）；flicker 观感需现场目视确认 |
| 10b 串流 | `stream.c` | 🟡 接口已固定 | 独占 chn1 的契约已定义；待 VENC H.264 + RTSP 实现（server 选型待定） |
| 11 控制 | `control.c` | 🟡 初版 | 判决逻辑和 LUT 刷新策略已有主机单测；ISP 统计读取已在测试驱动验证，缺正式主程序接线和 LUT 写回 |
| — 共享 | `pipeline.h` | ✅ 契约完成 | 通道、尺寸、状态、错误码、输入模式和指标已固定；实现仍由 main/pipeline 接管 |
| — 入口 | `main.c` | 🟡 骨架 | 仅演示构建闭环；缺整链编排、SYS/VB 初始化、优雅退出 |

### 2.2 模型侧（`models/`）

- ✅ 曲线预测网络实现（2026-06-14，`models/networks/expo_curve.py`，参数化 niter/filters/down/shared_curve，
  仅绿名单算子，pytest 11 过）+ 导出 ONNX（NCHW、opset13、单输出、无 Resize）+ ATC FP16 OM + 算子探测
  （干净落 AICore，无 AICPU/Cast）+ **板端实测耗时**（见 `models/expo-curve-network.md`）。
- ✅ AIPP 配置（`models/configs/aipp_nv21_1024x576.cfg`，NV21/rbuv_swap//255）+ ATC 转 FP16 OM；AIPP 开销实测 ≈0ms。
- 🟡 **结论**：1024x576 全分辨率超 33ms（niter8=94.9ms，瓶颈是全分辨率访存）；实时档应取 `768x432+共享曲线`
  （27ms）或 `640x360`。横评 Zero-DCE Lite/SCI/MSEC/CoTF 后，**唯一能在 1024x576 真实时的是 CoTF 路线**
  （NN 出 LUT ~5ms + ISP 硬件施加，见 `models/cotf-route-verification.md`）。
- ❌ 认真训练（结构与耗时已解耦验证完，可投训练）；Config-Q（高画质 1024x640 级）产出。

### 2.3 脚本与工程

- ❌ `scripts/deploy_board.sh` / `scripts/run_board.sh`（README 已标注"待实现"）。
- ❌ 板端测试自动化：目前 `test_display` 靠手工 scp + ssh，部署脚本应统一接管。
- ❌ `LICENSE` 内容待定（当前占位）。
- ✅ `feat/display-hdmi` 已合入 `main` 并推送（2026-06-10）。

### 2.4 架构 §6 待验证点（决定方案成立与否的实测）

1. ✅ OM 结构验证（2026-06-14，`models/expo-curve-network.md`）：曲线网络干净落 AICore（无 AICPU/Cast）。
   但 **1024x576 全分辨率超 33ms 预算**（niter=8 实测 94.9ms，即便 niter=1 仍 38.9ms）；瓶颈是全分辨率
   访存（曲线施加 + ConvTranspose + TransData），非 backbone（砍 filters 反而变慢）。实时档建议降到
   `640x360`/`768x432` + 共享曲线（`640x360 niter8 共享 = 19.5ms`）。整图路线降为备选，
   Config-R 主线改为 CoTF + ISP CLUT；1024x576 整图输出留 Config-Q。
2. ✅ AIPP 全分辨率 CSC/归一开销（2026-06-14）：实测 **≈0ms**（1024x576 挂/不挂 AIPP 同速，差值噪声内），
   预处理可零开销融入 OM 前端，直接吃 VPSS chn1 的 NV21。
3. 🟡 CoTF CLUT 相机链加载和热刷新（2026-06-16）：**相机→ISP+CLUT→HDMI 实时整链 30fps 联机点亮、
   触摸点屏 toggle CLUT 开/关、零 NPU、运行中即时生效**（`board/tests/test_cotf_live.c`）。
   AE 在 CLUT 之前测光，故 CLUT 效果不被 AE 抵消。**仍开放**：mesh 几何标定（主机打包/恒等 LUT 均不透传、
   出伪色——轴序/遍历待对照 SDK《ISP CLUT 调优说明》；当前用几何无关 gamma 绕过法做出可见校正）；
   flicker 观感现场目视终判；LUT 刷新率标定。详见 `models/cotf-route-verification.md`「板端联机点亮」。
4. ✅ `ss_mpi_isp_get_ae_stats` 低频读取验证（2026-06-11）：`isp_get_luma_stats` 归约
   1024-bin 直方图为 mean/clip%，直接喂 `control_decide`，判决与实际场景相符。
5. 🟡 VGS+VO 替代 GFBG 是否无 flicker：链路侧冒烟 PASS，**面板观感需现场目视确认**。
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
- ✅ 代码集成：host 桥（`models/tools/cotf_lut_pack.py` / `models/tools/cotf_make_lut.py`，9 单测）+
  板端 API（`isp_set_clut`/
  `isp_load_clut_lut`，编译链接验证）+ 控制策略（`control_should_refresh_lut`，主机单测）。
- ✅ 流水线优势：NN 与施加在不同硬件块，LUT 低频刷新，**模型移出每帧关键路径**，NPU 占用 ~3–15%。

板端联机（2026-06-16）：

- ✅ **CLUT 联机点亮**：相机链（capture_init→ISP pipe0）上 `isp_load_clut_lut(5508)` + `isp_set_clut(en)`
  在运行中 ISP 即时生效，**相机→ISP+CLUT→HDMI 30fps、触摸点屏 toggle 开/关、零 NPU、零掉帧**
  （`board/tests/test_cotf_live.c`，交叉编译复用 capture/isp/display）。AE 在 CLUT 之前测光，效果不被 AE 抵消。
- ❌ **CLUT mesh 标定（唯一卡点）**：`cotf_lut_pack.HW_MESH_DIMS`（默认 17×18×18）的节点坐标/遍历顺序
  与硬件实际不符——主机打包 LUT 灌进去出**彩色乱码**，恒等立方 LUT 也**不透传**（高光伪色环），等效转置。
  10bit 位打包格式经默认表回读核对**可信**，错的是 linear-index↔(r,g,b) 几何映射；需对照 SDK《ISP CLUT
  调优说明》标定——**只改常量，不动主流程**。当前演示用几何无关绕过法（读硬件默认表→输出值叠 gamma）拿到干净校正。
- ❌ **LUT 刷新率标定**：`control_should_refresh_lut` 的间隔/阈值（初版经验值）按实测场景适应延迟标定。
- 🟡 **flicker 观感**：整链稳定无 MPI 错，热刷 LUT 的 flicker 观感需现场目视终判。
- ❌ **画质补偿（ISP 局部块）**：用 LDCI/DRC/SHARPEN/Dither 补全局 LUT 的局部对比/细节损失（**不**加全分辨率细节 NN，会撞访存地板）。
- ❌ **余量 NPU 感知→控制**：主 NNN ~85%+ 闲 + SVP_NNN 整颗闲，跑场景分类/人脸测光等喂 `control.c`（复用 ModelZoo OM，非像素任务）。

技术路线分级（主推 CoTF-LUT / 备选 曲线网络 768x432 / 兜底 Zero-DCE 单向）与待测项详见
[model-route-summary.md](model-route-summary.md)。

## 3. 开发规划

原则：**先打通最小闭环、再补画质与控制**；每个模块按"实现 → 板端冒烟 → README 记录"推进（开发规范 §10）。

### 阶段 A — 采集到显示的最小直通链（不含 NN）

目标：`OS08A20 → VI → ISP(默认参数) → VPSS chn0 → display` 实时上屏。

1. 🟡 `pipeline.h` 接口契约已完成；`main.c` 待接管 SYS/VB 初始化与整链编排
   （当前最小直通链在 `test_capture` 测试驱动中，需迁入主程序）。
2. ✅ `capture.c`：线性模式和 WDR 2to1 均已点亮，板端 30fps。
3. ✅ `vpss.c`：chn0 直送 display 已实测；chn1/chn2 待整链启用。
4. ✅ 冒烟：实时相机画面上屏 30.2 fps（`test_capture --display`）；§6 点 5 flicker 观感待目视。
5. ❌ 收尾：补 `deploy_board.sh` / `run_board.sh`。

### 阶段 B — 模型最小可用 + 推理上板（与 A 可并行）✅ 已完成（2026-06-15）

目标：拿到一个能跑的 FP16 OM 并测出真实耗时,尽早回答 §6 点 1。

1. ✅ 实现曲线预测网络（参数化、随机权重，结构正确优先）。
2. ✅ 导出 ONNX → ATC → OM,算子探测确认全落 AICore（AIPP 开销另测 ≈0ms）。
3. ✅ 板端独立 benchmark 实测耗时——**结论：1024x576 超预算，实时档取 768x432+共享曲线（27ms）；不投 1024x576 训练**。
4. ✅ §6 点 2（AIPP 开销）实测 ≈0ms。
5. ✅ 额外：五架构速率横评 + **CoTF 路线验证与代码就绪**（§2.6，唯一突破访存地板的全分辨率实时路线）。

### 阶段 C — 全链路打通（A+B 汇合）

目标：相机 → ISP CLUT 增强 → 全屏显示的实时演示雏形；严格同帧原图/增强分屏不属于 M3。

接口、所有权和验收条件统一按 [data-path-interface-design.md](data-path-interface-design.md) 执行。

1. 🟡 相机链联机点亮**已完成**（`test_cotf_live`：相机→ISP+CLUT→HDMI 30fps + 触摸 toggle，2026-06-16）；
   仍需用 identity/诊断 LUT 完成 **CLUT mesh 几何标定**（当前主机打包/恒等 LUT 不透传出伪色，用 gamma 绕过）。
2. `infer.c`：VPSS chn2 256x144 NV21 接 AIPP/ACL，运行 CoTF param-net 输出 LUT。
3. `lut_bridge.c`：实现板端 LUT 重采样/打包，并通过 ISP 接口写回。
4. 把 AE 统计、`control_should_refresh_lut` 和 LUT 热刷新接入低频 control worker。
5. `main.c` 建立 display/control/可选 stream 三线程并实现逆序回滚和优雅退出。
6. 测量 30fps 主链、LUT 刷新耗时和热刷新稳定性。整图后处理/VGS 分屏作为备选演示另行评估。

### 阶段 D — 控制大脑与画质

1. 将已验证的 ISP 统计读取迁入正式主程序，完成迟滞防抖和 dehaze/DRC/CLUT 参数写回。
2. 模型正式训练与画质验证（≥20–50 张同分辨率样本,含代表性相机帧——沿用工作区既有画质检查规则）。
3. 完成 OS08A20 WDR 的强逆光、运动鬼影和长时稳定性验证。
4. 阈值标定（control.c 注释已标注初版经验值待标定）。

### 阶段 E — 串流与收尾

1. `stream.c`：VENC H.264 + RTSP（选型:先评估 SDK 自带 sample 的 RTSP 实现可否复用）。
2. Config-Q 拍照高画质路径。
3. 文档收尾、LICENSE、演示脚本。

### 里程碑判据

| 里程碑 | 判据 |
| --- | --- |
| M1（阶段 A） | 相机实时画面上屏,链路可一键部署/启动 |
| M2（阶段 B） | OM 板端实测耗时出数,Config-R 可行性有结论 |
| M3（阶段 C） | CoTF LUT 相机链联机生效（🟡 30fps 整链 + 触摸 toggle 已点亮 2026-06-16；余 mesh 标定 + 刷新耗时分解） |
| M4（阶段 D） | 场景自适应生效,画质验证通过 |
| M5（阶段 E） | RTSP 远程可看,拍照路径可用,可交付演示 |

## 4. 风险与依赖提示

- 运行中热刷新已板端跑通（30fps 不掉帧、点屏 toggle 即时生效，2026-06-16）；**当前最大技术风险收窄为 ISP CLUT
  mesh 几何标定**（轴序/遍历顺序——主机打包/恒等 LUT 均不透传出伪色，未标定前任意 NN-LUT 无法精确落地，
  当前用几何无关 gamma 绕过法演示）。整图 OM 的性能风险已在阶段 B 定量关闭。
- 板端媒体状态易残留：所有新模块冒烟前确认无其他媒体进程,失败后优先干净重启（见 [board-operations.md](board-operations.md)）。
- WDR 限制（§7）可能改变第 1–2 级通路形态,阶段 A 先用线性模式规避阻塞。
