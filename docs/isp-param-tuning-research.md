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

## 5. 保真度闸门首轮结果（2026-07-03，阶段 0 完成）

**目标**：实测裁决"解析模拟器的参数排序是否与硬件 ISP 一致"（§4.4 阶段 0），单场景（室内静物、
强逆光窗口）、收窄模块（DRC tone / LDCI）。

**命令路径**：`python -m models.isp_simulator.fidelity_gate gen`（16 组参数 blob：中性帧 +
tone 5 档 + LDCI 4 档 + strength 3 档 + mix 2 档 + 组合）→ 板端
`test_raw_replay --settle 8 --blob ...`（17 项 84 秒内完成）→ `fidelity_gate analyze`
（帧级特征 + Spearman 秩相关）。

**结果**：

| 判据 | 数值 | 判定 |
|---|---|---|
| DRC tone 组内秩相关（shadow，n=5） | **+1.000** | ✅ |
| LDCI 组内秩相关（shadow，n=4） | **+1.000** | ✅ |
| 方向一致率（全特征，vs 中性帧） | 100%（highlight 75%） | ✅ |
| 全体混合秩相关（shadow） | +0.476 | 参考（见缺口 1） |

**闸门判定：PASS**——模拟器在单模块维度内的参数排序与硬件完全一致，蒸馏/代理校准路线可行。

**解读与已识别缺口**（Phase 2 开工前须知）：

1. **DRC manual strength 未建模，是跨模块幅度错位的根因**（全体混合 rho 低的唯一主因）。
   SDK 明示"值越大整体图像越亮"[0,1023]；实测响应（恒等 tone，相对中性帧 shadow 0.155）：
   `s=128→0.211，512→0.388，896→0.578`，超线性、比任何 tone 曲线提升都强。
   **行动**：模拟器加 strength 维度（以实测响应拟合 gain 曲线），或蒸馏 θ 直接包含 strength。
2. **参数切换需 ≥8 帧 settle**（ISP 内部时域滤波）：settle=2 时每组首项被前组状态污染
   （v1 的 t1、v2 的 l1/m1 异常均由此）。校准采集纪律：`--settle 8`。
3. **highlight 方向一致率 75%**：模拟器高光行为有偏差，残差校准（阶段 1）重点。
4. **local mixing 对帧级亮度特征不敏感**（m1=m2）：评估该模块需局部梯度/细节特征，暂不纳入判据。
5. 首轮为单场景；结论外推前应在 3–5 个场景重复（快：每场景 ~2 分钟采集）。

发现过程记录（防重蹈）：v1 闸门 FAIL 的直接原因是板端 blob 加载器把 strength 写死 512，
LDCI 组净效应（±0.015）被 DRC strength 效果（+0.24）淹没——blob v2 增加 strength 字段
（板端 `isp.c` 兼容 v1 默认 512），LDCI 组以 strength=0 隔离后组内 rho 从 −0.2 → +1.0。

### 5.1 strength 建模与多曝光复测（2026-07-03，缺口 1/5 关闭）

**strength 已建模**：参数向量扩至 97 维（`drc_strength`），模拟器以幂函数近似
`y = x^exp(-1.4·s/1023)`（`drc.drc_strength_apply`，常数 1.4 由 s 组实测三点拟合，误差 <2%）。
同一批板端数据离线复验（blob bit 一致，零板端成本）：**全体混合 shadow rho +0.476 → +0.925**，
mean +0.936，std +0.904；strength 组绝对值误差 <4%。

**多曝光条件复测**（同场景，手动曝光两档 + 原 AE 档）：

| 条件 | 中性 shadow | tone 组内 | LDCI 组内 | strength 组内 | 全体混合 shadow |
|---|---|---|---|---|---|
| AE 正常 | 0.155 | +1.000 | +1.000 | +1.000 | +0.925 |
| 亮（16ms/2x） | 0.119 | +1.000 | +1.000 | +1.000 | +0.921 |
| 暗（2ms/1x） | 0.007 | +1.000 | +1.000 | +1.000 | +0.371 |

三条件主判据全部 PASS（组内 rho=+1.000 ×3 组，方向一致率 100%）。**新增已知边界**：
极暗输入（shadow<0.05）下 strength 幂函数外推低估（s3：hw 0.304 vs sim 0.173——硬件对极暗区
有超出幂函数的额外提升），是标定域（shadow≈0.155）外推 20 倍所致；混合 rho 0.371 全部由此贡献。
**行动**：残差校准（阶段 1）覆盖极暗输入域，或蒸馏时该区间靠硬件标签兜底。

采集数据归档：`models/weights/fidelity/board_scene1{,_dark,_bright}/`（不入库）。

### 5.2 合成 RAW 多场景外推（2026-07-03，缺口 5/5 关闭，不动相机）

