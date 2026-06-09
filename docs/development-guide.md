# 开发规范

本文件是本仓库的开发约定主文档。新成员或协作者开始编码前应先阅读本文。
适用平台：海思 SS928 / SD3403（Hi3403V100，海鸥派）。

## 1. 目录结构约定

| 路径 | 用途 | 备注 |
| --- | --- | --- |
| `board/` | 板端应用（C/C++） | 交叉编译，CMake 构建 |
| `board/app/` | `main` 与应用编排（线程、帧循环） | |
| `board/src/<stage>/` | 数据通路各级实现 | 见下表 |
| `board/include/` | 对外头文件 | |
| `board/third_party/` | 外部 SDK 头/库的链接说明 | 大件不入库 |
| `models/` | 模型训练/导出/转换/配置/权重指针 | |
| `scripts/` | 环境、构建、部署、运行脚本 | |
| `configs/` | 运行期配置（分辨率/模式/ISP 预设） | |
| `tools/` | 主机侧工具 | |
| `tests/` | 单元/集成测试 | |
| `docs/` | 规范/架构/操作文档 | |
| `artifacts/` | 生成物 | 绝大部分被 git 忽略 |

`board/src/` 子目录与数据通路一一对应，**代码即架构**：

```
capture → isp → vpss → preprocess → infer → postprocess → compose → display / stream
                                                                控制大脑: control
                                              公共设施: common
```

新增模块时：在对应 `board/src/<stage>/` 下加源文件，并在该目录留一份简短 `README.md`（目标/接口/注意事项）。

## 2. 命名约定

- C/C++ 文件：小写下划线，按模块前缀，如 `isp_dehaze.c`、`infer_acl.c`、`compose_vgs.c`。
- 函数：模块前缀 + 动作，如 `Isp_SetDehaze()`、`Infer_Run()`。
- 宏/常量：全大写下划线。
- 分辨率/配置标识统一写法：`1024x576`、`640x320`（宽 x 高）。
- 模型文件命名：`<任务>_<分辨率>_<精度>.{onnx,om}`，如 `expo_1024x576_fp16.om`、`expo_curve_960x540_fp16.om`。

## 3. 开发环境与依赖

> 原则：**不绑定特定 conda 环境名**。下面明确列出所用软件包，便于他人自行用任意虚拟环境复现。建议在 `models/requirements.txt` 中锁定确切版本。

### 3.1 主机侧 · 模型训练与导出（Python）

核心包（建议 Python 3.9–3.10）：

| 包 | 用途 | 版本备注 |
| --- | --- | --- |
| `torch`, `torchvision` | 训练曝光校正/曲线网络 | CPU 或 CUDA 版按机器选 |
| `numpy` | 数值 | |
| `opencv-python` | 图像读写/预处理/可视化 | |
| `pillow` | 图像 IO | |
| `onnx` | ONNX 模型 | |
| `onnxsim`（onnx-simplifier） | 图简化 | |
| `onnxruntime` | 导出后精度校验 | |
| `tf2onnx` + `tensorflow` | 仅当复用 TF/Keras 权重（如 Zero-DCE Lite）时需要 | `--opset 13` |

导出约定：见 §6（FP16、静态 shape、AIPP 需 NCHW、避免 Resize）。

### 3.2 主机侧 · 模型转换（ATC / CANN）

- 工具：海思 SVP_NNN / NNN ATC（来自 SDK `01.software/pc/NNN` 与 `SVP_NNN`）。
- CANN 版本：`Ascend-cann-toolkit_5.20.t6.2.b060`（与板端运行库匹配）。
- `soc_version` 取 `OPTG`。
- 转换在独立 Python 3.9 环境运行；避免与 base 环境的高版本 Python/`PYTHONPATH` 混用（高版本会导致 TBE 模块不兼容）。

### 3.3 板端交叉工具链（C/C++）

