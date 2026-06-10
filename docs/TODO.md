# TODO — 未完成部分与开发规划

> 状态快照日期：2026-06-10。`feat/display-hdmi` 分支（display 模块 + 黑场预帧修复 + 实测记录）已合入 `main`。
> 数据通路与模块定义见 [architecture.md](architecture.md)；规范见 [development-guide.md](development-guide.md)。

## 1. 当前已完成（基线）

- 仓库骨架、`.gitignore`、`.clang-format`、文档体系（开发规范 / 架构 / 板端操作 / AGENTS）。
- 统一构建闭环：CMake + `aarch64-mix210-linux` toolchain file，`scripts/build_board.sh`（SDK-free 与 `ENABLE_SDK` 两态）、`scripts/test_host.sh` 主机单测。
- `control` 模块：场景曝光模式判决（纯逻辑初版 + 主机单测）。
- `display` 模块：VO→HDMI 1024x600 DVI 驱动（数据通路第 10a 级），板端冒烟 `test_display` 三轮 PASS（2026-06-10），无 MPI 错误、退出干净。启动序含**黑场预帧**（先稳定黑场再使能 PHY），消除了实测发现的面板锁定瞬间杂线；实现细节与实测数据见 `board/README.md`。
- `capture` + `vpss` 模块：OS08A20(8M30 线性, sensor1/J4) → VI(online) → ISP → VPSS 多路缩放（数据通路第 1–5 级），封装厂商 sample_comm 层（CMake `vendor_comm` 静态库）。板端冒烟 `test_capture`（2026-06-10）：取帧 29.3 fps、落盘帧画面正常；`--display` 整链相机→VPSS→display 上屏 **30.2 fps 稳定**（阶段 A 最小直通链已在测试驱动中跑通）。

## 2. 未完成部分

### 2.1 板端模块（架构 §5 的模块映射，按数据通路级别）

| 通路级 | 模块文件 | 状态 | 说明 |
| --- | --- | --- | --- |
| 1 采集 | `capture.c` | ✅ 已完成 | 线性模式板端 30fps 实测；WDR/DOL 模式受 §7 限制需实测 |
| 2–4 ISP | `isp.c` | 🟡 部分 | 3A 已随 capture 起链运行；dehaze/DRC/3D-LUT 增强参数配置待做 |
| 5 分发缩放 | `vpss.c` | ✅ 已完成 | chn0 1024x600 已实测；chn1/chn2 配置项已留好待启用 |
| 6 预处理 | （融入 OM 的 AIPP） | ❌ 未开始 | 属模型侧工作，板端只需对齐 chn1 输出格式 |
| 7 推理 | `infer.c` | ❌ 未开始 | ACL 初始化、OM 加载、零拷贝输入输出 |
| 8 后处理 | `postproc.c` | ❌ 未开始 | FP16→NV21/ARGB1555；Q6 DSP 还是 IVE 待比选（§6 点 3） |
| 9 合成 | `compose.c` | ❌ 未开始 | VGS 原图/增强图分屏或叠加 |
| 10a 显示 | `display.c` | ✅ 已完成 | 板端冒烟 PASS，启动杂线已修（黑场预帧）；flicker 观感需现场目视确认 |
| 10b 串流 | `stream.c` | ❌ 未开始 | VENC H.264 + RTSP 推流（RTSP server 选型待定） |
| 11 控制 | `control.c` | 🟡 初版 | 判决逻辑有；缺迟滞防抖、ISP 统计读取（`ss_mpi_isp_get_ae_stats`）、参数写回 |
| — 共享 | `pipeline.h` | ❌ 未开始 | 跨模块帧结构 / 枚举 / VB 约定 |
| — 入口 | `main.c` | 🟡 骨架 | 仅演示构建闭环；缺整链编排、SYS/VB 初始化、优雅退出 |

### 2.2 模型侧（`models/`）

- ❌ 曲线预测网络实现：`AvgPool /4 → backbone @144x256 → 曲线参数 → ConvTranspose 上采样 → 全分辨率施加 x+r(x²-x)`（架构 §2 第 7 级）。当前仅有环境与约定文档，无任何代码/权重。
- ❌ 训练（或基于 Zero-DCE 系迁移）与导出 ONNX（NCHW、单 opset、单输出、无 Resize 算子）。
- ❌ AIPP 配置（NV21/YUV420SP_U8、rbuv_swap、/255 归一）+ ATC 转 FP16 OM（`soc_version=OPTG`、静态 1x3x576x1024）。
- ❌ 上板算子探测：确认无 AICPU/Cast 残留、全部落 AICore。
- ❌ 两套配置产出：Config-R（实时 1024x576）与 Config-Q（高画质 1024x640 级）。

