# ISP 参数调优网络训练方法——文献调研

## 目标

回答一个核心工程问题：**当部署目标是闭源硬件 ISP（SS928）时，如何训练"从场景图像预测 ISP
参数"的网络，而无法把硬件 ISP 放进训练的梯度内层环？** 本文汇总学术界的成熟解法，映射到本项目
的约束，作为 [`isp-auto-tuning-prompt.md`](isp-auto-tuning-prompt.md) Phase 2（ParamNet 训练）的
方法学依据，与 [`../models/isp_simulator/semantic_gap.md`](../models/isp_simulator/semantic_gap.md)
的 sim-to-real 分析配套阅读。

> 记录口径：以下为公开论文的方法与结论，数字取自各论文/主页原文。本项目未复现，引用时保留出处。

## 1. 问题定式：硬件为什么进不了训练环

硬件 ISP 难以加入训练，是三件事叠加，而非单纯"不可微"：

1. **不可微**——没有 `∂output/∂θ`，梯度传不回 ParamNet。
2. **吞吐受限**——单次评估 ≈「MPI 写参数（<1ms，热刷新已验证）+ 等 ISP 稳定几帧（~100ms）+
   抓帧 + 打分」，现实约 `0.2–0.5s/次`。SGD 需 `1e5–1e6` 次评估 → 直接在环需数十小时且独占板卡。
3. **独占状态**——一次只有一个全局 ISP 状态，还与自有管线争 VB（见 `AGENTS.md` 媒体纪律）。

**学术界的共识解法只有一句：把硬件从"梯度内层环"挪到"离线数据生成"。** 硬件只用于离线批量产数据，
产完退出训练循环。按"硬件产什么数据"分成三条主线路（下节）。

## 2. 文献地图

### 路线 A：可微代理（Differentiable Proxy）——本问题的教科书标准答案

离线采样 `(输入图, 参数 θ) → 硬件ISP输出`，训练一个可微 CNN 代理 `S(x, θ) ≈ 硬件ISP(x, θ)`，
再拿 S 当训练环境反传 ParamNet。代理可微、可精修，替换掉"固定、靠猜"的解析模拟器。

| 文献 | 要点 | 与本项目关系 |
|---|---|---|
| **Tseng et al., SIGGRAPH Asia 2019 / ACM TOG 38(4)**，*Hyperparameter Optimization in Black-box Image Processing using Differentiable Proxies* | 用 **U-Net** 作黑盒 ISP 的可微代理；先离线采样拟合代理，再用一阶 SGD 搜超参；支持任意可微目标（含 FRCNN 检测）。把应用侧调优从"数月"降到"数小时"。 | **直接对口**——这就是"闭源 ISP 不可微、要 GD 优化其参数"的原始解法 |
| **Qin et al., ECCV 2022**，*Attention-aware Learning for Hyperparameter Prediction in Image Processing Pipelines* | 在代理基础上加**逐图像**参数预测网络：multi-attention 把 RAW 特征映射到参数空间，为每张图预测专属参数（而非全局固定）。在检测/分割/人眼观看三类任务验证。 | **最贴近本项目形态**——NN 从场景图逐帧预测 ISP 参数 |
| **arXiv 2407.17530 (2024)**，*Learning Instance-Specific Parameters of Black-Box Models Using Differentiable Surrogates* | 代理输入 = 图像 + 参数拼成的附加通道；另一个 U-Net "参数学习器"预测**逐实例**参数。**两阶段训练避免黑盒进环**：先用随机参数样本拟合代理，再只用代理反传训练参数学习器。BM3D 去噪 on SIDD：PSNR `36.51` vs 随机搜索 `36.0` / 基线 `33.23`。 | **方法模板最完整**——逐实例 + 黑盒 + 两阶段解耦，可直接照搬骨架 |
| **Applied Sciences 2025 (MDPI 15/6/3371)**，*Conditional GAN-Based Two-Stage ISP Tuning: A Reconstruction–Enhancement Proxy Framework* | 指出端到端单代理会带来明显色彩/纹理漂移；改为**两阶段（重建 + 增强）代理**，更好地抽取 ISP 管线的层级信息。 | 佐证"代理别一步到位"，与本项目**残差代理**思路一致 |

**代理路线的工程要点**：
- 别在 ~99 维里均匀撒点（维度灾难）；围绕 vendor-auto 工作点 + Sobol/拉丁超立方在降维子空间采，
  再沿优化轨迹**迭代补采**（active learning / 信任域）。
