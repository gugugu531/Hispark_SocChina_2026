# AI 代理开发指南

本文件是 AI 编码代理在本仓库开展工作前应阅读的第一个文件。它汇总了项目背景、阅读顺序、开发约定、板端操作习惯、已验证事实与已知坑点。人类协作者同样适用。

## 任务背景

本仓库是面向海思 SS928 / SD3403（Hi3403V100，海鸥派）平台的**实时双向曝光校正系统**参赛软件仓库：既拉亮暗部，也抑制高光过曝。

> SS928 / Hi3403V100 / SD3403 / 海鸥派 / EulerPi 均指同一块板卡。

核心数据通路（详见 `docs/architecture.md`）：

```text
OS08A20 -> VI -> ISP(WDR/3DNR/LTM/去雾) -> VPSS 多路缩放
  ├─ chn0 -> 显示底图
  └─ chn1 -> AIPP -> NNN/ACL OM(曲线预测+施加) -> 后处理(DSP/IVE)
                          ▼
        VGS 合成 -> VO -> HDMI 1024x600 本地显示
                  └─> VENC -> RTSP 远程串流
  场景自适应控制(读 ISP AE 统计 -> 调 ISP/模型参数)贯穿全链
```

设计要点：

- 降噪交给 ISP 硬件（3DNR/HNR），NN 只做**单输入"低分辨率预测曲线 + 全分辨率施加曲线"**。
- 除推理与 AIPP 在 NNN 上，其余环节全部由 ISP/VPSS/IVE/VGS/VENC/DSP 硬件块承担，目标**零 CPU 逐像素搬运**。
- 两套运行配置：Config-R（实时 30fps，模型输入 `1024x576`，预算 ≤33ms/帧）与 Config-Q（拍照/低帧，高画质）。

## 先读这些

1. `README.md` —— 仓库地图与快速开始。
2. `docs/architecture.md` —— 系统架构、完整数据通路、已核实 SDK 接口与待验证点。
3. `docs/development-guide.md` —— 开发规范主文档（环境/构建/编码/模型/板端/Git/文档/测试）。
4. `docs/board-operations.md` —— 板端部署/运行/恢复手册。
5. `board/README.md` —— 板端代码组织与构建。
6. `models/README.md` —— 模型环境与导出/转换约定。

本仓库文档已覆盖关键路径与接口名，不要在已索引文档足够用之前就从搜索整个 SDK 开始。

## 目录约定

| 路径 | 用途 |
| --- | --- |
| `board/` | 板端应用（C/C++，交叉编译，CMake 构建）。 |
| `models/` | 模型训练 / ONNX 导出 / ATC→OM 转换，环境依赖清单。 |
| `scripts/` | 环境、构建、部署、运行脚本。 |
| `docs/` | 规范 / 架构 / 操作文档。 |

板端代码采用拍平的常规嵌入式 Linux 结构：`board/{CMakeLists.txt, cmake/, include/, src/, tests/}`。

- `src/` 平铺，每个数据通路阶段一个 `.c`（`capture/isp/vpss/preproc/infer/postproc/compose/display/stream/control`），`main.c` 为主程序入口。
- `src/*.c`（除 `main.c`）编成静态库 `socchina`，主程序与测试共用。
- `tests/test_*.c` 每个一个测试入口，CMake glob 自动收集，无需手动登记。
- 构建产物、模型权重、采集数据、SDK/工具链一律不入库（见根 `.gitignore`）。

## 构建与测试

所有外部依赖路径**经环境变量传入，不写死任何机器上的绝对路径**：

```sh
export CROSS_COMPILE_ROOT=/path/to/aarch64-mix210-linux   # 交叉工具链根目录
export ISL_LIB_DIR=/path/to/libisl                        # 工具链前端缺 libisl.so.19 时设置
# export SS928_SDK_ROOT=/path/to/ss928_sdk                # 接入 SDK 模块后再设

scripts/build_board.sh [Debug|Release]   # 板端构建，产物 build/socchina_app (aarch64)
scripts/test_host.sh                     # 主机单元测试（SDK-free 纯逻辑，本机 cc 直接编译运行）
```

