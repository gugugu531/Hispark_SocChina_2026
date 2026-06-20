# 系统架构与图像增强链数据通路

## 1. 设计目标与原则

- 任务：**实时双向曝光校正**——既拉亮暗部，也抑制高光过曝（而非单向拉亮）。
- 过曝是 sensor 像素阱饱和导致的**不可逆**信息丢失，正解是**拍摄端预防（WDR/曝光控制）**，而非末端补救。
- 关键设计：**多帧降噪交给 ISP 硬件（3DNR/HNR）**；NN 读取低分辨率缩略图并预测低频参数，
  只预测 RGB 17³ CLUT；ISP CLUT 对全分辨率帧施加。Gamma 由 AE 规则控制，DRC/LDCI
  由 ISP 自动或配置策略管理。这样规避多帧融合鬼影和整图模型的访存瓶颈。
- 主路径不做 CPU 或 NPU 全分辨率逐像素搬运。NPU 与少量 CPU LUT 打包仅在低频控制旁路工作。

## 2. 端到端数据通路

```text
主路径（每帧，目标 30fps）：

OS08A20 -> VI -> ISP
  ISP: WDR/3A/3DNR/DRC/dehaze -> Gamma/CLUT(当前参数) -> YVU420SP
       -> VPSS chn0 1024x600 -> VO -> HDMI
       -> VPSS chn1 1024x576 -> VENC -> RTSP

控制旁路（低频或场景变化时）：

ISP AE 统计 -> 规则 Gamma
VPSS chn2 256x144 NV21 -> AIPP -> CoTF-inspired param-net -> 安全 CLUT 桥
       -> 颜色: ISP 17³ CLUT
       -> control_should_refresh_lut 控制刷新频率
ISP DRC/LDCI ------------------------------------> ISP 自动/配置策略
```

ISP 参数块位于公共图像链内，因此同一 ISP 输出天然是“已增强帧”。产品范围为全屏增强与增强开关，
不实现严格同帧原图/增强图分屏，也不实现整图模型旁路。

## 3. 逐级明细

| # | 阶段 | 硬件块 | 输入 | 输出 | 操作 | NPU |
|---|---|---|---|---|---|---|
| 1 | 采集 | VI+MIPI | OS08A20 RAW12 WDR/DOL | RAW(DOL) | 多曝光交错读出（OS08A20 WDR 有限制，见§7）| 0 |
| 2 | 宽动态融合 | ISP | 多帧RAW | 1帧宽DR RAW | WDR融合（源头保高光，受 §7 限制）| 0 |
| 3 | 降噪 | ISP 3DNR+HNR | RAW/YUV | 去噪YUV | 时域+空域硬件降噪（替代NN融合）| 0 |
| 4 | 色调与 LUT 施加 | ISP dehaze/LTM/DRC/Gamma/CLUT | RAW/RGB/YUV | YVU420SP | 每帧施加当前参数 | 0 |
| 5 | 分发缩放 | VPSS | NV21 全幅 | chn0/chn1/chn2 | 1024x600 显示 + 1024x576 串流 + 256x144 控制缩略图 | 0 |
| 6 | 参数网预处理 | AIPP（融入 OM） | chn2 NV21 | NCHW FP16 RGB /255 | CSC+归一+布局 | NNN，低频 |
| 7 | 参数预测 | NNN/ACL OM | 256x144 FP16 | RGB 17³ LUT | CoTF-inspired param-net | NNN，缩略图 benchmark 约 0.8ms/次 |
| 8 | 安全 CLUT 桥 | CPU 轻量 | RGB 17³ LUT | ISP packed CLUT | clamp、端点/单调/步长检查、17v2 格式转换 | 0 |
| 9 | 参数刷新 | CPU→ISP | 安全参数 | ISP 参数块 | 限流/迟滞后热刷新 | 0 |
| 10a | 本地显示 | VO→HDMI | chn0 NV21 | 1024x600 DVI | 零拷贝送显 | 0 |
| 10b | 远程串流 | VENC→RTSP | NV21 | H.264 | 硬件编码+推流 | 0 |
| 11 | 控制 | CPU | ISP统计 | ISP参数+刷新决策 | 场景判决+迟滞 | 0 |

## 4. 唯一产品运行配置

- **Config-R（实时 30fps，主线）**：低频参数网络读取 VPSS 缩略图；ISP Gamma/DRC/CLUT
  对每帧全分辨率施加。正式契约为 **NN→CLUT、AE 规则→Gamma、ISP 策略→DRC/LDCI**。
  规则判决→Gamma 与正式 NN、chn2+AIPP、17v2 bridge 已接入生产
  control worker；连续失败降级回退和 post-CLUT 基础反馈抑制已实现。生产配置可选择
  linear/WDR 2to1 与目标帧率，待正式画质与长时验收。