- 代理不必逐像素复现整图，可直接做"**目标代理**"`(图像特征, θ) → 质量指标`，维度骤降、采样更省。
- 采样量级 `1e3–1e5` 条，是**几小时、一次性**的板端时间，可摊销。

### 路线 B：硬件在环黑盒优化（0 阶，不训代理也不训网络）

不建代理，用无梯度优化器（CMA-ES / 贝叶斯优化 / 0 阶随机）直接查询硬件，逐场景搜最优参数。

| 文献 | 要点 | 关键结论 |
|---|---|---|
| **Mosleh et al., CVPR 2020**，*Hardware-in-the-Loop End-to-End Optimization of Camera Image Processing Pipelines*（代码：`princeton-computational-imaging/LITL-Optimization`） | 用**新型 0 阶随机求解器直接接硬件 ISP**，求解非线性多目标优化，无需任何可微近似。 | 车载 2D 检测上，比人工专家调优高 **30% mAP**，**比基于 ISP 近似（代理）的方法高 18% mAP** |

> **对本项目最重要的一条**：Mosleh 表明**直接在真实硬件上做黑盒优化，反而能超过可微代理法 18% mAP**。
> 因为代理引入了自己的"代理 vs 硬件"gap，替换但未消灭近似误差。这直接回应了"模拟器难以逼近闭源 ISP"
> 的担忧——最稳的做法可能是**根本不逼近它，直接用它**（每场景 `1e2–1e3` 次评估，几分钟/场景，离线可行）。

由此衍生本项目**路线 B'（蒸馏）**：离线逐场景在真实 ISP 上跑黑盒优化得 `θ*(scene)`，再用**参数空间
回归** `scene → θ*` 训 ParamNet。**训练环里没有 ISP，梯度全程可微**；硬件只在离线产标签。

### 路线 C：软件可微 ISP + RL/端到端（注意：这些工作不碰闭源硬件）

学术界很多"自适应 ISP"其实是在**自己实现的可微软件 ISP** 上做的，**不存在闭源硬件 gap**——引用其
结论时必须看清这条界线，否则会高估可迁移性。

| 文献 | 要点 | 与本项目的界线 |
|---|---|---|
| **AdaptiveISP, NeurIPS 2024** | RL 把 ISP 配置建成 MDP，逐图选模块（离散）+ 预测参数（连续）；奖励 = 冻结 YOLOv3 检测误差下降；`1ms/stage`，最优 3–5 级。 | ISP 是**可微软件 ISP**，梯度经冻结检测器回传；**非硬件**、且目标是检测非感知质量 |
| **DynamicISP, ICCV 2023 / PQDynamicISP, arXiv 2403.10091** | 按上一帧识别/质量结果，逐帧平滑控制 ISP 参数；PQDynamicISP 面向**任意 sensor 的感知质量**。 | 参数控制思路可借鉴，但同样建立在软件可微/近似 ISP 上 |
| **ReconfigISP** | 为特定任务优化 ISP 管线与参数，但推理期**固定**（非逐图自适应）。 | 对比基线，说明"逐图自适应"的价值 |

## 3. 对本项目的方法结论

映射到 SS928 闭源硬件 + RTX 4060 训练 + 板端吞吐受限 + 竞赛周期，**不押单条，按风险与已有资产分层**：

| 阶段 | 采用 | 硬件角色 | 依据文献 |
|---|---|---|---|
| **门控** | 先跑保真度闸门；NN 控制范围收窄到高对齐模块（Gamma / DRC-tone / LDCI） | 仅**离线验证** | `semantic_gap.md` + 标定/调优分离（原厂《ISP 图像调优指南》） |
| **主路径** | **路线 B'（蒸馏）**：离线逐场景黑盒优化产 `θ*`，参数空间回归训 ParamNet | **离线**产标签 | Mosleh 2020；2407.17530 |
| **提精** | **路线 C（残差代理）**：只学"解析模拟器 → 硬件"的残差，复用已建的 `models/isp_simulator/` | **离线**采少量 `(x,θ)→out` | 2407.17530；MDPI 2025 两阶段代理 |
| **可选 polish** | 硬件在环**微调**：SPSA/0 阶只需每步 ~2 次硬件查询，精修 sim 预训练网络 | 少量在环查询 | Mosleh 2020 的 0 阶思路 |

