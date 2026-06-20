# 开发规范

本文件是本仓库的开发约定主文档。新成员或协作者开始编码前应先阅读本文。
适用平台：海思 SS928 / SD3403（Hi3403V100，海鸥派）。

## 1. 目录结构约定

起步保持精简，**子目录随开发推进按需细化**，不预先过度拆分。

| 路径 | 用途 |
| --- | --- |
| `board/` | 板端应用（C/C++，交叉编译，CMake 构建） |
| `models/` | 模型训练 / 导出 / 转换，环境依赖清单 |
| `scripts/` | 环境、构建、部署、运行脚本 |
| `docs/` | 规范 / 架构 / 操作文档 |

约定：构建产物、生成物、模型权重、采集数据不入库（见根 `.gitignore`）。

模型侧按职责分层：

```text
models/ ── networks/ · exporters/ · tools/ · configs/ · tests/ · weights/
```

- `networks/` 只放网络结构和构建函数；
- `exporters/` 放 ONNX 导出入口，共用导出逻辑通过包内导入复用；
- `tools/` 放 LUT 打包等主机侧工具；
- `configs/` 放 AIPP/ATC 配置；
- Python 入口从仓库根目录以 `python -m models.<子包>.<模块>` 运行，不修改 `sys.path`。

板端采用**常规嵌入式 Linux 工程结构**（拍平，不做框架）：

```
board/ ── CMakeLists.txt · cmake/ · include/ · src/ · tests/
```

- `src/` 平铺，每个功能一个 `.c`，命名用数据通路语义（`capture.c/isp.c/vpss.c/infer.c/lut_bridge.c/display.c/stream.c/control.c/pipeline.c`），`main.c` 为主程序入口；
- `include/` 放头文件；跨模块共享的帧结构/枚举放 `pipeline.h`；
- `tests/test_*.c` 每个一个测试入口；
- 新增功能直接加文件，CMake 自动收集（glob）。起步不预建空文件。

## 2. 命名约定

- C/C++ 文件：小写下划线，按模块前缀，如 `isp_dehaze.c`、`infer_acl.c`、`lut_bridge.c`。
- 函数：模块前缀 + 动作，如 `Isp_SetDehaze()`、`Infer_Run()`。
- 宏/常量：全大写下划线。
- 分辨率/配置标识统一写法：`1024x576`、`640x320`（宽 x 高）。
- 模型文件命名：`<任务>_<分辨率>_<精度>.{onnx,om}`，如 `expo_1024x576_fp16.om`、`expo_curve_960x540_fp16.om`。

## 3. 开发环境与依赖

> 原则：**不绑定特定 conda 环境名**。下面明确列出所用软件包，便于他人自行用任意虚拟环境复现。确切版本已锁定在 `models/requirements-model.txt`（训练/导出）与 `models/requirements-atc.txt`（ATC 转换）。

> 依赖版本**以 SDK 官方文档为依据**固化，不依赖某台机器已有的 conda 环境。需两套独立虚拟环境（protobuf/numpy 等版本要求不同）。

### 3.1 主机侧 · 模型训练与导出（Python）

- 环境：`conda create -n soc-model python=3.10`（文档要求 `python>=3.8`）。
- 依赖清单：`models/requirements-model.txt`（已固化版本）。
- 依据：海思 SS928 SDK《Yolov8 模型转换与部署》§2.1 的 `requirements_yolov8.txt`——`torch==2.1.0`、`torchvision==0.16.0`、`numpy==1.26.4`、`opencv-python==4.8.1.78`、`onnx>=1.12.0`、`onnxsim>=0.4.1`。
- 复用 TF/Keras 权重（如 Zero-DCE Lite）转 ONNX 时另需 `tensorflow`+`tf2onnx`（`--opset 13`；官方转换文档未覆盖，使用时自行锁定版本）。
- 导出约定见 §6。

### 3.2 主机侧 · 模型转换（ATC / CANN）

- 环境：`conda create -n soc-atc python=3.9.2`；依赖清单：`models/requirements-atc.txt`（已固化版本）。
- 依据：海思 SS928 SDK《Yolov8 模型转换与部署》§2.2 + 《NNN/SVP_NNN 驱动和开发环境安装指南》。文档固化依赖：`protobuf==3.13.0`、`psutil==5.7.0`、`decorator==4.4.0`、`sympy==1.5.1`、`cffi==1.12.3` + numpy/scipy/pyyaml/pathlib2。
- CANN 工具包（单独安装，非 pip）：`Ascend-cann-toolkit_5.20.t6.2.b060_linux-x86_64.run`（随海思 SS928 SDK 提供）。
- `soc_version` 取 `OPTG`。
- 避免与 base 环境的高版本 Python/`PYTHONPATH` 混用（高版本会导致 TBE 模块不兼容）。

