# 系统架构与图像增强链数据通路

## 1. 设计目标与原则

- 任务：**实时双向曝光校正**——既拉亮暗部，也抑制高光过曝（而非单向拉亮）。
- 过曝是 sensor 像素阱饱和导致的**不可逆**信息丢失，正解是**拍摄端预防（WDR/曝光控制）**，而非末端补救。
- 关键设计：**多帧降噪交给 ISP 硬件（3DNR/HNR）**；NN 读取低分辨率缩略图并预测低频参数，
  ISP Gamma/DRC/CLUT 对全分辨率帧施加。这样规避多帧融合鬼影和整图模型的访存瓶颈。
- 主路径不做 CPU 或 NPU 全分辨率逐像素搬运。NPU 与少量 CPU LUT 打包仅在低频控制旁路工作。

## 2. 端到端数据通路

```text
主路径（每帧，目标 30fps）：

OS08A20 -> VI -> ISP
  ISP: WDR/3A/3DNR/DRC/dehaze -> Gamma/CLUT(当前参数) -> YVU420SP
       -> VPSS chn0 1024x600 -> VO -> HDMI
       -> VPSS chn1 1024x576 -> VENC -> RTSP

控制旁路（低频或场景变化时）：

ISP AE 统计 ---------------------------> control_decide
VPSS chn2 256x144 NV21 -> AIPP -> CoTF-inspired param-net -> 安全参数桥
       -> 全局曝光/色调: ISP Gamma；动态范围: DRC；颜色: 17³ CLUT
       -> control_should_refresh_lut 控制刷新频率

整图增强备选：

VPSS chn1 -> AIPP -> ExpoCurveNet 768x432 -> 后处理 -> VGS/VO
```

ISP 参数块位于公共图像链内，因此同一 ISP 输出天然是“已增强帧”。如果演示必须同时显示严格同帧的
原图/增强图，需要额外的旁路能力或使用整图增强备选；当前主线优先支持全屏增强和增强开关切换。

## 3. 逐级明细

| # | 阶段 | 硬件块 | 输入 | 输出 | 操作 | NPU |
|---|---|---|---|---|---|---|
| 1 | 采集 | VI+MIPI | OS08A20 RAW12 WDR/DOL | RAW(DOL) | 多曝光交错读出（OS08A20 WDR 有限制，见§7）| 0 |
| 2 | 宽动态融合 | ISP | 多帧RAW | 1帧宽DR RAW | WDR融合（源头保高光，受 §7 限制）| 0 |
| 3 | 降噪 | ISP 3DNR+HNR | RAW/YUV | 去噪YUV | 时域+空域硬件降噪（替代NN融合）| 0 |
| 4 | 色调与 LUT 施加 | ISP dehaze/LTM/DRC/Gamma/CLUT | RAW/RGB/YUV | YVU420SP | 每帧施加当前参数 | 0 |
| 5 | 分发缩放 | VPSS | NV21 全幅 | chn0/chn1/chn2 | 1024x600 显示 + 1024x576 串流 + 256x144 控制缩略图 | 0 |
| 6 | 参数网预处理 | AIPP（融入 OM） | chn2 NV21 | NCHW FP16 RGB /255 | CSC+归一+布局 | NNN，低频 |
| 7 | 参数预测 | NNN/ACL OM | 256x144 FP16 | 17³ LUT/色调参数 | CoTF-inspired param-net | NNN，缩略图 benchmark 约 0.8ms/次 |
| 8 | 安全参数桥 | CPU 轻量 | NN 参数 | Gamma/DRC/CLUT 输入 | clamp、正则检查、格式转换 | 0 |
| 9 | 参数刷新 | CPU→ISP | 安全参数 | ISP 参数块 | 限流/迟滞后热刷新 | 0 |
| 10a | 本地显示 | VO→HDMI | chn0 NV21 | 1024x600 DVI | 零拷贝送显 | 0 |
| 10b | 远程串流 | VENC→RTSP | NV21 | H.264 | 硬件编码+推流 | 0 |
| 11 | 控制 | CPU | ISP统计 | ISP参数+刷新决策 | 场景判决+迟滞 | 0 |

## 4. 两套运行配置

- **Config-R（实时 30fps，主线）**：低频参数网络读取 VPSS 缩略图；ISP Gamma/DRC/CLUT
  对每帧全分辨率施加。当前规则判决→Gamma 已板端 30fps 跑通，NN 接入仍在开发。
- **Config-R 备选**：`768x432 + 共享曲线 niter8` 整图 OM，单次执行约 `27.2ms`；适合快速演示，
  但仍需为整链开销留预算，且 NPU 每帧参与。
- **Config-Q（拍照/低帧，高画质）**：更大输入（如 1024x640）、更多迭代、可叠加多帧堆栈；约 100ms 量级，用于按键抓拍增强。
- **兜底路线**：若双向方案在画质/工期上均不达预期，退回探索层已板端实测的 **Zero-DCE Lite**（单向提亮，须配强光场景门控）。技术路线分级（主推/备选/兜底）见 [model-route-summary.md](model-route-summary.md)。

### 4.1 两条候选模型路线（曲线网络 vs 受 CoTF 启发的 LUT 路线）

> **命名说明**：本仓库所称「CoTF 路线」只移植了官方 CoTF（`CoNet`）中「预测 3D-LUT」这一子组件（该子组件本身借自
> Image-Adaptive-3DLUT / SepLUT），并把施加卸给 ISP 硬件。官方 CoTF 的命名贡献——协同变换、自适应采样、注意力
> 融合——因落在 NNN 红名单已**全部丢弃**，故画质 ≈ 全局 3D-LUT 级，**不等于官方 CoTF**。对照详见
> [../models/cotf-route-verification.md](../models/cotf-route-verification.md) 与 [model-route-summary.md](model-route-summary.md)。
> 硬件施加和 ACL 推理已分别联机；训练闭环已完成，正式权重与生产接线待完成。

