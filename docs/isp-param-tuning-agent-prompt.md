# SS928 ISP 参数自动调优——后续开发 Prompt（AI 代理交接）

> 本文是继续本主线开发的**单一入口**。目标：让冷启动的 AI 代理无需重读长对话即可续上。
> 完整实验记录见 [isp-param-tuning-research.md](isp-param-tuning-research.md) §5（本文只给可执行现状、
> 基础设施用法、后续路线，以及**一路踩出来的坑与对策**——这些是最省时间的部分）。
> 初始实施 Prompt（Phase 1–3 原始设计）见 [isp-auto-tuning-prompt.md](isp-auto-tuning-prompt.md)，已大部执行。

## 0. 一句话现状

NN（ParamNet）从场景图预测 SS928 ISP 参数（DRC/LDCI/Gamma），ISP 硬件 30fps 全帧施加，
替代人工启发式。**四段执行序已全部打通**，画质在部署域（真实相机 RAW 实时增强）已验证有竞争力。

| 阶段 | 内容 | 状态 |
|---|---|---|
| 0 保真度闸门 | 排序保真：tone/strength/LDCI/Gamma 组内秩相关全 +1.000（跨曝光/场景） | ✅ |
| 1 残差校准 | `sim+R` 校准代理；R v1 窄域 27.4dB，v2 宽域收敛 22.4dB（宽域更难） | ✅ |
| 2 ParamNet 预训练 | 352K 全卷积，代理上训；LCDP valid 20.6dB（超 CTBG 逐像素上限 19.83） | ✅ |
| 3 硬件蒸馏 | θ\* 硬件标签 + 排练微调；4 留出图 over2 过压 +2.28dB；机制验证成功，**需扩标签规模** | 🔶 首轮 |
| — 定位验证 | 真实 RAW 欠曝恢复本路线胜 Zero-DCE +5.2dB；LOL(8-bit暗图)是 RGB 深网主场（§5.15/5.16） | ✅ |

**下一步最高优先级**：扩蒸馏标签规模（画质路线）**或** OM 导出+实时闭环集成（部署验证）。见 §5。

## 1. 不可违背的核心方法论（每一条都有实测代价背书）

1. **硬件永远不进 SGD 内层环**。硬件不可微、吞吐 ~0.2–0.5s/次、独占状态。硬件只在**离线**产数据：
   产 `(x,θ)→输出` 训可微代理 `R`，或产 `scene→θ*` 蒸馏标签。训练环里只有代理（可微）。
2. **域一致性由结构保证，不靠正则**。`paramnet.u_to_theta` 必须与 `calib_dataset.sample_params`
   **逐段完全同映射**（tone 对称 ±0.30、strength、ldci、mix、ctrl、blend、gamma γ 幂曲线）。
   改一处必须改两处，否则 ParamNet 输出落到校准代理的域外（代理无保真承诺）。
3. **收敛后才裁决，不拿被掐断的残局下结论**。训练冻结/中断 ≠ 收敛；不同 LR 阶段的 val 不可直接比
   （cosine 未退火的中途值偏低）。踩过：曾据 ep160 冻结值误判"容量无用"，续训到收敛后 +0.4dB。
4. **诚实记口径**。PSNR 带分辨率/是否 GT 均值对齐；"离线模型耗时" ≠ "端到端实时帧率"；
   代理口径 PSNR ≠ 板端真值。板端实测优先于代理数字。

## 2. 基础设施清单（命令 + 已知陷阱）

所有 Python 从**仓库根**用 `python -m models.isp_simulator.<模块>` 运行（相对路径依赖 cwd，
`python -m` 之外的调用会 `No module named 'models'`）。

### 板端 harness：`board/tests/test_raw_replay.c`（RAW 定格/文件回灌 + 批量 sweep）
- 编译：`scripts/build_board.sh Release`（环境变量见 [[hispark-build-env]] / `scripts/env.sh`）。
- **部署新二进制**：板端 `/root/socchina-2026/fidelity/*/test_raw_replay` 是旧拷贝，改了源码要 scp
  新的。踩过：`RAW_FILE_MAX` 从 8 提到 16 后没部署，`--raw-file` 报 `unknown arg`。
- 模式：
  - dump 定格：`--compress-none --save-raw` 采一帧裸 12bpp bayer（供主机解析/合成）。
  - 文件回灌：`--raw-file <f>`（≤16）从裸 bayer 文件构造帧作输入。
  - 批量：`--blob-dir <dir>` 按名序加载全部 `*.bin`（≤511），每 raw × 每 blob 全组合施加，
    输出 `out_f<fi>_<idx>_blob_<name>.nv21`（idx=0 是 baseline，blob 从 idx1 起；**文件名含 `.bin`**）。