### 3.3 板端交叉工具链（C/C++）

- 工具链：`aarch64-mix210-linux`（来自 SS928/SD3403 原厂 SDK）。
- 已知主机依赖：工具链前端需要 `libisl.so.19`；若主机缺失，需自行安装 `isl=0.19` 并将其库路径加入 `LD_LIBRARY_PATH`（在 `scripts/env.sh` 中设置，不写死个人 conda 路径）。
- 板端运行库：板上 `/opt/lib/npu/libascendcl.so`（ACL/NNN 运行时）、MPP/ISP/IVE/VGS/VENC 等 SDK 动态库。
- **不要**用 Xilinx/Vitis 的 `aarch64-linux-gnu-gcc` 编译 SDK/MPP/ACL 相关代码。

### 3.4 SDK 与工具链不入库

海思 SS928 SDK、交叉工具链、CANN、模型权重、采集数据均为外部大件，**不提交到仓库**，需自行获取。仓库内仅以文档说明"获取方式、版本号、放置位置"；实际库路径通过 `scripts/env.sh` 指向本地，不写死。

## 4. 构建系统（CMake）

板端统一用 **CMake** 构建，统一入口 `scripts/build_board.sh`。

统一构建命令：

```sh
export CROSS_COMPILE_ROOT=/path/to/aarch64-mix210-linux   # 交叉工具链根目录
export ISL_LIB_DIR=/path/to/libisl                        # 工具链前端缺 libisl.so.19 时设置
# export SS928_SDK_ROOT=/path/to/ss928_sdk                # 接入硬件模块后再设
scripts/build_board.sh [Debug|Release]                    # 默认 Release
```

约定：

- 顶层 `board/CMakeLists.txt`；toolchain file 为 `board/cmake/toolchain-aarch64-mix210-linux.cmake`（指定 `aarch64-mix210-linux` 编译器）。
- 编译器/SDK 路径全部经环境变量与 CMake 变量传入（`CROSS_COMPILE_ROOT` / `SS928_SDK_ROOT`），**不写死绝对路径**。
- **未设 `SS928_SDK_ROOT` 时仍可构建最小程序**（`ENABLE_SDK=OFF`），便于新成员先验证工具链；设置后自动开启 SDK 链接。
- 构建入口封装 `source scripts/env.sh` + `cmake` 配置/编译；输出统一进 `build/`（被 git 忽略）。
- 模块增多后在 CMake 中拆分为库目标，由主程序链接。
- C++ 标准（C11 / C++17）与告警等级（`-Wall -Wextra`）在顶层 CMake 统一设定。

## 5. 编码规范（C/C++）

- 风格：仓库根 `.clang-format`（Google 基础，4 空格，列宽 110）。提交前对改动文件执行 `clang-format`。
- **返回值必检**：所有 `ss_mpi_*` / `acl*` 调用必须检查返回码，失败打印错误码（`%#x`）与上下文后走统一清理路径，禁止忽略。
- **命名前缀**（核对自海思 SS928 SDK 头文件）：本平台 MPI 函数为 **`ss_mpi_*`**（snake_case），类型/枚举为 **`ot_*`**（如 `ot_isp_dehaze_attr`、`OT_WDR_MODE_3To1_LINE`）。**不要**使用 Hi3516 系的 `HI_MPI_*` 命名。
- **资源去初始化顺序**：媒体/ACL 资源严格按"创建逆序"释放（VENC/VO/VPSS/VI/ISP/SYS、ACL device/context/stream）。每个 init 必须有配对 deinit。
- 重启鲁棒性：启动前清理可能残留的状态（如 `ss_mpi_isp_exit` 清 ISP0 旧状态），避免 `already inited` 类错误。
- 日志与计时：用统一日志宏（分级）与计时宏，禁止散落裸 `printf`；性能关键路径打印每级耗时（capture/preprocess/infer/post/display）。
- 内存：板端帧缓冲优先用 VB 池物理地址，避免 CPU 逐像素搬运；跨模块传递帧用句柄/物理地址，避免多余拷贝。
- 错误处理统一用 `goto cleanup` 模式集中释放，禁止多出口泄漏。

## 6. 模型规范