**方法**：闸门测的是排序保真，输入的关键属性是直方图/局部结构分布而非语义——用合成 RAW
文件回灌替代物理换景，网格化覆盖直方图空间。基础设施：

- 板端 `test_raw_replay` 新增 `--compress-none`（pipe RAW 切裸 12bpp packed bayer）与
  `--raw-file`（≤8 个文件，公共 VB 池构造帧回灌，字段填法照厂商
  `sample_comm_vi_malloc_frame_blk`）；
- 主机 `synth_raw.py`：`parse` 自动判定 12bpp packed 布局（实测 **lsb**：连续
  little-endian 位流，b0=p0[7:0]，b1=p1[3:0]<<4|p0[11:8]，b2=p1[11:4]；预览图目检验证）
  并报告黑电平；`gen` 生成 8 个受控直方图场景（平坦暗/中、渐变、逆光双峰、中调纹理、
  暗纹理、渐变亮斑、低照噪声），灰度 bayer 保 AWB 中性。

**结果**（8 场景 × 16 参数 × settle 8，板端一次会话 ~3 分钟）：

| 场景 | tone | LDCI | strength | 混合 shadow rho |
|---|---|---|---|---|
| flat_dark / flat_mid / hgrad / backlit / texture_mid / vgrad_spots / lowlight_noise | +1.000 | +1.000 | +1.000 | 0.93–1.00 |
| **dark_texture** | +1.000 | **−1.000** | +1.000 | +0.857 |

24 个组内秩相关 23 个 +1.000。**多场景外推坐实：模拟器排序保真不依赖场景内容，
物理换景对闸门非必需。**

**新发现（残差目标 #2）**：dark_texture（窄直方图暗纹理）上 LDCI 完美反序——硬件
he_pos_wgt 单调提亮暗部（0.243→0.318，与所有其他场景一致），**模拟器 CLAHE 近似却把
暗区压暗**（0.199→0.131：对比度拉伸的暗侧下压行为，硬件的正向高斯加权没有）。
定位在 `ldci.py` 的 CLAHE 近似；蒸馏路线（硬件标签）天然免疫，残差校准阶段修正。

### 5.3 阶段 1 起步：批量校准数据集与残差基线（2026-07-03）

**基础设施**：`test_raw_replay --blob-dir`（单会话 ≤511 组参数，按名序）+
`calib_dataset.py`（LHS 采样 29 有效维、tone cummax 保单调、偏提亮范围 → blob 目录；
`report` 输出残差量化）。

**v1 数据集**：128 组 LHS 参数 × 8 合成场景 = **1024 对 (中性帧, θ) → 硬件输出**，
板端单会话 ~7 分钟全部采齐（512x288 输出），归档 `models/weights/calib/`（不入库）。

**残差基线**（sim(中性帧,θ) vs 硬件输出）：

| 指标 | 数值 |
|---|---|
| PSNR 中位 / p5 / p95 | **16.0 / 10.1 / 22.0 dB** |
| \|Δluma\| 中位 | 0.09–0.12（暗输入场景更差） |
| 标量 γ 校正上限 | 仅 +1.3 dB（16.3→17.7），拟合 γ 分布宽（0.21–1.37） |

**解读**：排序保真（§5.1/5.2）与绝对保真的差距已量化——sim 直接当训练环境的绝对误差
约 16 dB 量级。残差**不是低维全局项**（标量 γ 吃不掉），且最差参数跨场景一致
（c067/c116 反复出现）→ 残差主要**由参数决定**而非场景内容。结论：残差网络
`R(sim输出, θ)` 以参数条件化为主轴、空间/色调依赖为辅，v1 数据集已够第一版训练。
下一步：训残差网络，验收判据 = 留出参数组校正后 PSNR 中位 >25 dB。

### 5.4 blob v3：官方参数对账补缺与 Gamma 闭环（2026-07-03）

**对账结论**（vs SDK `ot_common_isp.h` 官方完整定义）：LDCI 稳态参数已全覆盖
（`tpr_incr/decr_coef` 时域项固定默认，flicker 验收时复查）；DRC 官方 ~50 字段中
v2 只覆盖核心 12 组，**三组缺口与项目"双向"目标直接相关**：
① `bright_gain_limit(+step)`——高光抑制的官方直控（v2 无任何高光侧参数）；
② `dark_gain_limit_luma/chroma`——提亮护栏；
③ `color_correction_lut/ctrl + low/high_saturation_color_ctrl`——DRC 提亮后饱和度补偿
（调优指南明示）。另发现 v2 只写 FilterX 通路 mixing，漏了 Filter 主通路
`local_mixing_bright/dark[33]`。

**blob v3**（板端 reader 兼容 v1/v2）：DRC 段追加主通路 mixing（与 X 通路同值）；
新增可选子段 GUARD(bit3)/COLOR(bit4)（护栏与色彩作为固定 preset/规则参数，
**不进 NN 输出维度**，避免蒸馏标签空间膨胀）；新增 GAMMA 段(bit2)：64 节点 u16 曲线 +
strength，经 `isp_gamma_apply_curve` 施加，所有 blob 携带该段（非 gamma 项 strength=0
显式还原默认，防状态泄漏）。

