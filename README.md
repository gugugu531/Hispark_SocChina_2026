# Hispark SoC China 2026

面向海思 SS928 / SD3403（Hi3403V100，海鸥派）平台的**实时图像增强系统**参赛软件仓库。

系统在板端构建一条以 ISP 硬件为底座、NPU 曲线/3D-LUT 网络为主体的**双向曝光校正**链路：既能拉亮暗部，也能抑制高光过曝，并通过 HDMI 本地显示与 RTSP 远程串流输出。

> 说明：SS928 / Hi3403V100 / SD3403 / 海鸥派 / EulerPi 均指同一块板卡。

## 系统总览

```
OS08A20 ─► VI ─► ISP(WDR/降噪/LTM/去雾) ─► VPSS(多路缩放)
   ├─ chn0 ─► 显示底图 ─────────────────────────────┐
   └─ chn1 ─► AIPP ─► NNN/ACL OM(曝光校正) ─► 后处理 ─┤
                                                      ▼
                                  VGS 合成 ─► VO ─► HDMI 本地显示
                                            └─► VENC ─► RTSP 远程
   场景自适应大脑（读 ISP AE 统计 → 调 ISP/模型参数）贯穿全链
```

完整数据通路与设计依据见 [docs/architecture.md](docs/architecture.md)。

## 目录结构

起步保持精简，子目录随开发推进再细化。

| 路径 | 用途 |
| --- | --- |
| `board/` | 板端应用（C/C++，交叉编译，CMake 构建）。 |
| `models/` | 模型训练 / ONNX 导出 / ATC→OM 转换，环境依赖清单。 |
| `scripts/` | 环境、构建、部署、板端运行脚本。 |
| `docs/` | 开发规范、系统架构、板端操作手册。 |

## 快速开始

### 板端构建（统一入口）

无需 SDK 即可先验证工具链与构建闭环（产出 aarch64 可执行文件）：

```sh
# 各人按自己机器设置（不写死个人路径）：
export CROSS_COMPILE_ROOT=/path/to/aarch64-mix210-linux   # 交叉工具链根目录
export ISL_LIB_DIR=/path/to/libisl                        # 若提示缺 libisl.so.19 时设置
# 接入硬件后再设置：export SS928_SDK_ROOT=/path/to/ss928_sdk

scripts/build_board.sh            # 默认 Release，产物在 build/socchina_app
```

设置 `SS928_SDK_ROOT` 后自动开启 SDK 链接（`ENABLE_SDK=ON`）。环境变量说明见 `scripts/env.sh`。

### 主机单元测试

SDK-free 的纯逻辑模块可在本机直接跑（无需板子/工具链）：

```sh
scripts/test_host.sh              # 原生编译 + ctest
```

板端代码组织与测试规范见 [board/README.md](board/README.md) 与 [docs/development-guide.md](docs/development-guide.md) §10。

### 其它

- 环境与依赖详解：[docs/development-guide.md](docs/development-guide.md) §3–§4。
- 模型训练/导出/转换：[models/README.md](models/README.md)（两套独立环境，依据 SDK 文档固化）。
- 部署/运行脚本（`deploy_board.sh` / `run_board.sh`）：待实现。

## 文档导航

- [docs/development-guide.md](docs/development-guide.md) — 开发规范（环境/构建/编码/模型/板端/Git/文档）
- [docs/architecture.md](docs/architecture.md) — 系统架构与完整数据通路
- [docs/board-operations.md](docs/board-operations.md) — 板端部署/运行/恢复手册

## 许可证

见 `LICENSE`（暂未指定，待定）。