横评 5 个架构后确认，「输出整图」的模型在 1024x576 都撞**全分辨率访存的像素线性地板**（与参数量无关）。两条路线：

| 路线 | NN 做什么 | 全分辨率施加 | NPU 占用 | 1024x576 实时 | 适用 |
| --- | --- | --- | --- | --- | --- |
| **曲线网络（本仓库 ExpoCurveNet）** | /4 backbone 出曲线 + **全分辨率逐像素施加** | 在 NPU | ~100%（每帧 gating） | ❌（768x432 可，27ms） | 快速上线、实现简单 |
| **CoTF（NN 出 LUT + ISP 施加）** | 低分辨率出 3D-LUT（~1–5ms） | **ISP 硬件 CLUT**（零 NPU） | ~3–15%（低频刷 LUT） | ✅（NN 移出每帧路径） | 全分辨率真实时、释放 NPU |

硬件 CLUT/Gamma 施加、ACL 推理和 30fps 主链均已分别验证。CLUT 几何已由厂商文档确认：
17³ 逻辑节点写入 17×18×18 带填充存储。剩余工作是正式训练、chn2+AIPP、参数桥和动态闭环验证。

## 5. 模块与代码映射

板端代码平铺在 `board/src/`。CoTF 主线需要 `capture/isp/vpss/infer/lut_bridge/control/display/stream`；
`postproc/compose` 主要服务整图增强备选。头文件位于 `board/include/`，跨模块通道、状态和指标约定放
`pipeline.h`；`main.c` 负责生命周期和线程编排。

Config-R 固定 `chn0=1024x600` 显示、`chn1=1024x576` 串流/整图备选互斥复用、
`chn2=256x144` CoTF 控制缩略图。模块接口、帧所有权、刷新事务和并行开发完成定义见
[data-path-interface-design.md](data-path-interface-design.md)。

## 6. 与数据通路绑定的待验证点

1. ✅ 已验证（2026-06-14，`models/expo-curve-network.md`）：`/4 AvgPool→backbone→ConvTranspose→全分辨率施加`
   **干净落 AICore（无 AICPU/Cast）**；但 **1024x576 超 33ms 预算**（niter=8=94.9ms，niter=1 仍 38.9ms），
   瓶颈为全分辨率访存而非 backbone。整图增强备选需降到 `640x360`/`768x432` + 共享曲线
   （`640x360 niter8 共享=19.5ms`）；1024x576 归 Config-Q。
2. ✅ 已验证（2026-06-14）：AIPP 全分辨率 CSC/归一开销 **≈0ms**（挂/不挂同速，差值噪声内），
   预处理可零开销融入 OM 前端。
3. 🟡 参数网主线：硬件施加和热刷新已联机；待正式权重、chn2+AIPP、NN→ISP 安全参数桥及动态闭环验证。
4. ~~ISP 统计读取~~ ✅ 已验证（2026-06-11）：`ss_mpi_isp_get_ae_stats` 低频读取正常，
   `isp_get_luma_stats` 归约为 mean/clip% 直接供 `control_decide`（见 `board/README.md`）。
5. VO 视频层的现场 flicker 观感待确认；VGS 分屏仅属于整图增强备选。

## 7. 已核实的 SDK 接口与关键约束

接口名核对自海思 SS928 SDK 头文件（`ss_mpi_*` 函数 + `ot_*` 类型）：

| 用途 | 已核实接口 |
| --- | --- |
| 去雾 | `ss_mpi_isp_set_dehaze_attr` / `get` |
| 动态范围/局部色调(DRC/LTM) | `ss_mpi_isp_set_drc_attr` / `get` |
| 3D-LUT（CLUT） | `ss_mpi_isp_set_clut_attr` / `set_clut_coeff` |
| 宽动态曝光 | `ss_mpi_isp_set_wdr_exposure_attr`、`ss_mpi_isp_set_expander_attr` |
| 曝光 | `ss_mpi_isp_set_exposure_attr` |
| AE/亮度统计 | `ss_mpi_isp_get_ae_stats` |
| WDR 模式枚举 | `OT_WDR_MODE_{2,3,4}To1_{LINE,FRAME}` 等 |
| IVE 直方图均衡/CSC/滤波+CSC/阈值 | `ss_mpi_ive_equalize_hist`、`ss_mpi_ive_csc`、`ss_mpi_ive_filter_and_csc`、`ss_mpi_ive_threshold` |
| VGS 缩放/合成 | `ss_mpi_vgs_add_scale_task`、`ss_mpi_vgs_add_*_task` |
| VENC 编码 | `ss_mpi_venc_send_frame`、`ss_mpi_venc_get_stream` |
| VPSS 取帧 | `ss_mpi_vpss_get_chn_frame` |

关键约束：

- **OS08A20 的 WDR 模式有文档记载的限制**（SDK《Sensor support list》原文：short exposure
  change < vblanking，切帧率时收敛变慢）。SDK 仅适配 **2to1 WDR**（10bit 4K，5–30fps），无 3to1。
  **2026-06-11 板端初测正面**：30fps 满帧稳定，同场景高光/暗部裁剪大幅下降（数据见
  `board/README.md`）；切帧率收敛与画质细调待测。若后续不达预期，过曝防线退化为
  **AE 曝光控制 + DRC/CLUT 色调压缩**为主。
- Resize/插值在 NNN 上实测异常，上采样统一用 ConvTranspose（见 `development-guide.md` §6）。
