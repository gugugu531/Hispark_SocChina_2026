# 模型路线稳定结论

## 目标

汇总阶段 B 的模型侧稳定结论，为后续整链实现提供单一决策入口。具体命令、算子清单和完整测速表仍保留在
[`models/expo-curve-network.md`](../models/expo-curve-network.md) 与
[`models/cotf-route-verification.md`](../models/cotf-route-verification.md)。

## 技术路线分级（主推 / 备选 / 兜底）

| 级别 | 路线 | 一句话 | 实时性 | 方向 | 状态 |
| --- | --- | --- | --- | --- | --- |
| **主推（开发中）** | CoTF-inspired 参数网 + ISP 参数块 | NN 低频出参数，ISP Gamma/DRC/CLUT 全分辨率施加 | ✅ 1024x576 可（NN 移出每帧路径） | 双向 | 正式权重、256x144+AIPP、17v2 bridge、生产闭环与失败降级已完成；画质待验收 |
| **备选（已验证可行）** | 曲线网络 ExpoCurveNet（整图增强） | 单段全分辨率 NN 直接出增强帧 | 🟡 仅 768x432（27ms，NPU 每帧满载） | 双向 | 结构去风险完，可直接训练 |
| **兜底** | Zero-DCE（Lite） | 探索层已板端实测的单向低光增强 | ✅ 640x320≈30ms | **单向（仅提亮）** | 已有 OM 与实测基线，需场景门控 |

选型原则：**优先主推路线**（唯一能 1024x576 真实时且释放 NPU）；实现成本敏感或主推联机受阻时用**备选**
（768x432 单段网络，简单直接）；若双向方案画质/工期均不达预期，退到**兜底** Zero-DCE（单向，须强光旁路）。

## 命令/路径

- 整图模型结构、OM 算子和速度矩阵：
  [`models/expo-curve-network.md`](../models/expo-curve-network.md)
- CoTF 参数网络、LUT 桥和 CLUT API 验证：
  [`models/cotf-route-verification.md`](../models/cotf-route-verification.md)
- 原始速度矩阵：`artifacts/expo_curve_speed_matrix.csv`（本地生成物，索引见
  [`artifacts/README.md`](../artifacts/README.md)）
- 模型侧测试：`python -m pytest models/tests -q`

## 结果

- AIPP 可直接消费 VPSS NV21；`1024x576` 与 `640x360` 实测附加开销在测量噪声内，约为 `0ms`。
- ExpoCurveNet、Zero-DCE Lite、SCI、MSEC 等“输出整图”模型均受到全分辨率访存限制，参数量小不等于快。
- ExpoCurveNet 的实时备选为 `768x432 + shared niter8`，单次 OM 执行约 `27.2ms`；该数字不含整链开销。
- `1024x576` 整图输出路线无法满足 33ms 预算，不再作为 Config-R 主线。
- CoTF 参数网络正式 LCDP OM 使用 `256x144` VPSS chn2 NV21+AIPP 输入，板端 30/30 成功，
  NPU exec 平均 `1.13ms`（首轮冷启动最大 `3.84ms`）。
- CLUT/Gamma 硬件施加、热刷新、ACL 推理和 30fps 主链已分别验证；CLUT 为 17³ 逻辑节点、
  8 个奇偶 bank 的 4 路交织存储，RGB/RGB identity 门禁已通过。
- param-net 已用 LCDP 的 1415/100/218 对完成 200 epoch 正式训练；best epoch 167，
  val PSNR `19.7247dB`、独立 test PSNR `20.4813dB`（输入基线 `14.0203dB`）。
- checkpoint→ONNX→FP16+AIPP OM→板端相机帧推理链已完成；17v2 C bridge、生产刷新事务、
  Gamma/DRC/CLUT 用途分流、失败降级和基础画质护栏均已完成。
- 修正后的 17v2 RGB bridge 已在 `test_paramnet_live` 动态预览：每 30 帧推理并热刷完整 CLUT，
  20 秒 604 帧、20/20 更新成功；强光场景仍有预裁剪、亮度压暗和 post-CLUT 反馈问题，当前长时
  展示回退 Gamma，RGB 路径待补高光/端点护栏后再做长期画质验收。

## 解读

Config-R 主线应采用“低频参数网络 + ISP 参数块每帧施加”。这能把 NPU 从每帧关键路径移出，并复用已经验证
达到 30fps 的 VI/ISP/VPSS/VO 硬件链。

