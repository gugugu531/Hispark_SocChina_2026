# SS928 ISP 参数自动调优系统——AI 代理实施 Prompt

## 项目目标

在 SS928 板端实现基于神经网络的 ISP 参数自动调优系统，替代当前手工启发式规则（`board/src/ctbg_isp_map.c`），使 DRC/Gamma/LDCI/Dehaze 参数由神经网络从场景图像中直接预测，ISP 硬件对**每一帧**全分辨率施加（区别于 CTBG 的 30 帧中仅 1 帧增强）。

核心思路：将低光照增强从"逐像素 NPU 施加"转变为"NN 预测 ISP 参数 + ISP 硬件施加"，利用 SS928 ISP 已有的局部色调映射（DRC）和局域对比度增强（LDCI）能力，实现真正的 30fps 空间自适应增强。

## 背景与约束

- **硬件平台**: SS928 (Hi3403V100), Ascend 310 NPU (4T+6T 双核), ARM A55×1 CPU
- **当前管线**: OS08A20 → ISP(DRC/Gamma/LDCI/Dehaze) → VPSS → VO/VENC, 30fps
- **已有基础设施**:
  - `board/src/ctbg_isp_map.c` — 启发式 17×15 块聚合 → DRC/LDCI 参数，已通过 `ss_mpi_isp_set_drc_attr()` / `ss_mpi_isp_set_ldci_attr()` 验证热刷新可用
  - `board/src/isp.c:isp_gamma_apply_curve(curve[64], strength)` — 接受 64 节点浮点曲线，已封装，验证可用
  - `ss_mpi_isp_set_dehaze_attr()` — Dehaze 参数热刷新，验证可用
  - CTBG estimator 的 MobileIE backbone ONNX→OM 构建流程已跑通（ATC 5.20.T6.2.B060），可作为模型导出参考
  - GPU 训练环境: RTX 4060 8GB, PyTorch 2.5.1 (conda torch2), CUDA 13.0
- **SS928 ISP 参数结构**（来自 SDK 头文件和调优指南）:
  - ISP 管线顺序: AE → DRC → Gamma → LDCI → Dehaze
  - DRC: `tone_mapping_value[200]` [0,65535], `local_mixing_bright_x[33]` [0,128], `local_mixing_dark_x[33]` [0,128], `blend_luma_*` [0,255], `spatial_filter_coef` [0,5], `range_filter_coef` [0,10], `contrast_ctrl` [0,15]
  - Gamma: `table[1025]` [0,4095] 12-bit, `curve_type = USER_DEFINE`
  - LDCI: `he_pos_wgt{wgt,sigma,mean}`, `he_neg_wgt{wgt,sigma,mean}`, `blc_ctrl` [0,511], `gauss_lpf_sigma` [1,255]
  - Dehaze: `strength` [0,255]

## 架构设计原则

**本任务是全局回归而非密集预测**：输入一张低光照图像，输出一组 ISP 参数向量（~88 维连续值）。这与 CTBG 的逐像素系数预测（144×256 空间图）性质完全不同。MobileIE backbone 是为逐像素增强设计的（其 tail 输出 6ch×144×256），不适合本任务。

请参考以下架构范式自选或设计合适的 backbone：

| 参考 | 架构特点 | 适用性 |
|---|---|---|
| Qin et al. ECCV 2022 "Attention-Aware Learning for Hyperparameter Prediction in ISP Pipelines" | Multi-attention RAW→参数回归网络，可微代理训练 | 直接相关——预测 ISP 连续参数 |
| ACamera-Net (arXiv 2510.20550) | 轻量 CNN encoder + Distribution-Enhanced Loss，已在 HiSilicon Hi3516DV300 NPU 部署 | 硬件部署验证过的轻量架构 |
| DynamicISP (ICCV 2023, Sony) | 残差参数输出（静态基底+动态增量），稳定训练 | 参数回归稳定性好 |

设计约束：
- 推理需在 NPU 上完成（最终导出 ONNX→ATC→OM），算子在红名单外（无 Pow/Cast/ReduceMean）
- 参数量建议 < 500K（与现有 estimator 53K 同量级或适度增大）
- 输入为低分辨率下采样图像（256×144 或 512×288 RGB），输出为 88 维连续参数向量

## 技术方案

### 预测参数空间

NN 输出 88 维连续向量，映射到 SS928 ISP 硬件参数：