**Gamma 闭环验证**（8 合成场景 × 提亮曲线 3 档梯度）：**8/8 场景 Gamma 组内
rho=+1.000**——★5 对齐模块坐实。至此 NN 核心输出维度（DRC tone / strength / LDCI /
Gamma）全部处于已验证的"生成→施加→采集→对比"闭环内。m 组在主通路写入后出现微小
非零差异（v2 完全相同），确认 local mixing 是帧级亮度不敏感的细节参数（评估需局部
特征）；dark_texture 的 LDCI 反序一致复现（残差目标 #2 不变）。

### 5.5 残差校准网络 v1：验收 PASS（2026-07-03，阶段 1 核心判据达成）

**架构**（`models/isp_simulator/residual_net.py`）：FiLM 条件化小 CNN，**92.6K 参数**——
θ（31 维核心参数 = 参数向量 [65:96]）经 MLP 生成逐块 scale/shift，调制 4 层卷积骨干
（通道 32 + GroupNorm）；输出头 zero-init（初始即恒等，从 16 dB 基线起步）；
校正输出 = `clamp(sim + Δ)`。

**训练**：v1 校准集（8 合成场景 × 128 LHS 参数，512×288），按**参数组**划分
train 103 / val 25；L1 loss，AdamW 1e-3 + cosine，120 epochs（RTX 4060 约 40 分钟，
含 sim 预计算）。曲线平滑收敛、无过拟合（val 后期稳定 27+）。

**验收**（留出参数组 200 对，网络未见过的 θ）：

| 指标 | 未校正 sim | **sim + R（校准代理）** |
|---|---|---|
| PSNR 中位 | 15.49 dB | **27.39 dB（+11.9）** |
| PSNR p5 | 9.06 dB | **20.87 dB** |

**判据（校正后中位 >25 dB）：PASS。** 印证 §5.3 的判断——残差主要由 θ 决定、可被小
容量参数条件化网络学习。**"校准代理"路线成立**：`sim + R` 已是绝对保真 27 dB 量级的
可微硬件代理，阶段 2（ParamNet 预训练）的训练环境就绪。

v1 已知边界（进阶段 2 前评估是否需要扩）：
1. 数据域 = 8 个合成灰度场景（真实彩色场景的色彩残差未覆盖——CC/AWB 交互）；
2. θ 只含 blob DRC/LDCI 31 维（Gamma 已有硬件闭环但未纳入 θ 与采集，calib v2 应加）；
3. 分辨率 512×288（参数效应为全局性，风险低）；
4. dark_texture 的 LDCI 反序属训练分布内（网络可学），但机制性修正 `ldci.py` 仍值得做。

### 5.6 ParamNet 阶段 2 首轮预训练（2026-07-03）

**设计**（`models/isp_simulator/paramnet.py`）：输入 256×144 RGB；backbone 全卷积
stride-2 ×5 + 固定核 AvgPool + 1×1 头（NPU 红名单安全，无 Pow/Cast/ReduceMean），
**352.5K 参数**（<500K 约束内）。核心设计：网络输出 29 维 `u∈[0,1]`，经与 calib LHS
**逐段相同的可微映射** `u_to_theta` 转成 97 维 θ——**保证输出天然落在校准代理的有效域
内**（代理在域外无保真承诺，域一致性由结构保证而非正则）。

**训练**：LCDP 双向曝光校正数据集（train 1415 / valid 100，仓库 artifacts 内已有）；
冻结校准代理（sim + ResidualNet 27.4dB），loss = L1(代理输出, GT)；AdamW 3e-4 +
cosine 24 epochs。已知妥协：drc_ctrl 3 维 sim 主通路梯度断裂（滤波半径本质离散），
仅剩 ∂R/∂θ_ctrl 旁路信号——细调参数，最坏仅细调次优，阶段 3 蒸馏纠偏（详见
paramnet.py 头注释）。

**结果**（LCDP valid 100，512×288，PSNR vs GT）：

| | PSNR 中位 | p5 |
|---|---|---|
| 不处理 baseline | 15.32 | 10.10 |
| **ParamNet + 校准代理** | **20.60（+5.3）** | **15.42** |

**解读**：
- 曲线 ep8 后进入平台（20.0–20.6）——瓶颈不是容量/信号，而是 **31 维全局参数的表达力
  上限**（全局参数无法修正空间变化的曝光误差），符合路线预期；
- 参照：CTBG v9（逐像素 NN）收敛上限 val_psnr 19.83——ParamNet 用全局参数达到
  **20.60**（口径不严格相同：split/分辨率有差异，量级可比）。**参数化路线在画质上
  不输逐像素路线**，且天然 30fps 全分辨率施加、无 NPU 逐像素搬运；