三条主路径的共同点：**硬件永远在离线数据生成这一步，绝不进 SGD 内层环。**

结论要点：
1. "硬件进不了训练环"是**已知且已解决**的问题，无成熟方案把硬件塞进梯度内层环。
2. 主流手段是把硬件请到**离线**：产 `(x,θ)→out` 训可微代理（Tseng 2019），或产 `scene→θ*` 蒸馏成
   网络（最省心），或只学解析模拟器到硬件的**残差**（复用现有代码、样本最省）。
3. **不该假设代理一定优于直接硬件优化**——Mosleh 2020 反例在先；代理的价值是可摊销、可反传，不是精度上限。
4. 软件可微 ISP 类工作（AdaptiveISP 等）**不面对本项目的闭源硬件 gap**，其可迁移性结论需打折。
5. 标定类参数（黑电平/NR/Shading/AWB/CCM）是实验室物理标定、与场景无关，**不进 NN 逐帧预测**。

## 4. 执行路线评估：可微代理预训练 + 硬件微调

> 提案（2026-07-02）：先在可微代理上预训练 ParamNet，得到初步权重后放到硬件上 fine-tune。
> 裁决：**可行，是正确骨架**；与 §3 分层路线同构（sim-to-real 预训练 + HIL polish 的经典范式）。
> 失败方式不会是"思路错了"，而是三个衔接细节。本节记录问题与对策。

### 4.1 为什么骨架成立

- 预训练解决**冷启动**：随机初始化直接上硬件微调 ≈ 从零训练（1e5 次查询，不可行）；预训练后网络已有
  "场景→参数方向"先验，硬件阶段只做局部修正，查询量降 2–3 个数量级。
- 微调解决**最后一公里 gap**：代理再准也有残差，最终精度只能在真实 ISP 上收口（Mosleh 2020 的
  18% mAP 增益正来自这段）。

### 4.2 三个核心问题与对策（按严重度排序）

#### 问题 1：硬件端没有 Ground Truth——微调监督信号必须换，且有目标漂移风险

预训练 loss 是 `sim(x,θ) vs 数据集GT`（full-reference）；板端真实场景**不存在配对参考图**，微调
信号只能是无参考质量指标（NR-IQA）或蒸馏标签。两个目标不一致 → 网络可能"刷指标"（过饱和、
过对比的 metric gaming）。

对策：
1. **NR-IQA 先验证再使用**：候选指标（高光裁剪率、暗部熵、曝光均匀度、局部对比度、colorfulness、
   噪声度）组合加权，在 10–20 组人工判过好坏的 A/B 图像对上验证与人评的 Spearman 秩相关（目标
   >0.8）再上岗；与 [`quality-acceptance.md`](quality-acceptance.md) 的验收口径统一。
2. **锚定正则**：微调 loss = NR-IQA + λ‖θ − θ_pretrain(x)‖²，信任域限制离预训练输出不能太远，λ 退火。
3. **优先蒸馏信号而非 SPSA 端到端**：逐场景黑盒优化产 `θ*`，再回归微调。关键收益——把"指标被
   钻空子"的风险从在线训练**转移到离线可人审的阶段**（每个场景的 θ* 效果图可以人工过目再进训练集）。
4. 验收双轨：留出场景 NR-IQA + 人评，防指标单点失效。

#### 问题 2：预训练可能落入错误盆地，微调（局部修正）救不回来

微调只能在预训练解的邻域内修。若代理 gap 大（WDR ★1、Dehaze ★2 等低对齐模块，见
[`semantic_gap.md`](../models/isp_simulator/semantic_gap.md)），预训练联合最优点与硬件最优点
不在同一盆地，少量硬件查询救不回。"可微代理"具体指什么至关重要：

| 预训练环境 | gap | 微调能否收口 |
|---|---|---|
| 未校准解析模拟器（现状 `models/isp_simulator/`） | 未知且可能大 | 低对齐模块大概率不能 |
| 解析模拟器 + 硬件残差校准 | 小 | 大概率能 |
| 从硬件数据学的 CNN 代理（Tseng 式） | 小、可迭代精修 | 能 |