- **必带 `--settle 8`**：ISP 有时域滤波，切参后前几帧被前组状态污染。校准/搜索一律 settle 8。
- **锁曝光 `--exptime <us> --again <x1024>`**：文件回灌也要锁——否则 AE 数字增益自动改写回放帧亮度，
  "偷走" NN 的功劳/搅乱可比性。
- 约束：仅线性模式（`ss_mpi_isp_run_once` **不支持帧合成 WDR**）；VI 必须离线（`raw_replay=1` 走
  `OT_VI_OFFLINE_VPSS_OFFLINE` 且不启 `ss_mpi_isp_run` 线程，与 `run_once` 互斥）；独占媒体链。

### blob 协议 v3：`board/src/isp.c:isp_load_blob_and_apply` ↔ `models/isp_simulator/isp_blob.py`
- magic `ISP\0` + version + flags；DRC(tone200 + strength + 主/X 双路 mixing + 标量) + LDCI + GAMMA 段
  + GUARD/COLOR 护栏子段。板端兼容 v1–v3。**infer 出 blob 必须 `gamma_on=True`**（v2 θ 含 gamma，
  漏了 gamma 段是踩过的 bug）。
- **护栏/色彩(GUARD/COLOR)作 preset 不进 NN 维度**（高光 bright_gain_limit、CC LUT），避免蒸馏标签空间膨胀。

### sRGB→sensor RAW 配方：`models/isp_simulator/synth_raw.py:rgb_to_sensor_raw`
三个常量**全为板端实测**，改动需重新标定：
- bayer 相序 **BGGR**（OS08A20 pub_attr 实配；用 RGGB 会全图品红）。
- 逆 AWB：`R/=1.77, B/=1.85`（ISP 期望真实 sensor 色偏，灰输入实测；不预除会再被拉一次成品红）。
- 黑电平 **BLC=256**（条带 RAW 一次回灌定位；基准高于 BLC 的残留 DC 经 AWB 放大成暗部紫偏）。
- 12bpp packed 布局 **lsb**（`parse` 子命令自动判定，预览目检）。
- ⚠️ **局限**：把 8-bit 暗 sRGB 重编码进"高于 BLC 的 12-bit"会丢阴影信号（§5.15）——对**已裁信息的
  离线暗图**不公；对**真实相机 RAW**（有真实 bit 深度）正常。评测口径要认清这条。

### 训练/评测脚本
- `fidelity_gate.py`：闸门 sweep 生成 + `analyze`（秩相关；`--file-idx` 多场景）。
- `calib_dataset.py`：LHS 采样（v2 对称+γ，30 维）→ blob 目录；`report` 残差量化。
- `residual_net.py`：FiLM 残差校准网 R（θ 条件 95 维含 gamma；`--ch/--blocks` 可配；
  **`--init` 热启动 / `--resume` 全状态 + 每 eval 原子存 `last.pt`**——训练偶发 CUDA 忙等冻结，靠这个兜底）。
- `paramnet.py`：352K ParamNet（NPU 红名单安全：无 Pow/Cast/ReduceMean）；`prepare/train/eval/infer/export/audit`
  （`infer/export/audit --ckpt` 指定权重）；`CalibratedProxy` 按 ckpt 的 theta_slice/ch/blocks 自适应。
  - **`audit`（路线 B1 前置关口）**：导出 ONNX 并审计 NPU 部署友好性——红名单算子
    （Pow/Cast/ReduceMean/Resize…命中即 FAIL）/ 静态 NCHW shape / 单 opset。权重可缺
    （算子与权重无关，随机初始化即可审图）。`export`/`audit` 已改为无权重时随机初始化，
    不再硬失败。**已实测 PASS**（见 §4 路线 B）。
- `build_paramnet_om.sh`（路线 B1 关口②）：ONNX→OM（AIPP，部署形态，soc=OPTG）。ASCEND_TOOLKIT_HOME
  + ATC_PYTHON 驱动（CANN 5.20 在 `tools/local/Ascend/`）。**已实测产物 0 Cast/0 AICPU**（见 §4）。
- `hw_search.py`：真实 ISP 上两轮黑盒 θ\* 搜索（蒸馏标签原型，`--ref` 参照后缀）。
- `distill.py`：`gen`（选图+候选池 LHS+γ+per-image warmstart 保底）/`labels`（逐图 θ\*）/
  `finetune`（**排练式**：LCDP 代理目标保泛化 + 硬件 θ\* 拉修正）。

### Python 环境（互不相通，用 `.npy` 桥接跨环境）
- `torch2`：本项目主环境（PyTorch 2.5，CUDA）。8GB 显存，48ch/6blk 全帧 batch16 会 OOM → batch 8；
  设 `PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True`。
