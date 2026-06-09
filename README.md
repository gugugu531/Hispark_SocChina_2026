# Hispark SoC China 2026

面向海思 SS928 / SD3403（Hi3403V100，海鸥派）平台的**实时图像增强系统**参赛软件仓库。

系统在板端构建一条以 ISP 硬件为底座、NPU 曲线/3D-LUT 网络为主体的**双向曝光校正**链路：既能拉亮暗部，也能抑制高光过曝，并通过 HDMI 本地显示与 RTSP 远程串流输出。

> 说明：SS928 / Hi3403V100 / SD3403 / 海鸥派 / EulerPi 均指同一块板卡。

## 系统总览

```
OS08A20 ─► VI ─► ISP(3F-WDR/降噪/LTM/去雾) ─► VPSS(多路缩放)
   ├─ chn0 ─► 显示底图 ─────────────────────────────┐
   └─ chn1 ─► AIPP ─► NNN/ACL OM(曝光校正) ─► 后处理 ─┤
                                                      ▼
                                  VGS 合成 ─► VO ─► HDMI 本地显示
                                            └─► VENC ─► RTSP 远程
   场景自适应大脑（读 ISP AE 统计 → 调 ISP/模型参数）贯穿全链
```

完整数据通路与设计依据见 [docs/architecture.md](docs/architecture.md)。

## 目录结构

| 路径 | 用途 |
| --- | --- |
| `board/` | 板端应用（C/C++，交叉编译 `aarch64-mix210-linux`，CMake 构建）。`src/` 子目录对应数据通路各级。 |
| `models/` | 模型训练（PyTorch）、ONNX 导出、ATC→OM 转换、配置与权重指针。 |
| `scripts/` | 环境、交叉编译、部署、板端运行脚本。 |
| `configs/` | 运行期配置（分辨率/模式/ISP 预设）。 |
| `tools/` | 主机侧工具（RTSP 查看、画质对比可视化等）。 |
| `tests/` | 单元/集成测试。 |
| `docs/` | 开发规范、系统架构、板端操作手册。 |
| `artifacts/` | 生成物（日志/截图/profile，绝大部分被 git 忽略）。 |

## 快速开始

1. 环境准备：见 [docs/development-guide.md](docs/development-guide.md) §3（明确列出所需软件包，不绑定特定 conda 环境）。
2. 模型转换：`models/`（PyTorch → ONNX → FP16 OM）。
3. 板端构建：`scripts/build_board.sh`（CMake + 交叉工具链）。
4. 部署运行：`scripts/deploy_board.sh` → `scripts/run_board.sh`。

详细步骤待各模块实现后补充。

## 文档导航

- [docs/development-guide.md](docs/development-guide.md) — 开发规范（环境/构建/编码/模型/板端/Git/文档）
- [docs/architecture.md](docs/architecture.md) — 系统架构与完整数据通路
- [docs/board-operations.md](docs/board-operations.md) — 板端部署/运行/恢复手册

## 许可证

见 `LICENSE`（暂未指定，待定）。