对策：
1. **预训练前先校准代理**（一次板端时间换掉大半盆地风险）：收窄后的参数子空间（Gamma 曲线用
   ~5 控制点参数化 + DRC-tone 6 + LDCI 8 ≈ **~19 维有效自由度**）内，围绕 vendor-auto 工作点
   拉丁超立方采 `500–2000` 组，固定场景施加并抓帧，训残差 `R(x,θ)`（小 U-Net，输入 = sim 输出
   + θ 广播通道）或仅校准各模块标量响应曲线。
2. **校准验收判据用秩相关而非 PSNR**：留出参数组上"硬件质量排序 vs 校准代理质量排序"
   Spearman >0.9（训练需要的是排序对、不是逐像素对）。
3. **迭代精修**（Tseng 的核心 trick）：预训练收敛后，把 ParamNet 实际输出的 θ 分布拿到硬件补采、
   并入校准集精修代理，再续训 1–2 轮——把采样预算花在优化轨迹经过的区域。
4. 维持既定收窄：**WDR/Dehaze/DRC local-mixing 不进 NN 输出空间**，微调只需修小 gap。

#### 问题 3：板端微调输入分布不可控——摄像头前只有一个真实世界

微调输入 = 实际能摆出的场景（当前 7 类场景尚待采集授权）。若只覆盖 2–3 类，网络会过拟合并
**灾难性遗忘**预训练学到的其他场景行为。

对策（按杠杆从大到小）：
1. **RAW 回灌**：✅ **已查证支持（2026-07-02，线性模式）**，见下方查证结论。固定 20–50 张 RAW
   场景库反复回灌，微调输入完全可控、可复现、可夜间自动跑——本方案可控性的最大 enabler。
   板端 harness 已实现并冒烟通过（2026-07-03）：`board/tests/test_raw_replay.c`——定格一帧 RAW ×
   θ-sweep 确定性重放成立，DRC 强度扫描方向正确（暗部 +11.8 / 亮部 −2.9 @drc1023 vs 基线）；
   用法与冒烟数据见 `board/README.md`。
2. **冻结 backbone、只微调输出 head**（几千参数）+ L2-SP 正则拉住预训练权重——小数据微调标准配方。
3. **Rehearsal**：微调 batch 混入一半代理上的预训练样本，防遗忘。

##### RAW 回灌查证结论（2026-07-02）

来源：《ISP 开发参考》§2.2.4、《MPP 媒体处理软件 V5.0 开发参考》§3.4.32–3.4.35 / §3.4.48 / §3.4.57。

**官方回灌闭环全链路存在**，《ISP 开发参考》附有伪代码，单帧同步循环为：

```c
ss_mpi_vi_set_pipe_frame_source(pipe, OT_VI_PIPE_FRAME_SOURCE_USER);  /* 一次性切数据源 */
loop {
    ss_mpi_isp_run_once(vi_pipe);                       /* 驱动 ISP firmware 处理一帧 */
    ss_mpi_vi_send_pipe_raw(vi_pipe, frame_info, n, ms); /* 送 RAW（真实 VB） */
    ss_mpi_vi_get_chn_frame(vi_pipe, chn, &yuv, ms);     /* 直接取 ISP 输出 YUV */
    ss_mpi_vi_release_chn_frame(...);
}
```

采集侧同样具备：`ss_mpi_vi_set_pipe_frame_dump_attr`（enable + depth≤8）+
`ss_mpi_vi_get_pipe_frame` / `ss_mpi_vi_release_pipe_frame` 可 dump 物理 PIPE 的 RAW 帧，
用于建 RAW 场景库。

对本方案的三个利好：
- 逐帧同步（"上一帧处理完才能送下一帧"），**确定性回放**，同一 RAW 可在不同参数下重复施加；
- 输出经 `ss_mpi_vi_get_chn_frame` 直接取 YUV，**绕过 VENC/RTSP，无编码损失**，比抓流方案质量口径更干净；
- AE 与 sensor 解耦（数据不来自 sensor），配合 AE 手动/锁定可把变量隔离在被校参数上。

必须满足的约束（均已从 API 注意事项核实）：