- 该 PSNR 口径经过校准代理（≈硬件行为），即预测的参数上板后应有相近效果——待阶段 3
  真实 A/B 坐实。

下一步（按杠杆）：① 板端 A/B 冒烟——ParamNet 对真实场景中性帧出 θ→blob→回灌，与
vendor-auto 对比（打通阶段 3 最短路径）；② calib v2 纳入 Gamma + 真实场景域；
③ ONNX→ATC→OM 导出与算子探测。

### 5.7 阶段 3 首个板端 A/B 冒烟：NN→θ→硬件全链打通（2026-07-03）

**流程**：会话 A 采真实场景（白天室内桌面）RAW（compress NONE 定格）+ vendor-auto/
中性帧输出 → 主机 `paramnet infer`（中性帧 → θ → blob，输出 tone=[0,.33,.51,.63,.70,1]
暗部提升+亮部压制的 S 形，strength=0.17，LDCI=0）→ 会话 B **同一 RAW 文件回灌**施加。

**结果**（1024×576，中性帧暗/亮 mask）：

| | mean | shadow | highlight | 双端裁剪 |
|---|---|---|---|---|
| vendor-auto | 0.504 | 0.188 | 0.684 | 0% |
| 中性（关 DRC/LDCI） | 0.506 | 0.189 | 0.688 | 0% |
| **ParamNet θ** | 0.551 | **0.256(+35%)** | 0.709 | **0%** |

目检：暗部（屏幕边框/线缆/阴影）明显更通透，高光（木纹/白墙）未过曝。

**解读**：NN 出参 → blob → 硬件 ISP 施加的全链**语义正确**——暗部大幅提亮、高光受控、
零裁剪，符合曝光校正预期。限定：单场景、白天均匀光照（非考验场景，vendor-auto 本无
明显问题），本轮目的是链路打通而非画质裁决；夜间/强逆光的 vendor-auto 对比与多场景
验收待现场条件。此外该链为离线回灌口径；实时闭环（chn2 缩略图→NPU→热刷新）待
Phase 3 板端集成。

### 5.8 LCDP 双向数据集板端回灌演示 + sRGB→sensor 域 RAW 配方（2026-07-04）

**目标**：公开数据集图像（2 欠曝 + 2 过曝）经合成 RAW 回灌真实 ISP，ParamNet
（从各自板端中性帧 infer θ）施加，直观展示双向校正效果。

**攻克的三个域转换问题**（配方沉淀为 `synth_raw.rgb_to_sensor_raw()`）：
1. **bayer 相序 = BGGR**（`sample_comm_isp.c` OS08A20 pub_attr 实配）——按 RGGB 合成
   导致全图品红（G 被 demosaic 采到 R/B 位）；
2. **逆 AWB**：ISP 的 AWB/CCM 按真实 sensor 色彩响应标定（G 强 R/B 弱），"已平衡"的
   合成 RAW 会被再拉一次（灰输入实测输出 R/G=1.77、B/G=1.85）——合成时信号预除；
3. **黑电平 = 256**：8 档条带 RAW 一次回灌定位 ISP BLC 减除点（RAW≤256 输出全零）；
   合成基准若高于 BLC，残留 DC 三通道相同、经 AWB R/B 增益放大 → 暗部恒定紫偏。
   修正后板端中性帧对原图的亮度复现达 0.042 vs 0.047。

**效果**（各自中性帧 infer；ParamNet 对欠曝出强提亮 θ、过曝出近恒等 θ，方向 4/4 正确）：

| | input | 板端中性 | ParamNet | GT |
|---|---|---|---|---|
| under2（欠曝山景） | 0.094 | 0.071 | **0.150** | 0.207 |
| over2（过曝狗） | 0.733 | 0.678 | **0.640** | 0.702 |

目检：山景绿色鲜明、砖墙纹理浮现；过曝图高光压回、细节保持；色彩自然。

**暴露的真实限制**（与已知边界吻合）：极暗图（under1，输入 0.047）提升不足
（0.042→0.055，GT 0.149）——极暗域 strength 残差外推（§5.5 边界）+ 代理灰度域对
彩色输入的外推共同作用；欠曝提亮量普遍略低于 GT。**行动**：calib v2 纳入彩色/极暗
域采集后重训 R 与 ParamNet。

附加产出：`test_raw_replay` 文件模式支持 `--exptime/--again` 锁 AE（防 AE 数字增益
改写回放亮度）；sRGB→RAW 配方使**任意公开数据集可直接在真实 ISP 上做蒸馏/评估**——
阶段 3 蒸馏的数据通路就绪。

### 5.9 硬件 θ\* 上限诊断：差距归因定量（2026-07-04）