- **兜底路线**：若双向方案在画质/工期上均不达预期，退回探索层已板端实测的
  **Zero-DCE Lite**（单向提亮，须配强光场景门控）。产品路线与已关闭路线见
  [model-route-summary.md](model-route-summary.md)。

### 4.1 已关闭的整图路线

本仓库所称「CoTF 路线」只移植了官方 CoTF（`CoNet`）中「预测 3D-LUT」这一子组件（该子组件本身借自
Image-Adaptive-3DLUT / SepLUT），并把施加卸给 ISP 硬件。官方 CoTF 的协同变换、自适应采样和注意力
融合因落在 NNN 红名单已全部丢弃，故画质约等于全局 3D-LUT 级，不等于官方 CoTF。对照详见
[../models/cotf-route-verification.md](../models/cotf-route-verification.md) 与
[model-route-summary.md](model-route-summary.md)。

横评 5 个架构后确认，「输出整图」的模型在 1024x576 都撞**全分辨率访存的像素线性地板**。
ExpoCurveNet、Config-Q、`postproc/compose` 与整图分屏路径已明确舍弃，不属于产品待开发范围。
相关模型和测速数据只作为路线决策证据保留。

| 路线 | NN 做什么 | 全分辨率施加 | 结论 |
| --- | --- | --- | --- |
| ExpoCurveNet 整图输出 | 全分辨率逐像素增强 | NPU | **舍弃**：不实现产品路径 |
| CoTF-inspired CLUT | 低分辨率出 3D-LUT（~1–5ms） | ISP 硬件 CLUT | **唯一产品路线** |

硬件 CLUT/Gamma 施加、ACL 推理和 30fps 主链均已分别验证。CLUT 几何已由厂商资料和板端 sweep
确认：17³ 逻辑节点按三轴奇偶拆为 8 个 bank，并以 4 路交织写入 5508 项；RGB 轴序和
R高/G中/B低位序的 identity 门禁已通过。生产参数桥与失败降级已完成，剩余工作是动态闭环画质验证。

## 5. 模块与代码映射

板端代码平铺在 `board/src/`。产品路径使用 `capture/isp/vpss/infer/lut_bridge/control/display/stream`。
头文件位于 `board/include/`，跨模块通道、状态和指标约定放
`pipeline.h`；`pipeline.c` 提供配置默认值、合法性和状态名契约，`main.c` 负责硬件生命周期和线程编排。

Config-R 固定 `chn0=1024x600` 显示、`chn1=1024x576` 串流、
`chn2=256x144` CoTF 控制缩略图。模块接口、帧所有权、刷新事务和并行开发完成定义见
[data-path-interface-design.md](data-path-interface-design.md)。

## 6. 与数据通路绑定的待验证点

1. ✅ 已验证（2026-06-14，`models/expo-curve-network.md`）：`/4 AvgPool→backbone→ConvTranspose→全分辨率施加`
   **干净落 AICore（无 AICPU/Cast）**；但 **1024x576 超 33ms 预算**（niter=8=94.9ms，niter=1 仍 38.9ms），
   瓶颈为全分辨率访存而非 backbone。该结果已用于关闭整图产品路线，不再继续开发。
2. ✅ 已验证（2026-06-14）：AIPP 全分辨率 CSC/归一开销 **≈0ms**（挂/不挂同速，差值噪声内），
   预处理可零开销融入 OM 前端。
3. 🟡 参数网主线：正式权重、chn2+AIPP、17v2 bridge、生产 control worker、降级回退和
   post-CLUT 基础反馈抑制已联机；待 10 分钟验收、动态参数标定和闭环画质评估。
4. ~~ISP 统计读取~~ ✅ 已验证（2026-06-11）：`ss_mpi_isp_get_ae_stats` 低频读取正常，
   `isp_get_luma_stats` 归约为 mean/clip% 直接供 `control_decide`（见 `board/README.md`）。
5. VO 视频层的现场 flicker 观感待确认；不再规划 VGS 分屏。

设备运行完整性已经形成以下闭环：systemd 配置 schema v1 → 严格参数校验 → linear/WDR
生产起链 → 统一状态日志与逆序释放 → 普通故障自动重试；ACL/SMMU 致命故障使用退出码 70，
由 `RestartPreventExitStatus` 阻止重启风暴，等待板卡干净重启。`socchina-health` 用于只读核对配置、
服务、MIPI、进程、RTSP、VPSS/NNN 与可选 HDMI 状态。

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