| 约束 | 影响 |
|---|---|
| **VI 必须离线模式**（"VI在线VPSS在线、VI在线VPSS离线不支持"送 RAW） | 生产配置为 `OT_VI_ONLINE_VPSS_OFFLINE`（`board/src/capture.c`），校准会话需以 `OT_VI_OFFLINE_VPSS_OFFLINE` 启动——做成**专用校准模式配置**，与生产配置二选一启动，不共存 |
| ⚠️ **`ss_mpi_isp_run_once` 不支持帧合成 WDR 模式** | 回灌校准仅覆盖**线性模式**。与既定收窄一致（WDR 不进 NN，由 AE 规则管理）；残余风险：WDR 模式下 DRC 行为不同，WDR 场景联合效果退回真实场景 A/B 验证 |
| `run_once` 必须与 `ss_mpi_isp_init`/3A register 同进程、单进程 | 校准 harness 写成独立板端测试程序（`board/tests` 惯例 `test_<名>`），不与生产 `socchina_app` 同时跑 |
| RAW 格式：linear、SDR8、compress NONE/LINE/FRAME、与 PIPE 属性一致、真实 VB | dump 与回灌用同一 PIPE 属性配置即可天然满足 |

### 4.3 次级问题（提前知道即可）

- **域差距（施加点不同）**：数据集是他机 sRGB 成品图，模拟器在其上施加是"二次处理"；硬件 ISP 在
  RAW/linear 域施加。预训练输入至少做逆 gamma 到伪 linear（更彻底：unprocessing 到伪 RAW，
  Brooks et al. CVPR 2019）。
- **闭环稳定性**：板端 ParamNet 的输入是"当前参数处理后的 ISP 输出"，预测新参数后输入又变，
  存在反馈回路。对策：训练时随机化输入的"当前参数"状态；板端复用 CLUT 桥已验证的逐次步长
  护栏 + 反馈抑制经验（DynamicISP 的"按上一帧结果平滑控制"同理）。
- **吞吐预算**：单次硬件评估 `0.2–0.5s`。SPSA 端到端 1e3 步 ≈ 10–17 分钟纯查询、加场景摆放按天计；
  蒸馏式每场景搜出的 θ* 可复用训练多个 epoch，硬件时间利用率高得多——SPSA 只做最后可选 polish。

### 4.4 修正后的四段执行序

| 阶段 | 内容 | 硬件角色 | 通过判据 |
|---|---|---|---|
| 0 保真度闸门 | 确认 NN 控制范围收窄到 Gamma/DRC-tone/LDCI | 离线验证 | 各模块硬件 vs 模拟器质量**秩相关** |
| 1 代理校准 | 残差校准解析模拟器；RAW 回灌已查证支持（线性模式，见 §4.2 问题 3） | 离线采 `(x,θ)→out` 500–2000 组 | 留出参数组秩相关 >0.9 |
| 2 预训练 | 校准代理 + 配对数据集 + full-reference loss（伪 linear 域） | 不参与 | 数据集 val 指标收敛 |
| 3 硬件微调 | 蒸馏信号（黑盒 θ* 回归）微调 head；冻结 backbone + rehearsal；SPSA 可选 polish | 离线产 θ* 标签 + 少量在环查询 | 留出场景 NR-IQA + 人评双轨优于 vendor-auto |

## 参考文献

- Tseng et al. 2019, *Hyperparameter Optimization in Black-box Image Processing using Differentiable Proxies*, ACM TOG 38(4). https://light.princeton.edu/publication/proxy_opt/
- Mosleh et al. 2020, *Hardware-in-the-Loop End-to-End Optimization of Camera Image Processing Pipelines*, CVPR. https://light.princeton.edu/publication/hil_image_optimization/ ，代码 https://github.com/princeton-computational-imaging/LITL-Optimization
- Qin et al. 2022, *Attention-aware Learning for Hyperparameter Prediction in Image Processing Pipelines*, ECCV. https://www.ecva.net/papers/eccv_2022/papers_ECCV/papers/136790265.pdf
- 2024, *Learning Instance-Specific Parameters of Black-Box Models Using Differentiable Surrogates*, arXiv:2407.17530. https://arxiv.org/abs/2407.17530
- 2025, *Conditional GAN-Based Two-Stage ISP Tuning Method: A Reconstruction–Enhancement Proxy Framework*, Applied Sciences 15(6):3371. https://doi.org/10.3390/app15063371
- AdaptiveISP 2024, NeurIPS, arXiv:2410.22939. https://openimaginglab.github.io/AdaptiveISP/
- DynamicISP 2023 (ICCV) / PQDynamicISP, arXiv:2403.10091. https://arxiv.org/abs/2403.10091
- Brooks et al. 2019, *Unprocessing Images for Learned Raw Denoising*, CVPR（sRGB→伪 RAW 逆变换，用于域差距对策）. https://arxiv.org/abs/1811.11127