- `tf2onnx`：跑 Zero-DCE-Lite SavedModel（缺 PIL，用 npy 传图）。
- `Retinexformer`：`/home/alan/Learning/Retinexformer`，LOL_v1 权重。**arch 坑**：仓库 `RetinexFormer`
  包装类硬编码 `level=1`，但 LOL_v1 权重是 `level=2` 训的——直接用 `RetinexFormer_Single_Stage(
  n_feat=40,level=2,num_blocks=[1,2,2])` 并剥 `body.0.` 前缀加载。
- ATC 环境转 OM（见 §5 路线 B）。

## 3. 板端操作纪律（踩坑集大成，违反很费时间）

- **每次板端会话**：`systemctl stop socchina-stream`（释放 VB 独占）→ 采集 → `systemctl start` →
  确认 `systemctl is-active`。用户已授权本会话及后续沿用此流程（[[isp-param-tuning-mainline]]）。
- 运行需 `LD_LIBRARY_PATH=/opt/lib/npu`。
- **日志放持久位置**（如 `models/weights/*/train.log`），不要只放 scratchpad——scratchpad 会被清，
  丢过训练曲线。GPU 后台训练用 `run_in_background`，靠完成通知驱动后续。
- 网络偶发抖动（scp 慢到 KB/s）：`ping`/1MB 测速确认恢复再重传；大批帧用 `tar czf - | tarxzf`。
- 判 GPU 是否卡死：`nvidia-smi` 看 util（100%=在跑；0% + 进程存活 9h = 冻结，`pkill` 后 `--resume`）。
- 板端媒体脏（D 态/`already inited`/SMMU CMDQ timeout）→ 干净重启板卡比反复重试省时间。

## 4. 后续路线（按优先级；每条给具体步骤）

### 路线 A（画质主线）：扩蒸馏标签规模
现状：15 图 θ\* 标签 + 排练微调，机制成功但 per-scene 有噪声（over1 -0.85，因场景类型未覆盖）。
纯回归 15 图会**灾难性遗忘**（valid 20.4→14.9），排练式防住（→19.9）。
- 步骤：`distill gen --num <N> --cands 128`（N=200~400，跨亮度桶）→ 板端多会话（每 ≤16 图/会话，
  ~15 分钟/会话，累计 ~3h）→ `distill labels` → `distill finetune`（排练式，`--lam-d` 调硬件项权重）。
- 判据：4 留出演示图 + LCDP valid **代理口径不回归** + 板端复验跨场景一致提升（尤其 over 组趋近 θ\* 上限）。
- 提效：θ\* 候选池含 per-image warmstart（保底 θ\*≥当前网络）；128 LHS 候选离线可对新参照重评分（零板端成本）。

### 路线 B（部署验证，可能比 A 更根本）：OM 导出 + 实时闭环
"能否 30fps 实时"是整个架构成立的前提，尚未端到端验证。
- ONNX 导出 ParamNet（红名单已规避 Pow/Cast/ReduceMean；固定输入 256×144 NCHW，单 opset）。
  - **B1 关口①（ONNX 算子审计）已 PASS**（`python -m models.isp_simulator.paramnet audit`，主机离线，
    随机权重即可）：opset 单一 =13、输入 `[1,3,144,256]` 静态 NCHW、输出 `[1,30]`，算子仅
    `Conv×6 / Relu×5 / AveragePool×1 / Sigmoid×1 / Flatten×1`——**零红名单算子**（AvgPool 是固定核，
    非 ReduceMean）。红名单安全声称已客观坐实。
- ATC→OM（FP16、`soc_version=OPTG`）；**算子探测先行**：转 OM 确认无 AICPU/Cast 残留、profiling 全落 AICore。
  - **B1 关口②（ATC→OM）已 PASS**（主机离线，CANN 5.20.t6.2.b060 在 `tools/local/Ascend/`）：
    一键脚本 `models/isp_simulator/build_paramnet_om.sh`（ASCEND_TOOLKIT_HOME + ATC_PYTHON 驱动）。
    - **AIPP 部署版**（`--insert_op_conf=configs/aipp_nv21_256x144.cfg --output_type=FP16`）：
      算子仅 `Conv2D×5 / FullyConnection / AvgPoolV2 / Flatten / TransData`——**0 Cast、0 AICPU**。
      AIPP 把 chn2 NV21→RGB→/255→fp16 吸进 OM 前端，边界 Cast 全消。
    - 无 AIPP 基线：多 2 个 FP32 输入/输出边界 Cast（`trans_Cast_0` 吃 image、`trans_Cast_28` 出 u），
      仍全 AICore、无 AICPU；部署走 AIPP 即可消掉。
    - **踩坑**：ATC 的 TBE 编译器需带依赖(numpy/decorator/sympy/cffi)的 python3——用 conda `atc`
      环境 python；系统/base python3 会 `Failed to init tbe`。CANN 环境 `source <toolkit>/bin/setenv.bash`。
  - **剩余未验证**：端到端**实时帧率**（是否 30fps / param-net ~10Hz 刷新延迟）——需板端 profiling（关口②只证图/编译干净，非运行时性能）。这是路线 B 的下一关，走 B2 实时闭环。