```
模块       NN 输出维度   映射方法                          HW API
──────────────────────────────────────────────────────────────────
Gamma      64×[0,1]      np.interp→1025 节点→4095 LUT     isp_gamma_apply_curve()
DRC tone    6×[0,1]      6 控制点插值→200 节点→65535     drc_attr.tone_mapping_value
DRC mix    12×[0,128]    3 段 bright+dark × FilterX       drc_attr.local_mixing_bright/dark_x
DRC ctrl    3             spatial_filter, range_filter,    drc_attr
                          contrast_ctrl
DRC blend   4×[0,255]     luma_max, dark/bright 阈值      drc_attr.blend_luma_*
LDCI        8             he_pos(3)+he_neg(3)+blc+σ       ldci_attr.manual_attr
Dehaze      1×[0,255]     直接标量                         isp_set_dehaze()
```

曲线参数（Gamma 64 节点、DRC tone 6 节点）需保证单调性约束。

### 训练数据

需使用低光照/曝光校正配对数据集。基本要求：包含欠曝/过曝图像及对应的正常曝光参考图（GT）。自行选择合适的数据集（如 LOL、SICE、MIT-Adobe FiveK、MSEC 等公开数据集），可组合多个来源以覆盖多样的光照条件和场景类型。数据增强（随机裁剪、翻转、色彩扰动）由代理自行决定。

### 训练策略

采用**可微 ISP 代理（Differentiable ISP Proxy）+ 监督训练**：

1. 构建 NumPy/PyTorch 可微 ISP 模拟器，实现 Gamma（插值 LUT）、DRC（torchcomp 可微动态范围压缩）、LDCI（CLAHE 近似）、Dehaze（暗通道先验）的可微版本
2. 用配对数据集训练：输入低光照图，NN 预测参数，模拟器施加，Loss 对比 GT
3. 损失函数自选（L1、LPIPS perceptual、Zero-DCE 零参考损失等），消融实验评估各 ISP 模块的独立贡献

### 板端集成

训练完成后：ONNX 导出 → ATC 构建 OM → 替换 `control_worker` 中的 `ctbg_isp_map_apply()` 调用。控制线程中 estimator 触发逻辑不变（场景变化时运行，~10Hz），仅将系数聚合+启发式映射替换为 ParamNet OM 推理。

## 参考资源

| 来源 | 用途 |
|---|---|
| Qin et al. ECCV 2022 | Attention-Aware ISP 参数回归架构 |
| ACamera-Net (arXiv 2510.20550) | 轻量 CNN 预测曝光/WB，HiSilicon NPU 部署 |
| DynamicISP (ICCV 2023, Sony) | 残差参数输出稳定训练 |
| ParamISP (CVPR 2024) | GlobalNet 色调映射公式 |
| torchcomp (PyPI) | 可微 DRC `compexp_gain()` |
| ReconfigISP (ICCV 2021) | 可微 ISP 代理架构参考 |
| SS928 SDK (`Reference/08. 原厂SDK/.../NNN/`) | ATC 工具指南、算子规格、ISP 调优指南 |

## 成功标准

核心判据——该系统必须满足**实际可用**：

1. **全帧有效**: NN 预测的 ISP 参数对**每一帧**生效（通过 ISP 硬件施加），不存在类似 CTBG writeback 的"30 帧中仅 1 帧增强"问题
2. **目视改善**: 在暗场景、背光场景、正常场景下，与纯 Gamma-only（strength=0.25）相比有明显目视改善——暗部可见性提升同时高光不过曝
3. **不引入劣化**: 不出现色偏、条带、闪烁、halo 等 ISP 参数不当导致的 artifacts
4. **场景自适应**: 不同光照条件下自动产出不同的参数组合（如暗场景增强 DRC 和 LDCI，亮场景仅微调 Gamma）
5. **量化可选**: SICE val 集上 PSNR 优于当前 CTBG 启发式规则（baseline: CTBG v9 19.83 dB）

## 实施顺序

```
Phase 1: ISP 模拟器
  - 用 NumPy/PyTorch 实现可微 Gamma、DRC、LDCI、Dehaze 模块
  - 验证模拟器输出与板端硬件输出一致性

Phase 2: 模型训练
  - 选定/设计 backbone 架构
  - SICE 数据加载 + 训练 pipeline
  - 监督训练 + 消融实验

Phase 3: 板端部署
  - ONNX→OM 导出
  - 替换 control_worker 中 ctbg_isp_map_apply()
  - A/B 目视对比测试
```

## 注意事项

- ATC 构建需 Ascend toolkit 环境（当前可用: 5.20.T6.2.B060），ccec 编译器路径已配置
- ONNX 图使用 NCHW 格式；AIPP（NV21 硬件转换）暂不可用于纯 elementwise 图（缺 TransData 格式桥接），如需要 NV21 输入需自行解决
- 板端测试前 `systemctl stop socchina-stream`（VB 池独占）
- DRC `tone_mapping_value` 200 节点范围 [0,65535]，Gamma `table` 1025 节点范围 [0,4095]
- 模型推理在控制线程中执行（~10Hz 周期，95ms 窗口），耗时需 < 50ms 以免阻塞控制环