### 2.3 脚本与工程

- ❌ `scripts/deploy_board.sh` / `scripts/run_board.sh`（README 已标注"待实现"）。
- ❌ 板端测试自动化：目前 `test_display` 靠手工 scp + ssh，部署脚本应统一接管。
- ❌ `LICENSE` 内容待定（当前占位）。
- ✅ `feat/display-hdmi` 已合入 `main` 并推送（2026-06-10）。

### 2.4 架构 §6 待验证点（决定方案成立与否的实测）

1. ❌ OM 结构验证：`/4 backbone + ConvTranspose + 全分辨率施加` 全落 AICore 的实测总耗时——**决定 Config-R（30fps）是否成立，最高优先级风险项**。
2. ❌ AIPP 在 1024x576 全分辨率的 CSC/归一开销。
3. ❌ 后处理 Q6 DSP vs IVE 比选。
4. ❌ `ss_mpi_isp_get_ae_stats` 低频读取 AE/luma 统计验证。
5. 🟡 VGS+VO 替代 GFBG 是否无 flicker：链路侧冒烟 PASS，**面板观感需现场目视确认**。
6. ❌ OS08A20 WDR/DOL 实测（§7 已知限制）；若不达预期，过曝防线退化为 AE 控制 + DRC/CLUT。

## 3. 开发规划

原则：**先打通最小闭环、再补画质与控制**；每个模块按"实现 → 板端冒烟 → README 记录"推进（开发规范 §10）。

### 阶段 A — 采集到显示的最小直通链（不含 NN）

目标：`OS08A20 → VI → ISP(默认参数) → VPSS chn0 → display` 实时上屏。

1. ❌ `pipeline.h` 定帧结构与 VB 约定；`main.c` 接管 SYS/VB 初始化与整链编排
   （当前最小直通链在 `test_capture` 测试驱动中，需迁入主程序）。
2. ✅ `capture.c`：线性模式点亮 OS08A20，板端 30fps（WDR 留待 §6 点 6 验证）。
3. ✅ `vpss.c`：chn0 直送 display 已实测；chn1/chn2 待整链启用。
4. ✅ 冒烟：实时相机画面上屏 30.2 fps（`test_capture --display`）；§6 点 5 flicker 观感待目视。
5. ❌ 收尾：补 `deploy_board.sh` / `run_board.sh`。

### 阶段 B — 模型最小可用 + 推理上板（与 A 可并行）

目标：拿到一个能跑的 FP16 OM 并测出真实耗时,尽早回答 §6 点 1。

1. 实现曲线预测网络（可先用随机/预训练近似权重,结构正确优先）。
2. 导出 ONNX → AIPP+ATC → OM,算子探测确认全落 AICore。
3. 板端独立 benchmark（文件输入即可,不接相机）实测 1024x576 耗时——**若超预算,立即降分辨率或砍 backbone,再继续训练投入**。
4. 同时测 §6 点 2（AIPP 开销）。

### 阶段 C — 全链路打通（A+B 汇合）

目标：相机 → 增强 → 分屏显示的实时演示雏形。

1. `infer.c`：chn1 NV21 零拷贝接 ACL。
2. `postproc.c`：先选实现成本低的一条路（IVE 优先,Q6 DSP 留作优化）,完成 §6 点 3 比选。
3. `compose.c`：VGS 原图/增强分屏。
4. 整链帧率/时延测量,对照 Config-R ≤33ms 预算分解各级耗时。

### 阶段 D — 控制大脑与画质

1. `control.c` 接真实 ISP 统计（§6 点 4）+ 迟滞防抖 + 参数写回（dehaze/DRC/CLUT/OM 曝光目标）。
2. 模型正式训练与画质验证（≥20–50 张同分辨率样本,含代表性相机帧——沿用工作区既有画质检查规则）。
3. OS08A20 WDR 实测（§6 点 6）,确定过曝防线最终形态。
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
| M3（阶段 C） | 原图/增强分屏实时演示,整链耗时分解完成 |
| M4（阶段 D） | 场景自适应生效,画质验证通过 |
| M5（阶段 E） | RTSP 远程可看,拍照路径可用,可交付演示 |

## 4. 风险与依赖提示

- **最大技术风险**是 §6 点 1（OM 全分辨率施加的实测耗时）,故规划将其前置到阶段 B 而非等训练完成——结构验证与训练解耦。
- 板端媒体状态易残留：所有新模块冒烟前确认无其他媒体进程,失败后优先干净重启（见 [board-operations.md](board-operations.md)）。
- WDR 限制（§7）可能改变第 1–2 级通路形态,阶段 A 先用线性模式规避阻塞。