- 未设 `SS928_SDK_ROOT` 时仍可构建最小程序（`ENABLE_SDK=OFF`），用于验证工具链闭环；设置后自动开启 SDK 链接。
- **不要**用 Xilinx/Vitis 的 `aarch64-linux-gnu-gcc` 编译 SDK/MPP/ACL 相关代码；统一用厂商 `aarch64-mix210-linux` 工具链。
- 两类测试分清：主机单测跑 SDK-free 纯逻辑；触硬件的 `test_<名字>` 交叉编译后部署到板上手动运行，不进自动化。

## 编码规范要点

完整规范见 `docs/development-guide.md` §5，最容易被违反的几条：

- **命名前缀**：本平台 MPI 函数为 `ss_mpi_*`（snake_case），类型/枚举为 `ot_*`。**不要**使用 Hi3516 系的 `HI_MPI_*` 命名。
- **返回值必检**：所有 `ss_mpi_*` / `acl*` 调用必须检查返回码，失败打印错误码（`%#x`）与上下文。
- 错误处理统一 `goto cleanup`，资源严格按"创建逆序"释放，每个 init 配对 deinit。
- 启动前清理可能残留的状态（如 `ss_mpi_isp_exit` 清旧 ISP 状态），避免 `already inited` 类错误。
- 帧数据用 VB 池物理地址/句柄跨模块传递，避免 CPU 逐像素拷贝。
- 统一日志/计时宏，禁止散落裸 `printf`；性能关键路径打印每级耗时。
- 风格用仓库根 `.clang-format`（4 空格，列宽 110），提交前对改动文件执行 `clang-format`。

## 模型规范要点

完整规范见 `docs/development-guide.md` §6 与 `models/README.md`：

- 部署精度 **FP16 OM**（FP32 会引入 `Cast`/AICPU 导致执行异常或超时）；静态输入尺寸；`soc_version=OPTG`。
- 用于 AIPP 的模型必须导出为 **NCHW**（`--inputs-as-nchw`），清理为单 opset、单输出。
- **禁用 Resize/插值上采样**（NNN 实测异常），上采样一律用 **ConvTranspose**（固定 bilinear 权重）。
- 算子探测先行：任何新模型上板前，先转 OM 确认无 `AICPU`/`Cast` 残留、profiling 确认全部落 AICore。
- 模型命名 `<任务>_<分辨率>_<精度>.{onnx,om}`；分辨率统一写 `宽x高`（如 `1024x576`）。
- 训练/导出与 ATC 转换是**两套独立 Python 环境**（protobuf/numpy 版本冲突），依赖清单在 `models/requirements-model.txt` 与 `models/requirements-atc.txt`。
- OM 文件大小不代表参数量；记录每个模型的权重来源、导出命令、opset、输入输出 shape。

## 板卡访问习惯

板卡 SSH 目标：`root@192.168.1.168`；板端运行根目录：`/root/socchina-2026/`。

用判断力控制探测频率：

- 简单只读 SSH 命令不必过度探测网络/串口。
- 在有风险的媒体管线操作、模块重载、显示重启或长时间板端运行前，确认 SSH/网络与进程状态正常。
- 长时运行前先做有限帧数的短冒烟测试，确认链路与计时正常。

媒体管线纪律（违反会留下脏状态，浪费大量时间）：

- **不要**用 `pkill` 停厂商 `sample_hdmi`；用其 FIFO 停止脚本，否则残留 MPP/VB/HDMI 状态。
- 不要在相机/增强链运行时启动厂商 sample 应用（争用 SYS/VB，导致 `ss_mpi_vb_set_conf failed`）。
- ISP/VI 报 `already inited` 时，先找到并停止既有媒体进程，再清理重启。
- 出现 ACL `aclmdlExecute` 失败 + SMMU `CMD_SYNC`/`CMDQ timeout` 时，NPU/SMMU 上下文已坏，**重启板卡**是可靠恢复路径。
- 媒体栈脏（进程 D 态、无法 kill）时，干净重启比反复重试省时间。

健康检查命令集见 `docs/board-operations.md` §3（`/proc/umap/{vpss,hdmi0,gfbg0,svp_nnn}`）。