**问题**：ParamNet 板端输出与 LCDP GT 差距较大（§5.8），差距是"网络/代理可修"还是
"参数空间结构性上限"？**方法**：`hw_search.py` 两轮批处理黑盒搜索（64 宽范围候选 →
top 邻域精化，目标 = 板端输出 vs GT 的 PSNR），在真实 ISP 上逐图搜 θ\*——θ\* 的效果
即当前 31 维参数空间的**硬件真实上限**。单轮 4 图 × 65 候选约 100 秒。

**归因表**（PSNR vs GT，512×288）：

| 图 | input | ParamNet | 硬件 θ\* 上限 | 归因 |
|---|---|---|---|---|
| under1（极暗） | 17.07 | 18.08 | 18.30 | **结构性**——网络已达上限（差 0.2dB）；θ\* luma 0.060 vs GT 0.149，参数空间本身到不了 |
| under2 | 16.55 | **19.45** | 19.35 | **结构性**——网络超过搜索上限 |
| over1 | 15.39 | 16.94 | **20.98** | **可修 4.0dB**（网络/代理） |
| over2 | 22.46 | 21.64 | **25.23** | **可修 3.6dB**，且网络比 input 还差（压过头） |

**结论与行动**：
1. **欠曝方向：ParamNet 已把当前参数空间用满**，与 GT 的差距是结构性上限——但注意
   **搜索空间同样未含 Gamma**，"31 维上限"≠"全参数上限"。行动：**θ 纳入 Gamma**
   （最强全局亮度杠杆，闭环已验证），欠曝上限有望显著抬高；
2. **过曝方向：3.5–4dB 可修空间**，根因明确——calib LHS 采样偏提亮
   （tone delta ∈[-0.10,+0.30]），压暗参数域校准覆盖不足 → 代理在该域失真 →
   网络学坏。行动：**calib v2 采样对称化**（±0.30）；
3. calib v2 综合清单（全部有定量归因支持）：对称 tone + 纳入 Gamma + 彩色真实内容
   场景（`rgb_to_sensor_raw`）+ 极暗档；重训 R 与 ParamNet；
4. `hw_search.py` 即阶段 3 蒸馏标签生成的原型（θ\* 就是硬件标签），已转正入库。

### 5.10 命题裁决："ISP 能否完成 Zero-DCE 类任务"（2026-07-04）

**问题**：§5.9 的结构性上限是否意味着"ISP 参数做不了 Zero-DCE 那样的增强"？
**方法**：把参照系从人工 GT 换成 **Zero-DCE-Lite 输出本身**（板端同款 iter8 模型，
输入 = 同一板端中性帧），用 `hw_search.py` 搜 θ\* 逼近（复用已有 128 候选离线重评分
+ 一轮定向精化，板端 ~100 秒）。

**结果**（ISP θ\* 复现 Zero-DCE 输出的 PSNR）：

| 图 | ISP 逼近 Zero-DCE | 对照：两者各自逼近人工 GT |
|---|---|---|
| under1 | **29.7 dB** | 18.3 / — |
| under2 | **27.4 dB** | 19.4 / — |
| over1 | 21.3 dB | 21.0 / — |
| over2 | 20.8 dB | 25.2 / — |

**裁决**：命题证伪。**欠曝增强（Zero-DCE 的主战场）ISP 参数复现到 27–30 dB
（目检不可区分）**——Zero-DCE 的有效成分（单调提亮曲线 + 低频空间调制）基本落在
DRC tone/strength + LDCI 的能力范围内。两点佐证：① under1 上 Zero-DCE 同样提不动
（0.042→0.052，与 θ\* 的 0.055 一致）——极暗恢复对谁都是信息问题，真正的解在
AE/WDR（本系统有、离线增强方法没有）；② over 组 Zero-DCE 单向提亮（过曝图也提亮，
行为本身错误），逼近意义有限。结论：**与人工 GT 的差距是"非语义方法 vs 人工局部修图"
的共同差距，不是 ISP 相对 Zero-DCE 的短板**；且 Zero-DCE Lite 板端 640×320 需 29.9ms
（全分辨率 30fps 不可达），ISP 参数路线在效果相当的前提下独享实时性。
对比图：`artifacts/lcdp-replay-demo/6_isp_vs_zerodce.png`。

### 5.11 Zero-DCE 原生数据域复验（2026-07-04）

**方法**：Zero-DCE-Lite 仓库自带 sample_images 取 4 张真实低光照片（car/street/
roadside/chairs，中性帧 luma 0.012–0.116，比 LCDP 更暗）走完整链路：
`rgb_to_sensor_raw` 回灌 → 中性帧 → {Zero-DCE-Lite 参照, ParamNet θ, θ\* 逼近}。

**结果**：