- 部署精度：**FP16 OM**（FP32 会引入 `Cast`/AICPU 导致执行异常或超时）。
- 静态输入尺寸：按目标分辨率固定生成；变更分辨率即重新转换。
- **AIPP 需 NCHW 输入**：用于 AIPP 的模型必须导出为 NCHW（`--inputs-as-nchw`），并清理为单 opset、单输出。
- **禁用 Resize/插值上采样**：NNN 不支持 Resize（实测异常）。上采样一律用 **ConvTranspose**（固定 bilinear 权重模拟双线性），训练态/部署态分别用插值/转置卷积。
- 算子探测先行：任何新模型上板前，先转 OM 并检查图中无 `AICPU`/`Cast` 残留、profiling 确认全部落 AICore。
- OM 与 ONNX 同源：记录权重来源、导出命令、opset、输入输出名与 shape；OM 文件大小不代表参数量。
- 版本化：模型按 §2 命名，权重大件不入库，仅在 `models/` 下留 `.md` 指针（来源/精度/指标）。
- Python 模块按 `models/networks`、`models/exporters`、`models/tools` 分层；跨层使用
  `models.*` 绝对导入，不依赖当前工作目录。

## 7. 板端运行规范

- 板端受管网络访问优先使用 `~/.ssh/config` 中的 `hispark-remote` 别名；其 HostName 是
  当前 DHCPv6 地址。`root@192.168.1.168` 仅用于电脑网线直连回退。
- 不在仓库脚本中写死动态 DHCP 地址；完整约定见 [network-access.md](network-access.md)。
- 运行根目录建议：`/root/socchina-2026/`（部署脚本统一使用）。
- 启动媒体/ACL 任务前，先确认无冲突进程、媒体栈状态干净。
- **不要**用 `pkill` 停 `sample_hdmi`；用其 FIFO 停止脚本，否则会残留 MPP/VB/HDMI 状态。
- 不要在相机/增强链运行时启动厂商 `sample_hdmi`（会争用 SYS/VB）。
- 出现 ACL `aclmdlExecute` 失败 + SMMU `CMD_SYNC`/`CMDQ timeout` 时，重启板卡是可靠恢复路径。
- 健康检查与恢复命令集见 [board-operations.md](board-operations.md)。

## 8. Git 规范

- 默认分支 `main`，**直接提交并推送到 `main`**（当前不强制走 PR）。提交前确保本地能构建通过。
- 提交信息格式：`<类型>: <简述>`，类型用 `feat`/`fix`/`docs`/`chore`/`refactor`/`test`/`build`。正文说明动机与影响。
- 提交粒度：一个提交聚焦一件事；构建产物、日志、模型权重、采集数据不提交。
- 多人协作下减少冲突：提交前先 `git pull --rebase`；改动尽量按模块/数据通路阶段分工，避免同文件并发修改。
- **入库**：源码、脚本、CMake、配置、文档、小型示例配置。
- **不入库**：`build/` 输出、生成物（profile/截图/日志）、模型权重/ONNX/OM、SDK/工具链、`*.yuv/*.raw/*.h264`（见根 `.gitignore`）。
- 大件如需共享，走外部存储并在文档中留指针。

## 9. 文档规范

- 稳定结论/规范/架构写入 `docs/`；模块实现细节写入模块目录 `README.md`。
- 记录统一采用：**目标 / 命令路径 / 结果 / 解读** 四段式。
- 结论若有前提或测量条件，紧贴数字写明（如"实测/估算"、分辨率、是否含预处理）。
- 结果依赖生成文件时，同时链接结论文档与产物路径。

## 10. 测试规范

每个模块都有独立测试入口，便于各 owner 自测、互不干扰。

- **一个可执行只能有一个 `main`**：功能代码不放 `main`；`main` 只在 `src/main.c` 与各 `tests/test_*.c` 中。
- `src/*.c`（除 `main.c`）编成静态库 `socchina`，主程序与测试共用；`tests/test_*.c` 各编一个可执行并注册 `ctest`（CMake 自动收集，无需手登记）。
- **两类测试**：
  - 主机单元测试（SDK-free 纯逻辑，如 `control`）：`scripts/test_host.sh` 用本机 `cc` 直接编译运行（不需板子/qemu）。
  - 板端测试/驱动（触 SDK/硬件）：交叉编译出的 `test_<名字>` 部署到板上手动运行，不进自动化。
- Python（模型组）：测试放 `models/tests/`，用 `pytest`。