## 已验证事实与已知风险

来自先前板端实验的稳定结论（编码与决策时直接采信，不必重新求证；推翻需附新实测）：

- Zero-DCE Lite FP16 OM 可在 SVP_NNN 上运行；离线模型执行约 `29.9 ms @ 640x320`、`96 ms @ 1024x640`。这些是**离线基准**，不是已验证的实时数字。
- AIPP 路径已在小分辨率（160x160）验证可用，约 `1 ms`；全分辨率 `1024x576` 的 AIPP 开销待实测。
- 历史实时路径的瓶颈是 CPU YUV/RGB/FP16 打包与 framebuffer 合成，而非模型本身——这正是本架构用 AIPP/VGS/DSP 替代的动机。
- HDMI 面板为类 DVI 的 1024x600 面板：原生时序、DVI 模式、音频禁用、RGB full 路径稳定；GFBG 图层路径有轻微 flicker（VGS 合成替代方案待验证）。
- 相机是 **OS08A20**（早期曾被误认为 OS04A10，已纠正）。
- `/proc/umap/svp_nnn` 的 `hw_utilization` 是目前最佳的 NPU 百分比式利用率指标；ACL profiling 会扭曲 CPU/内存计时，资源监控与 profiling 要分开跑。
- **OS08A20 的 WDR 模式有 SDK 文档记载的限制**（短曝光精度/亮度受影响）。"WDR 作为过曝主防线"需先实测；不达预期则退化为 AE 曝光控制 + DRC/CLUT 色调压缩为主。

架构级待验证点清单维护在 `docs/architecture.md` §6，其中**第 1 条（新 OM 结构全落 AICore 且耗时达标）是 Config-R 成立与否的 go/no-go 门槛**，优先级最高。

## Git 规范

- 默认分支 `main`，直接提交并推送（当前不强制 PR）；提交前确保本地构建通过，先 `git pull --rebase`。
- 提交信息：`<类型>: <简述>`，类型用 `feat`/`fix`/`docs`/`chore`/`refactor`/`test`/`build`。
- 一个提交聚焦一件事；构建产物、日志、模型权重、采集数据不提交。
- 不要回退工作树中无关的既有改动；避免 `git reset --hard`、宽泛 `rm` 等破坏性操作，除非用户明确要求。

## 文档规则

以"让下一个代理无需重读整段对话即可重建实验"为标准记录开发：

- 稳定结论/规范/架构写入 `docs/`；模块实现细节写入模块目录 `README.md`。
- 记录统一四段式：**目标 / 命令路径 / 结果 / 解读**。
- 结论若有前提或测量条件，紧贴数字写明（实测/估算、分辨率、是否含预处理）。
- 结果依赖生成文件时，同时链接结论文档与产物路径。
- 完成影响项目方向的改动后，立即更新相关文档，不要攒。

## 如何干净地扩展工作

新增板端功能：

1. 往 `board/src/` 加 `.c`、`board/include/` 加 `.h`（CMake glob 自动收集）。
2. SDK-free 纯逻辑配 `board/tests/test_*.c` 主机单测；触硬件的写板端 `test_<名字>` 驱动。
3. 本地构建通过后再部署上板。

改动实时链路：

1. 确认当前板卡进程与媒体状态干净。
2. 构建并部署到 `/root/socchina-2026/` 下命名清晰的位置。
3. 先跑有限帧数冒烟测试，再长时运行。
4. 采集日志/计时，立即更新相关文档。

## 不要做的事

- 不要把 OM 文件大小当作参数量。
- 不要假设 Keras、ONNX 和 OM 的输出按位一致。
- 不要在不说明输入图像与 resize 策略的情况下对比模型画质。
- 不要混淆"离线模型执行耗时"与"端到端实时帧率"，引用数字时带上口径。
- 不要把帧周期占用当作 NPU 硬件利用率上报。
- 不要在不记录改了什么的情况下静默覆盖板端脚本。
- 不要提交构建产物、权重、采集数据，或把生成物混回源码目录。
- 不要写死任何个人机器上的绝对路径；外部依赖一律走环境变量（`scripts/env.sh`）。