| 图 | θ\* 复现 Zero-DCE | 中性 luma | Zero-DCE luma | **ParamNet luma** |
|---|---|---|---|---|
| car | 29.6 dB | 0.012 | 0.028 | **0.111** |
| street | 23.5 dB | 0.052 | 0.108 | **0.275** |
| roadside | 28.1 dB | 0.022 | 0.060 | **0.200** |
| chairs | 24.0 dB | 0.116 | 0.167 | **0.270** |

1. **θ\* 复现 23.5–29.6 dB**：在 Zero-DCE 原生域再次确认 ISP 参数覆盖其表达力（§5.10 结论成立）；
2. **意外发现：ParamNet 的实际增强显著强于 Zero-DCE-Lite 本尊**——极暗输入上
   Zero-DCE-Lite（零参考损失）提亮保守（车库/唐人街/椅子仍暗），ParamNet（LCDP 配对
   监督）出图接近可用照片（街景招牌路面全亮、房间细节可辨）。ParamNet vs Zero-DCE 的
   PSNR 低（12.7–17.8）恰是因为它**做得更多**而非更差；
3. 诚实记录：极暗区强提亮暴露伪影（car 地面色块/banding、暗区色偏）——极暗域是
   压力测试口径（真实产品 AE 先行），也再次指向 calib v2 极暗档目标。
   对比图：`artifacts/lcdp-replay-demo/7_zerodce_domain.png`。

### 5.12 calib v2 首轮：宽域校准与 ParamNet v2（2026-07-04，进行中）

**v2 内容**（§5.9 归因驱动，全部落地）：采样对称化（tone ±0.30）+ θ 纳入 Gamma
（γ∈[0.45,1.55] 幂曲线，R 条件扩 95 维）+ 场景换真实内容（LCDP train 10 张，亮度谱
0.017–0.666 含 3 张极暗，`rgb_to_sensor_raw`）+ 灰度合成 2 张。板端 12 场景 × 162 项
分两批采集（发现并修复 `RAW_FILE_MAX=8` 上限 → 16）。

**首轮结果**：

| 项 | v1（窄域） | v2（宽域） |
|---|---|---|
| R 留出（各自域） | 15.5→27.4 dB | 15.8→**22.0 dB**（+6.1，曲线未饱和） |
| ParamNet 代理口径 | 20.60 | 20.47（θ 空间大得多） |
| 板端 4 图 vs GT | 18.1/19.5/16.9/21.6 | 17.9/18.7/**18.3(+1.4)**/20.2 |

**解读**：over1 兑现改善（朝 θ\* 上限 20.98 前进），但整体未达预期——**瓶颈是 R v2
保真度**：宽域（双向+Gamma+真实彩色+极暗）比 v1 窄域难，22 dB 的代理放大了训练-硬件
落差（under 组与 over2 轻微退步）。v2 的 θ 行为本身正确（over 组学会 tone 压暗 +
strength 补偿的组合，under 组强提亮）。另修 `paramnet infer` 未携带 gamma 段的 bug。

**R v2.1 训练冻结与断点续训（2026-07-05）**：48ch/6blk（参数 ×2.5）+ 240ep 重训，
但在 **ep160 后进程冻结**（GPU 0%、CPU 单核忙等、Rl 态干等 9.5h，环境/CUDA 挂起，
非收敛）。**冻结点 val 仍在稳步爬升**（ep100→160：21.22→21.98，每 20ep +0.3~0.4dB），
且此时 cosine LR 仍高（对比 v2.0 是 120/120ep LR 已退火到底），故"容量无用"的
早期结论**站不住**——已回退。
处理：`residual_net.py` 加 `--init`/`--resume` 与每 eval 原子保存 `last.pt`（防冻结丢
进度），从 ep160 权重续训至收敛后再裁决容量路线。
过程坑记录：48ch/6blk 全帧 batch16 在 8GB 显存 OOM（→batch 8）；训练进程会偶发
CPU 忙等冻结（原子 last.pt + resume 兜底）。

### 5.13 容量路线板端裁决：转蒸馏（2026-07-05，收敛数据）

R v2.1 断点续训至**真收敛**（48ch/6blk，val 曲线 ep75→120 走平）：留出 **22.37 dB**
（+0.4 over v2.0 的 22.0）。用它重训 ParamNet v2.1 并板端复验 4 图（vs GT）：

| 图 | input | v1 | v2 | v2.1 | θ\* 上限 |
|---|---|---|---|---|---|
| under1 | 17.07 | 18.08 | 17.85 | 18.03 | 18.30 |
| under2 | 16.55 | 19.45 | 18.67 | 18.71 | 19.35 |
| over1 | 15.39 | 16.94 | **18.34** | 18.27 | **20.98** |
| over2 | 22.46 | 21.64 | 20.24 | **18.44** | **25.23** |