`768x432` ExpoCurveNet 保留为整图增强备选和对照路径；`1024x576` 整图增强归入 Config-Q 或低帧率场景。

## 待测技术路线

围绕主推路线，以下为尚未完成的画质与控制项：

1. **与官方 CoTF 的差距（正名）**：我们的路线只移植了官方 CoTF（`CoNet`）里"预测 3D-LUT"这一子组件
   （该子组件本身借自 Image-Adaptive-3DLUT / SepLUT）。官方 CoTF 的两个命名贡献——**协同变换**
   （自编码器逐像素分支）与**自适应采样**（grid_sample）——连同其注意力融合（Attention/LayerNorm/GELU）
   全部落在 NNN 红名单，已**全部丢弃**。故画质上限 ≈ 全局 3D-LUT 级（非空间自适应），**不等于官方 CoTF**。
   文档与代码命名宜表述为"受 CoTF 启发的 3D-LUT / ISP-CLUT 路线"。对照详见
   [`models/cotf-route-verification.md`](../models/cotf-route-verification.md)。

2. **细节/局部对比补偿走 ISP 硬件块，而非第二个全分辨率 NN**：全局 LUT 是逐像素点变换，不模糊但会压扁
   "压缩色调区"的局部对比、并带量化 banding。补偿用 ISP 的 **LDCI（局部对比）/ DRC·LTM（局部色调）/
   SHARPEN（锐度）/ Dither（压条带）/ 3DNR·HNR（压噪）**——全分辨率、零 NPU、已在主链上。**不**新增全分辨率
   细节网络：那会撞回访存地板（实测 ~30ms+/帧），且被"单段 768x432 网络（27ms）"严格支配。

3. **余量 NPU 跑"感知→控制"而非像素任务**：LUT 路线下主 NNN 约 85%+ 空闲、第二颗 **SVP_NNN（5.6T）整颗
   闲置**。余量优先放**低频感知→控制**模型（场景分类、人脸测光、ROI、运动/banding 检测），输出元数据喂
   `control.c`，把曝光做得更智能；**不**放全分辨率像素任务（同样撞地板 + 进图像通路加延迟）。多数可直接塞
   主 NNN 空闲时段（复用 HiEuler ModelZoo 已验证 OM：YOLO11s/FaceNet 等），**无需**点亮 SVP_NNN 的独立
   dpico 工具链；后者留待主 NPU 余量不够或需真并行重任务时再起。护栏：内存带宽 / SMMU·CMDQ 稳定性 / 功耗共享，
   并发加任务前需实测。

## 兜底路线：Zero-DCE（Lite，单向，已板端实测）

若主推与备选的双向方案在画质或工期上均不达预期，退回探索层已在板上跑通的 **Zero-DCE Lite FP16 OM**
（同芯片族 SVP_NNN/ACL 实测基线）：

- 已知实测（离线单算 OM 执行，口径见根工作区 `AGENTS.md`）：`640x320 ≈ 29.9ms`、`1024x640 ≈ 96ms`、
  `160x160 + AIPP ≈ 1ms`；本仓库横评复测 `640x360 ≈ 33.9ms`、`320x180 ≈ 8.5ms`（与历史一致，交叉验证测速工具）。
- 算子全绿、可干净落 AICore，是**已验证能实时**的方案。
- **限制**：Zero-DCE 是**单向提亮**，在强光/混合光场景会把高光进一步推向截断（有害，见 roadmap §6.4）。
  作兜底须配**场景自适应门控**：按 `control_decide` 的平均亮度/裁剪比例，强光场景旁路、仅暗场启用。
- 定位：保命选项——保证"至少有一个能实时、能上板的增强"，但牺牲双向能力。

## 下一步

1. ✅ 256x144 chn2+AIPP 推理和 17v2 bridge 已接入生产 control worker。
2. ✅ 已实现失败保旧参数、sticky `PIPELINE_DEGRADED` 和规则 Gamma 回退。
3. 🟡 已增加端点、单调性、最大变化量、高光门控及 post-CLUT 基础反馈抑制；余动态标定。
4. 测量 10 分钟稳定性和现场 flicker；短测事务 p95 已低于 10ms。
5. 联调 ISP 局部块（AE/DRC/LDCI/SHARPEN/Dither），用 20–50 张代表性相机帧做画质评估。
6. Config-Q 高画质抓拍、板端测试自动化和交付收尾；余量 NPU 感知→控制属于后续增强项。