- 工具链：`aarch64-mix210-linux`（来自 SS928/SD3403 原厂 SDK）。
- 已知主机依赖：工具链前端需要 `libisl.so.19`；若主机缺失，需自行安装 `isl=0.19` 并将其库路径加入 `LD_LIBRARY_PATH`（在 `scripts/env.sh` 中设置，不写死个人 conda 路径）。
- 板端运行库：板上 `/opt/lib/npu/libascendcl.so`（ACL/NNN 运行时）、MPP/ISP/IVE/VGS/VENC 等 SDK 动态库。
- **不要**用 Xilinx/Vitis 的 `aarch64-linux-gnu-gcc` 编译 SDK/MPP/ACL 相关代码。

### 3.4 SDK 与工具链不入库

SDK、交叉工具链、CANN、模型权重、采集数据均为外部大件，**不提交到仓库**。`board/third_party/` 仅保存"从哪获取、放到哪、版本号"的说明文档；实际库通过 `scripts/env.sh` 指向本地路径。

## 4. 构建系统（CMake）

板端统一用 **CMake** 构建（与研究区 `zero-dce-npu-om` 一致）。

约定：

- 顶层 `board/CMakeLists.txt`；各 `src/<stage>/` 以静态库或对象库形式组织，由 `app/` 链接成最终可执行文件。
- 交叉编译通过 toolchain file 指定 `aarch64-mix210-linux` 编译器；SDK 头/库路径由变量（如 `SS928_SDK_ROOT`）传入，不写死绝对路径。
- 构建入口：`scripts/build_board.sh`（封装 `cmake -DCMAKE_TOOLCHAIN_FILE=... && cmake --build`，并 source `scripts/env.sh`）。
- 构建输出统一进 `build/`（被 git 忽略）。
- C++ 标准与告警等级在顶层 CMake 统一设定；鼓励 `-Wall -Wextra`。

## 5. 编码规范（C/C++）

- 风格：仓库根 `.clang-format`（Google 基础，4 空格，列宽 110）。提交前对改动文件执行 `clang-format`。
- **返回值必检**：所有 `ss_mpi_*` / `HI_MPI_*` / `acl*` 调用必须检查返回码，失败打印错误码（`%#x`）与上下文后走统一清理路径，禁止忽略。
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
- 版本化：模型按 §2 命名，权重大件不入库，`models/weights/` 仅留 `.md` 指针（来源/精度/指标）。

## 7. 板端运行规范

- 板端 SSH 目标：`root@192.168.1.168`。
- 运行根目录建议：`/root/socchina-2026/`（部署脚本统一使用）。
- 启动媒体/ACL 任务前，先确认无冲突进程、媒体栈状态干净。
- **不要**用 `pkill` 停 `sample_hdmi`；用其 FIFO 停止脚本，否则会残留 MPP/VB/HDMI 状态。
- 不要在相机/增强链运行时启动厂商 `sample_hdmi`（会争用 SYS/VB）。
- 出现 ACL `aclmdlExecute` 失败 + SMMU `CMD_SYNC`/`CMDQ timeout` 时，重启板卡是可靠恢复路径。
- 健康检查与恢复命令集见 [board-operations.md](board-operations.md)。

## 8. Git 规范

- 默认分支 `main`；初始脚手架与基础文档可直接落 `main`，**功能开发用特性分支**（如 `feat/isp-wdr`、`feat/infer-acl`），经自测后合并。
- 提交信息格式：`<类型>: <简述>`，类型用 `feat`/`fix`/`docs`/`chore`/`refactor`/`test`/`build`。正文说明动机与影响。
- 提交粒度：一个提交聚焦一件事；构建产物、日志、模型权重、采集数据不提交。
- **入库**：源码、脚本、CMake、配置、文档、小型示例配置。
- **不入库**：`build/`、`artifacts/` 大件、模型权重/ONNX/OM、SDK/工具链、`*.yuv/*.raw/*.h264`、日志（见根 `.gitignore`）。
- 大件如需共享，走外部存储并在文档中留指针。

## 9. 文档规范

- 稳定结论/规范/架构写入 `docs/`；模块实现细节写入模块目录 `README.md`。
- 记录统一采用：**目标 / 命令路径 / 结果 / 解读** 四段式。
- 结论若有前提或测量条件，紧贴数字写明（如"实测/估算"、分辨率、是否含预处理）。
- 结果依赖生成文件时，同时链接结论文档与产物路径。
