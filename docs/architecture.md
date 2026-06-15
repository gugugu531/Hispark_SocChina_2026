# 系统架构与图像增强链数据通路

## 1. 设计目标与原则

- 任务：**实时双向曝光校正**——既拉亮暗部，也抑制高光过曝（而非单向拉亮）。
- 过曝是 sensor 像素阱饱和导致的**不可逆**信息丢失，正解是**拍摄端预防（WDR/曝光控制）**，而非末端补救。
- 关键设计：**多帧降噪交给 ISP 硬件（3DNR/HNR）**；NN 只做**单输入"低分辨率预测曲线 + 全分辨率施加曲线"**。这样可规避多帧融合的运动鬼影、ACL 多输入未验证、模型过重与帧累积时延等风险。
- 除模型推理与 AIPP 在 NNN 上，其余环节全部由 ISP/VPSS/IVE/VGS/VENC/DSP 硬件块承担，**零 NPU、零 CPU 逐像素搬运**。

## 2. 端到端数据通路

```
┌─ 采集 + ISP（全程硬件，0 NPU）──────────────────────────────────────┐
│ OS08A20 ─MIPI CSI─► [VI] ─RAW12(WDR/DOL*)─► [ISP]   (*见§7 WDR限制)  │
│   ISP内: WDR融合 → 3A → BLC/坏点/LSC → demosaic                      │
│          → 3DNR+HNR(时域+空域降噪)  ← 替代NN多帧融合                  │
│          → dehaze / LTM / 动态对比度 / 3D-LUT  ← 压高光+提暗部底座    │
│          → CSC(RGB→YUV)                                              │
│   输出: YVU420SP(NV21)  + AE/亮度直方图统计(metadata)                │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ VB pool (零拷贝物理地址)
                    ┌──────────▼───────────[VPSS 硬件缩放, 0 NPU]──────┐
                    │ chn0 → 1024x600 NV21  (显示底图)                  │
                    │ chn1 → 1024x576 NV21  (模型输入, 16:9对齐)        │
                    │ chn2 → 320x180  NV21  (场景分析缩略图, 可选)      │
                    └─────┬──────────────────────┬─────────────────────┘
                          │chn1                   │chn0
         ┌────────────────▼─────────────┐        │
         │ [AIPP] 静态, 融入OM前端(NNN) │        │
         │  NV21(YUV420SP_U8,rbuv_swap) │        │
         │  → RGB → /255 → NCHW FP16    │  0 CPU │
         └────────────────┬─────────────┘        │
                          │ 1x3x576x1024 FP16     │
         ┌────────────────▼──────────────[NNN/ACL OM]──────────┐      │
         │ 单输入曲线预测网络:                                  │      │
         │  AvgPool /4 → 卷积backbone @144x256 (算力主体, 低)   │      │
         │  → 预测曲线参数 3*niter ch                           │      │
         │  → ConvTranspose 上采样曲线到全分辨率                │      │
         │  → 全分辨率施加曲线 x=x+r(x²-x) ×niter (逐像素, 廉价)│      │
         │ 输出: 增强后 1024x576 (FP16, 或模型尾部直接转NV21)   │      │
         └────────────────┬────────────────────────────────────┘      │
                          │ 增强帧                                     │
         ┌────────────────▼───────[后处理: Q6 DSP / IVE, 0 NPU]─┐      │
         │  FP16→NV21(给VENC) / FP16→ARGB1555(给GFBG)           │      │
         │  可选: IVE EqualizeHist / 3D-LUT 二次微调            │      │
         └────────┬───────────────────────────┬─────────────────┘      │
                  │ 增强NV21/ARGB              │                        │
        ┌─────────▼─────────[VGS 硬件合成]────▼────────────────────────▼─┐
        │  原图(chn0) | 增强图  分屏/叠加  → VO frame                    │
        └─────────┬───────────────────────────────────┬─────────────────┘
                  │                                     │
          [VO]→[HDMI 1024x600 DVI] 本地显示      [VENC H.264]→ RTSP 远程
                                                   ↑(零拷贝VB)
        ┌───────────────────────[场景自适应大脑: CPU 轻量]──────────────┐
        │ 读 ss_mpi_isp_get_ae_stats / chn2直方图 → mean_luma,clip%   │
        │ → 决策模式{旁路|提暗|压高光|双向} + 迟滞防抖                   │
        │ → 写回 ss_mpi_isp_set_{dehaze,drc,clut}_attr + OM 曝光目标    │
        └───────────────────────────────────────────────────────────────┘
```

## 3. 逐级明细