**裁决（收敛 + 板端实测，非截断残局）**：
1. **+0.4dB 更强代理未转化为板端提升**：v2.1 ≈ v2（under/over1 差 <0.1dB）；
2. **over2 逐版恶化**（22.46→21.64→20.24→18.44）：对称采样修好 over1（v1→v2 +1.4dB）
   却副作用性地**过压只需微调的过曝图**（v2.1 strength 拉满 0.844）——代理训练的网络
   导航不了"该压 vs 不该压"的权衡；
3. **θ\* 上限缺口巨大**：over1 差 2.7dB、over2 差 **6.8dB**——θ\* 知道 over2 应近恒等
   （GT≈input），代理训练的 ParamNet 学不到。

**结论：代理保真度 ~22dB 是硬约束，继续加容量/数据边际收益为负甚至为负向。
转 θ\* 硬件标签蒸馏**——`scene→θ*` 参数空间回归绕开代理保真依赖，`hw_search.py`
已证 θ\* 在过曝方向优 2.7–6.8dB。这是本项目四段执行序的阶段 3 主路径。

### 5.14 阶段 3 蒸馏首轮：机制验证成功，需扩标签规模（2026-07-05）

**管线**（`distill.py`）：gen（LCDP train 选 15 图跨亮度桶 + 候选池 128 LHS(对称+γ)
+ 每图 ParamNet warmstart 保底）→ 板端单会话搜 θ*（15 图 × 143 候选，~15 分钟）→
labels（逐图 θ* vs GT）→ finetune（排练式）。

**θ* 标签质量**：平均比 ParamNet 高 **+1.8 dB**（img08 +5.3、img13 +3.65）；关键
img13 上 ParamNet(17.63) < input(18.59) 的**过压 bug 被 θ*(21.28) 纠正**——硬件标签
天然携带"别过度处理"知识（代理学不到的那部分）。

**微调**：纯回归 15 图**灾难性遗忘**（LCDP valid 20.4→14.9）；**排练式**（LCDP 代理
目标保泛化 + 硬件 θ* 拉修正）防住（20.4→**19.9**）。

**4 留出图板端验收**（vs proxy 路线终点 v2.1）：

| 图 | input | v2.1(proxy) | **蒸馏** | θ* 上限 |
|---|---|---|---|---|
| under1 | 17.07 | 18.03 | **18.50** | 18.30 |
| under2 | 16.55 | 18.71 | **18.79** | 19.35 |
| over1 | 15.39 | 18.27 | 17.42 | 20.98 |
| over2 | 22.46 | 18.44 | **20.72** | 25.23 |

**结论**：蒸馏机制**验证成功**——① 靶向的 over2 过压 bug 修复 **+2.28 dB**（18.44→20.72，
θ* 知识迁移到留出图）；② under 组 +0.5，蒸馏网学会启用 LDCI（此前恒 0）；③ 排练防住
遗忘。**但 15 标签偏少**：over1 回退 -0.85（该逆光场景类型未被 15 图覆盖），per-scene
噪声明显。**下一步：扩 θ* 标签规模**（数百图、多会话，~数小时板端），使增益跨场景一致。
对比图 `artifacts/lcdp-replay-demo/8_distill.png`。

### 5.15 LOLv1 跨方法 benchmark（2026-07-05）

用户要求：本路线 vs 开源权重模型（Zero-DCE-Lite、Retinexformer）在 LOLv1 test（15 对）
同输入对比。公平口径：所有方法处理同一 16:9 裁剪→512×288 输入，对同一 GT 算指标。
本路线：`sRGB→rgb_to_sensor_raw→板端回灌→distill ParamNet θ→ISP`。

| 方法 | PSNR | SSIM | PSNR*(GT均值对齐) |
|---|---|---|---|
| input（不处理） | 7.76 | 0.197 | 19.34 |
| Zero-DCE-Lite | 12.43 | 0.586 | 20.65 |
| **Retinexformer**（RGB 深网） | **22.58** | **0.863** | **26.81** |
| Ours(ParamNet 蒸馏) | 9.08 | 0.255 | 13.70 |

（Retinexformer 复现 22.58 ≈ 官方 22.97，验证 benchmark 口径正确。）

**诚实诊断——本路线的低分是方法学 artifact，非 ISP 增强能力**：
- LOLv1 输入是极暗 8-bit sRGB（输入→GT 仅 7.76dB），需 **7.5x 提亮**（luma 0.060→0.455）；
- 本路线部署路径是 RAW 域：`rgb_to_sensor_raw` 把暗 sRGB（0.06）经 ^2.2 压到 ~0.002，
  映射进 12-bit `[BLC=256,3700]` 后仅高于黑电平 **~6 个量化级**——阴影真实信号几乎全丢；
  ISP 增益放大的是量化噪声 + AWB/CCM 在近黑信号上的色偏（对比图第 4 列可见绿/紫噪）；