- 实时闭环：`chn2 缩略图 → AIPP → ParamNet OM → u→θ → blob → ss_mpi_isp_set_*_attr 热刷新`。
  复用 `board/src/main.c` control worker（场景变化触发 ~10Hz）、CLUT 桥的逐次步长护栏/反馈抑制。
- 注意闭环稳定性：ParamNet 输入是"当前参数处理后的帧"，存在反馈回路——板端复用步长护栏经验。

### 路线 C（现场验收）：考验场景真实 A/B
桌面白天场景 vendor-auto 本无明显问题。需夜间/强逆光真实采集，与 vendor-auto/WDR 对比 + 10 分钟稳定性 +
现场 flicker 验收（口径见 `quality-acceptance.md`）。WDR 相关时域项 `tpr_incr/decr_coef` 此时复查。

### 已知残差目标（模拟器缺陷，蒸馏天然免疫，若走代理路线需修）
1. 极暗域 DRC strength 幂函数外推低估（标定域 shadow≈0.155 外推 20x）。
2. `ldci.py` CLAHE 在暗纹理域反序（硬件 he_pos 单调提亮，模拟器压暗）。

## 5. 训练/工程踩坑速查

| 现象 | 根因 | 对策 |
|---|---|---|
| 训练 GPU 0% CPU 忙等 9h | 偶发 CUDA/环境挂起（非收敛） | `pkill` + `--resume`（last.pt）；`expandable_segments` |
| 8GB OOM | 48ch/6blk 全帧 batch16 | batch 8 |
| 微调 valid 崩到 14.9 | 少标签纯回归灾难性遗忘 | 排练式（代理目标 + 硬件标签）|
| 板端 `--raw-file unknown arg` | 板端旧二进制 RAW_FILE_MAX=8 | 部署新二进制 |
| 回放全图品红 | bayer 相序 / 逆 AWB | BGGR + 预除 R/B |
| 暗部紫偏 | BLC 基准错位 | BLC=256（条带实测） |
| 切参首帧异常 | ISP 时域滤波 | settle≥8 |
| 欠曝回放被自动提亮 | AE 数字增益 | 锁 exptime/again |
| 容量"证伪" | 拿冻结截断值 + 不同 LR 阶段比 | 收敛后比，同调度阶段 |

## 6. 不要做的事

- 不要拿被掐断/未收敛的训练值下架构结论。
- 不要改 `u_to_theta` 而不同步 `calib_dataset.sample_params`（域漂移）。
- 不要在 LOL 这类离线 8-bit 暗图 benchmark 上评判本路线并当作能力否定（RAW 域 artifact，§5.15/5.16）。
- 不要把硬件塞进 SGD 内层环。
- 不要只把训练日志放 scratchpad（会被清）。
- 不要跳过 `--settle 8` 和锁曝光做校准采集。
- 不要用 RGGB 合成 RAW；不要漏 blob 的 gamma 段。
- 不要在未 stop socchina-stream 时跑板端采集（VB 争用）。

## 7. 可直接采信的关键数据点

- 保真度闸门：tone/strength/LDCI/Gamma 组内秩相关 +1.000（3 曝光 × 8 合成场景，23/24）。
- R：v1 窄域 27.4dB / v2 宽域收敛 22.4dB（宽域=双向+Gamma+真实彩色+极暗，更难；容量 32→48ch 仅 +0.4dB）。
- ParamNet：LCDP valid 20.6dB（>CTBG 逐像素 19.83）。
- θ\* 上限缺口：过曝 over1 2.7dB / over2 6.8dB（代理训练学不到 → 蒸馏必要性）。
- 蒸馏：θ\* 标签平均 +1.8dB；over2 过压 18.44→20.72（+2.28）。
- 定位：真实 RAW 欠曝 Ours 14.42 vs Zero-DCE 9.23（+5.2dB）；LOL Retinexformer 22.58 主场。
- 校准数据在 `models/weights/{calib,calib_v2,fidelity,paramnet,distill,lolbench,real}/`（不入库，重采约 7–15 分钟/批）。