| # | 阶段 | 硬件块 | 输入 | 输出 | 操作 | NPU |
|---|---|---|---|---|---|---|
| 1 | 采集 | VI+MIPI | OS08A20 RAW12 WDR/DOL | RAW(DOL) | 多曝光交错读出（OS08A20 WDR 有限制，见§7）| 0 |
| 2 | 宽动态融合 | ISP | 多帧RAW | 1帧宽DR RAW | WDR融合（源头保高光，受 §7 限制）| 0 |
| 3 | 降噪 | ISP 3DNR+HNR | RAW/YUV | 去噪YUV | 时域+空域硬件降噪（替代NN融合）| 0 |
| 4 | 色调底座 | ISP dehaze/LTM/DRC/3D-LUT | YUV | YVU420SP | 压高光+提暗部+去雾 | 0 |
| 5 | 分发缩放 | VPSS | NV21 全幅 | chn0/chn1/chn2 | 硬件多路缩放 | 0 |
| 6 | 预处理 | AIPP(融入OM) | chn1 NV21 | NCHW FP16 RGB /255 | CSC+归一+布局 | NNN |
| 7 | 曲线预测+施加 | NNN/ACL OM | FP16 1x3x576x1024 | 增强帧 | /4降采样backbone + 全分辨率曲线施加 | NNN |
| 8 | 后处理 | Q6 DSP / IVE | FP16 | NV21 / ARGB1555 | 格式转换(+可选EqualizeHist/LUT) | 0 |
| 9 | 合成 | VGS | 原图+增强 | VO frame | 分屏/叠加/缩放 | 0 |
| 10a | 本地显示 | VO→HDMI | VO frame | 1024x600 DVI | — | 0 |
| 10b | 远程串流 | VENC→RTSP | NV21 | H.264 | 硬件编码+推流 | 0 |
| 11 | 控制 | CPU | ISP统计/chn2 | ISP参数+OM参数 | 场景判决+迟滞 | 0 |

## 4. 两套运行配置

- **Config-R（实时 30fps，主线）**：/4 backbone + 全分辨率曲线施加，ISP 3DNR 降噪。预算目标 ≤33ms/帧（AIPP ~1ms 实测于 160；OM、后处理、合成需板端实测确认）。
  - **板端实测修正（2026-06-15，`models/expo-curve-network.md`）**：曲线网络在 1024x576 全分辨率超预算（niter8=94.9ms，瓶颈为全分辨率访存），实时档实测可达上限为 **768x432 + 共享曲线（27ms）** 或 640x360。AIPP 开销实测 ≈0ms。
- **Config-Q（拍照/低帧，高画质）**：更大输入（如 1024x640）、更多迭代、可叠加多帧堆栈；约 100ms 量级，用于按键抓拍增强。

### 4.1 两条已验证的模型路线（曲线网络 vs CoTF）

横评 5 个架构后确认，「输出整图」的模型在 1024x576 都撞**全分辨率访存的像素线性地板**（与参数量无关）。两条路线：

| 路线 | NN 做什么 | 全分辨率施加 | NPU 占用 | 1024x576 实时 | 适用 |
| --- | --- | --- | --- | --- | --- |
| **曲线网络（本仓库 ExpoCurveNet）** | /4 backbone 出曲线 + **全分辨率逐像素施加** | 在 NPU | ~100%（每帧 gating） | ❌（768x432 可，27ms） | 快速上线、实现简单 |
| **CoTF（NN 出 LUT + ISP 施加）** | 低分辨率出 3D-LUT（~1–5ms） | **ISP 硬件 CLUT**（零 NPU） | ~3–15%（低频刷 LUT） | ✅（NN 移出每帧路径） | 全分辨率真实时、释放 NPU |

CoTF 路线已逐环验证 + 代码就绪（`isp_set_clut`/`isp_load_clut_lut` + `control_should_refresh_lut` + host 打包桥），
剩 CLUT mesh 标定 + 联机点亮两步板端工作。完整记录见 [../models/cotf-route-verification.md](../models/cotf-route-verification.md)。

## 5. 模块与代码映射

板端代码平铺在 `board/src/`，**每个数据通路阶段一个 `.c`**（`capture/isp/vpss/preproc/infer/postproc/compose/display/stream`，加 `control` 场景控制），头文件在 `board/include/`（公共 `log.h/version.h`，跨模块共享的帧结构/枚举放 `pipeline.h`）。`main.c` 是主程序入口。加文件即扩展，不嵌套子目录（详见 `development-guide.md` §1）。

## 6. 与数据通路绑定的待验证点

1. ✅ 已验证（2026-06-14，`models/expo-curve-network.md`）：`/4 AvgPool→backbone→ConvTranspose→全分辨率施加`
   **干净落 AICore（无 AICPU/Cast）**；但 **1024x576 超 33ms 预算**（niter=8=94.9ms，niter=1 仍 38.9ms），
   瓶颈为全分辨率访存而非 backbone。**Config-R 实时档需降到 `640x360`/`768x432` + 共享曲线**
   （`640x360 niter8 共享=19.5ms`）；1024x576 归 Config-Q。
2. ✅ 已验证（2026-06-14）：AIPP 全分辨率 CSC/归一开销 **≈0ms**（挂/不挂同速，差值噪声内），
   预处理可零开销融入 OM 前端。
3. 后处理放 Q6 DSP 还是 IVE 更省。
4. ~~ISP 统计读取~~ ✅ 已验证（2026-06-11）：`ss_mpi_isp_get_ae_stats` 低频读取正常，
   `isp_get_luma_stats` 归约为 mean/clip% 直接供 `control_decide`（见 `board/README.md`）。
5. VGS 合成替代 GFBG，确认显示无 flicker。

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

