# Hispark SoC China 2026

面向海思 SS928 / SD3403（Hi3403V100，海鸥派）平台的**实时图像增强系统**参赛软件仓库。

系统在板端构建一条以 ISP 硬件为底座、低频 NPU 参数网络为控制旁路的**双向曝光校正**链路：
既能拉亮暗部，也能抑制高光过曝，并通过 HDMI 本地显示与 RTSP 远程串流输出。

> 说明：SS928 / Hi3403V100 / SD3403 / 海鸥派 / EulerPi 均指同一块板卡。

## 系统总览

```text
主路径：
OS08A20 -> VI -> ISP(WDR/降噪/DRC/Gamma/CLUT) -> VPSS -> VO/HDMI
                                              -> VENC/RTSP

低频控制旁路：
VPSS 缩略图 -> ISP ParamNet -> Gamma/DRC/LDCI/Dehaze 参数 -> ISP 硬件 30fps 施加
ISP AE 统计 -> 场景判决 -> 规则 Gamma（兜底）
ISP DRC/LDCI ------------------------------------> ISP 自动/配置策略
```

当前生产程序已用规则判决驱动 ISP Gamma 在板端 30fps 跑通；LCDP param-net 正式训练、
256x144 VPSS chn2 + AIPP OM、板端 NPU 推理和 SS928 `17v2` C LUT bridge 已完成独立验证。
完整 RGB CLUT 动态预览已跑通，但强光场景仍有 pre-CLUT 裁剪、端点保护不足和 post-CLUT
反馈问题；当前长期展示仍使用稳定 Gamma 路径。VENC H.264 + 轻量 RTSP/TCP 串流已接入
生产入口并完成纯串流板端验收：1024x576@30、约 3Mbps、断线重连和退出清理正常。
NN/bridge 已接入生产控制线程：启用模型时使用 chn2+AIPP 推理，经安全 bridge 限幅后刷新
ISP CLUT；连续三次普通刷新失败会关闭 CLUT 并回退规则 Gamma，ACL 运行时致命错误则安全停链。
当前主线已收敛为 **ISP 参数自动调优**路线：NN 从场景图像预测 Gamma/DRC/LDCI/Dehaze
参数（~88 维连续值），ISP 硬件对每一帧施加，实现真正的 30fps 空间自适应增强。
CTBG per-pixel apply 路线因 89ms/帧的硬件限制已关闭（仅保留诊断用途）。
完整 Prompt 见 [docs/isp-auto-tuning-prompt.md](docs/isp-auto-tuning-prompt.md)。

生产基线：CoTF param-net + ISP CLUT 桥 30fps 跑通，RTSP H.264 1024x576 约 30fps/3Mbps，
HDMI 1024x600 30fps，Web 控制台（MediaMTX + Go 服务）已板端部署。

数据通路与设计依据见 [docs/architecture.md](docs/architecture.md)。

## 目录结构

起步保持精简，子目录随开发推进再细化。

| 路径 | 用途 |
| --- | --- |
| `board/` | 板端应用（C/C++，交叉编译，CMake 构建）。 |
| `models/` | 模型网络、ONNX 导出器、LUT 工具、AIPP 配置与测试。 |
| `scripts/` | 环境、构建、部署、板端运行脚本。 |
| `web/` | 规划中的板端 Web/API/管理服务和静态页面；实现后按技术路线创建。 |
| `docs/` | 开发规范、系统架构、板端操作手册。 |

模型侧分层为 `models/{networks,exporters,tools,configs,tests,weights}`；可执行 Python 入口使用
`python -m models.<子包>.<模块>`。

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
scripts/test_host.sh              # 本机 cc 编译并运行 SDK-free 单元测试
```

板端代码组织与测试规范见 [board/README.md](board/README.md) 与 [docs/development-guide.md](docs/development-guide.md) §10。

### 其它

- 环境与依赖详解：[docs/development-guide.md](docs/development-guide.md) §3–§4。
- 模型训练/导出/转换：[models/README.md](models/README.md)（两套独立环境，依据 SDK 文档固化）。
- 板端网络与 SSH：[docs/network-access.md](docs/network-access.md)。
- 部署/运行：先在 `~/.ssh/config` 配置 `hispark-remote`，再运行
  `scripts/deploy_board.sh` / `scripts/run_board.sh`。
- RTSP 开机自启动：构建后运行
  `BOARD=hispark-remote scripts/install_board_service.sh`；默认仅 RTSP，使用板端
  `socchina-display on|off|status` 切换 HDMI。完整说明见
  [docs/board-operations.md](docs/board-operations.md) §8。

## 文档导航

- [AGENTS.md](AGENTS.md) — AI 代理/协作者开发指南（阅读顺序、约定汇总、板端习惯、已知坑点）
- [docs/development-guide.md](docs/development-guide.md) — 开发规范（环境/构建/编码/模型/板端/Git/文档）
- [docs/architecture.md](docs/architecture.md) — 系统架构与完整数据通路
- [docs/isp-auto-tuning-prompt.md](docs/isp-auto-tuning-prompt.md) — ISP 自动调优 AI 代理实施 Prompt（当前主线）
- [docs/TODO.md](docs/TODO.md) — 待完成清单与试验记录
- [docs/board-operations.md](docs/board-operations.md) — 板端部署/运行/恢复手册
- [docs/network-access.md](docs/network-access.md) — 受管网络、动态地址、Clash TUN 与 SSH

## 许可证

见 `LICENSE`（项目交付前仍需由所有者确定最终许可证）。