- **这是 LOL 协议对任何 RAW 域管线的固有不公**：8-bit 暗图重编码进"高于 BLC 的 12-bit"丢
  阴影，而 RGB 深网在 8-bit sRGB 上直接操作仍有信息。真实相机靠满阱深度+模拟增益在阴影
  保留真实信号，不会这样被压——LCDP（适度低光、3x 提亮）本路线可达 18–20dB 即为证。
- 次因：回灌锁定 1x 增益（`--again 1024`）阻止 ISP 增益上探，加剧欠提亮（输出仅到 0.103）。

**结论**：LOLv1 这类"离线恢复已裁掉信息的 8-bit 暗图"是 RGB 深网（Retinexformer）的主场，
不是实时 RAW-域 ISP 参数路线的适用域。两者是**不同任务**：本路线做的是实时相机 30fps
全分辨率 ISP 增强（Zero-DCE Lite 板端 640×320 已需 29.9ms、Retinexformer 更重，均不可实
时全分辨率）；离线 sRGB 恢复不是其目标。对比图 `artifacts/lcdp-replay-demo/9_lolv1_benchmark.png`。

### 5.16 公平域对比：真实相机 RAW 欠曝恢复（2026-07-05）

§5.15 的 LOL 落在对 RAW 域最不利的域。公平域测试：**真实相机、真实 RAW、实时可跑的方法**。
静态桌面场景，板端两次真实采集——正常曝光（AE 收敛，luma 0.505，作恢复参考）+
锁定低曝光（exptime 6000/1x，真实 12-bit RAW，中性帧 luma 0.044）。恢复 ~12x。
本路线 `欠曝 RAW → distill ParamNet θ → ISP`；Zero-DCE-Lite 处理同一欠曝 8-bit 帧。

| 方法 | PSNR | SSIM | 输出 luma |
|---|---|---|---|
| 欠曝输入 | 6.56 | 0.131 | 0.044 |
| Zero-DCE-Lite | 9.23 | 0.480 | 0.159 |
| **Ours(ParamNet+ISP)** | **14.42** | **0.811** | 0.329 |

**结果反转（vs §5.15 LOL）**：真实 RAW 域，本路线**大幅胜出** Zero-DCE（+5.2 dB PSNR，
SSIM 0.811 vs 0.480），且**无 LOL 的噪声/色偏**（对比图 `10_real_underexposure.png`：纸张
文字/木纹干净 recovered）。机理明确：本路线在 12-bit RAW 上操作，能利用真实传感器阴影
信号；Zero-DCE 吃的是同一欠曝的 8-bit 帧（luma 0.044 仅 ~11 个 8-bit 级），信息更少。
两者都未完全追平参考亮度（12x 恢复偏保守），但本路线的结构/噪声/色彩明显更优。

**两个 benchmark 合起来的完整结论**：
- **离线恢复已裁掉信息的 8-bit 暗图**（LOL 协议）→ 重型 RGB 深网（Retinexformer 22.58）主场，
  本路线因 RAW 重编码 artifact 失利；
- **真实相机 RAW 实时增强**（本路线的部署域）→ 本路线胜过同样实时可跑的 Zero-DCE，
  且 ISP 硬件 30fps 全分辨率施加（Zero-DCE Lite 板端 640×320 已需 29.9ms）。
路线定位由此坐实：**不是离线画质 SOTA 的竞争者，是实时 RAW-域 ISP 增强的可行解**。

## 参考文献

- Tseng et al. 2019, *Hyperparameter Optimization in Black-box Image Processing using Differentiable Proxies*, ACM TOG 38(4). https://light.princeton.edu/publication/proxy_opt/
- Mosleh et al. 2020, *Hardware-in-the-Loop End-to-End Optimization of Camera Image Processing Pipelines*, CVPR. https://light.princeton.edu/publication/hil_image_optimization/ ，代码 https://github.com/princeton-computational-imaging/LITL-Optimization
- Qin et al. 2022, *Attention-aware Learning for Hyperparameter Prediction in Image Processing Pipelines*, ECCV. https://www.ecva.net/papers/eccv_2022/papers_ECCV/papers/136790265.pdf
- 2024, *Learning Instance-Specific Parameters of Black-Box Models Using Differentiable Surrogates*, arXiv:2407.17530. https://arxiv.org/abs/2407.17530
- 2025, *Conditional GAN-Based Two-Stage ISP Tuning Method: A Reconstruction–Enhancement Proxy Framework*, Applied Sciences 15(6):3371. https://doi.org/10.3390/app15063371
- AdaptiveISP 2024, NeurIPS, arXiv:2410.22939. https://openimaginglab.github.io/AdaptiveISP/
- DynamicISP 2023 (ICCV) / PQDynamicISP, arXiv:2403.10091. https://arxiv.org/abs/2403.10091
- Brooks et al. 2019, *Unprocessing Images for Learned Raw Denoising*, CVPR（sRGB→伪 RAW 逆变换，用于域差距对策）. https://arxiv.org/abs/1811.11127
