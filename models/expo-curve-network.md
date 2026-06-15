# 双向曝光校正曲线预测网络 —— 结构 + FP16 OM + 板端耗时（阶段 B 去风险）

> 状态：2026-06-14。回答 [architecture.md](../docs/architecture.md) §6 **点 1**（最高优先级风险，OM
> 全分辨率实测耗时能否压进 ≤33ms）**与点 2**（AIPP CSC/归一开销）。
> 记录遵循 development-guide.md §9 四段式：**目标 / 命令路径 / 结果 / 解读**。

## 目标

- 实现 architecture.md §2 第 7 级网络（AvgPool/4 → 低分辨率 backbone → 预测曲线参数 →
  ConvTranspose 上采样 → 全分辨率施加 `x=x+r(x²-x)` ×niter），**结构正确 + 干净上板优先于画质**，
  权重随机（本任务不做认真训练）。
- 导出干净静态 ONNX（NCHW、opset 13、单输出、无 Resize），ATC 转 FP16 OM，算子探测确认
  **无 AICPU/Cast、全落 AICore**，板端实测每帧耗时，据此对照 33ms 预算做 go/no-go。

## 命令路径

环境（不绑定环境名，按 [README.md](README.md) 两套环境；本机实测用的解释器/工具如下）：

- 模型/导出：`torch 2.5.1` + `onnx 1.19.1`（Python 3.9）。
- ATC：`Ascend-cann-toolkit 5.20.t6.2.b060`，`soc_version=OPTG`。
- 交叉编译：`aarch64-mix210-linux-g++`（工具链前端需 `libisl.so.19`，经 `LD_LIBRARY_PATH` 提供）。
- 板端：`root@192.168.1.168:/root/socchina-2026/`，ACL 运行时 `/opt/lib/npu/libascendcl.so`，
  内核 4.19.90 aarch64。

```sh
# 1) 网络 + 导出 ONNX（FP32 供核对 + FP16 供 ATC）—— 步骤 1+2，不需硬件
python -m models.exporters.expo_curve_onnx --niter 8 --filters 16 \
    --width 1024 --height 576 --opset 13 --in-name input --out-name output
#   → models/weights/expo_curve_1024x576.onnx        (FP32, 清理 + checker + 打印算子直方图)
#   → models/weights/expo_curve_1024x576_fp16.onnx   (FP16 整图直转, 无 Cast)
#   参数化: --niter / --filters / --width --height / --shared（共享曲线变体）/ --tag（不覆盖正式产物）

# 2) ATC 转 FP16 OM（NCHW）—— 步骤 3
ATC_SETENV=<CANN>/bin/setenv.bash \
  models/build_expo_curve_fp16_om.sh \
    models/weights/expo_curve_1024x576_fp16.onnx models/weights expo_curve_1024x576_fp16 1024 576
#   atc --framework=5 --input_format=NCHW --input_shape='input:1,3,576,1024'
#       --soc_version=OPTG --output_type=FP16
#   → models/weights/expo_curve_1024x576_fp16.om  (+ .fusion_result.json)

# 2b) 可选挂 AIPP（NV21→RGB→/255 融入 OM 前端）—— §6 点 2 开销实测
AIPP_CFG=models/configs/aipp_nv21_1024x576.cfg ATC_SETENV=<CANN>/bin/setenv.bash \
  models/build_expo_curve_fp16_om.sh \
    models/weights/expo_curve_1024x576_fp16.onnx models/weights expo_curve_1024x576_fp16_aipp 1024 576
#   → 追加 --insert_op_conf=...；OM 输入变 NV21(YUV420SP_U8, NHWC, uint8)

# 3) 算子探测（步骤 4）：OM→JSON，确认无 AICPU/Cast，全落 AICore
atc --mode=1 --om=...expo_curve_1024x576_fp16.om --json=...om.json

# 4) 板端 benchmark（步骤 5）：交叉编译复用 acl_om_benchmark.cpp（原样），scp 运行
aarch64-mix210-linux-g++ -std=c++17 -O2 -I <CANN_arm64>/include \
    experiments/zero-dce-npu-om/src/acl_om_benchmark.cpp \
    -L <CANN_arm64>/lib64 -lascendcl -lmsprofiler \
    -Wl,--allow-shlib-undefined -Wl,--unresolved-symbols=ignore-in-shared-libs \
    -o acl_om_benchmark
scp acl_om_benchmark expo_curve_1024x576_fp16.om root@192.168.1.168:/root/socchina-2026/
ssh root@192.168.1.168 'cd /root/socchina-2026; export LD_LIBRARY_PATH=/opt/lib/npu:/opt/lib; \
    ./acl_om_benchmark --model expo_curve_1024x576_fp16.om --warmup 5 --repeat 30'

# 单元测试（development-guide §10）
python -m pytest models/tests/test_expo_curve_net.py -q   # 11 passed
```

## 结果

### 结构 / ONNX（步骤 1+2）

- 网络参数化 `niter`/`filters`/`down`/`shared_curve`；canonical 配置 `niter=8, filters=16` →
  **25,128 参数**（共享曲线变体 17,715）。输出范围钳到 `[0,1]`。
- ONNX：`input` `1x3x576x1024` FP16 → `output` `1x3x576x1024` FP16；opset 13、单输出、静态、无动态维。
- `onnx.checker` 通过。算子直方图（FP32/FP16 一致，68 节点 11 种）：
  `Conv x7 · ConvTranspose x1 · AveragePool x1 · Clip x7 · Tanh x1 · Concat x3 · Split x1 ·
  Mul x16 · Sub x8 · Add x8 · Constant x15`。
  **上采样为 ConvTranspose（x1）、无 Resize、无红名单（LayerNorm/attention/FFT 等）、无 Relu（隐层用 Clip）。**
- 隐层激活刻意用 `Clip(0,6)=ReLU6` 而非 `Relu`：绿名单含 Clip 不含 Relu，严格落在硬约束内。

### 算子探测（步骤 4）—— OM 干净落 AICore ✅

ATC 转换成功（`ATC run success`）。`om.json` 解析（48 OM 节点）+ `fusion_result.json`：

- **engine：AIcoreEngine x27（全部 16 个计算算子 + 11 个 TransData）、GE_LOCAL x20（Const/Data）、RTS x1。**
- **Cast：无。AICPU：无。** ← §6 点 1 的"能否干净上 NNN"结论：**能**。
- 健康融合：`ConvClipByValueFusionPass 6/6`（6 个 Conv+ReLU6 融成单 AICore kernel）、
  `MulSquareFusionPass 8/8`（x² 融成 Square）、`AvgPoolV2FusionPass 1/1`、`AutomaticUbFusion`
  把全分辨率逐像素链融成 UB kernel。

### 板端实测耗时（步骤 5，`warmup 5 / repeat 30`，单算 OM 执行，不含 AIPP/后处理）

耗时极稳（各配置 max-min < 0.2ms）。avg ms：

| 配置 (filters=16) | 1024x576 | 768x432 | 640x360 |
| --- | ---: | ---: | ---: |
| niter=8（独立曲线，3·niter 通道，canonical）| **94.86** | 52.66 | 37.27 |
| niter=4（独立曲线）| 53.57 | **30.47** ✅ | 20.66 ✅ |
| niter=2（独立曲线）| 44.59 | — | — |
| niter=1（独立曲线）| 38.87 | — | — |
| niter=8（**共享曲线** shared_curve）| 49.13 | — | 19.53 ✅ |
| niter=4（共享曲线）| 39.99 | — | — |

旁证：`filters=8, niter=4 @1024x576 = 56.55ms`，**比 filters=16 还慢** → 砍 filters 不是有效杠杆。
（对照 AGENTS.md：旧全分辨率 Zero-DCE Lite `1024x640 ≈ 96ms`，与本网络 `1024x576 niter=8 ≈ 95ms` 同量级。）

### AIPP CSC/归一开销（架构 §6 **点 2**）—— 实测 ≈ 0ms ✅

同一 niter=8 模型，挂 AIPP（NV21→RGB→/255 融入 OM 前端）对比不挂：

| 配置 | 不挂 AIPP | 挂 AIPP | 差值 | AIPP 输入 |
| --- | ---: | ---: | ---: | --- |
| 1024x576 niter=8 | 94.80 ms | **94.61 ms** | **−0.19**(噪声内) | NV21 uint8 NHWC 0.88MB |
| 640x360 niter=8 | 37.29 ms | **37.21 ms** | **−0.08**(噪声内) | NV21 uint8 0.35MB |

算子探测（AIPP OM）：输入 Data 变 `DT_UINT8 / NHWC / 1x576x1024x3`，新增 `Aipp x1`，
**TransData 11→10**（AIPP 吸收了一次输入端布局转换），engine 分布不变（AIcore x27），**无 Cast/AICPU**。

### 速率矩阵：模型变体 × 分辨率（2026-06-14，warmup3/repeat20，avg ms）

固定 `filters=16`（已确认非杠杆）。✅=进 33ms 预算。`ExpoCurveNet` 是本网络（/4 backbone + 曲线施加），
`ZeroDCE_Lite_ref` 是既有**全分辨率卷积**架构（无 /4 降采样）的对照 OM。

| 分辨率(px) | distinct niter8 | shared niter8 | shared niter4 | ZeroDCE_Lite_ref |
| --- | ---: | ---: | ---: | ---: |
| 1024x576 (590k) | 94.85 | 49.13 | 39.76 | 97.29 @1024x640 |
| 768x432 (332k) | 52.64 | **27.20** ✅ | **22.64** ✅ | — |
| 640x360 (230k) | 37.35 | **19.51** ✅ | **15.26** ✅ | 33.88 @640x360 |
| 512x288 (147k) | **23.50** ✅ | **12.10** ✅ | **9.77** ✅ | — |
| 384x216 (83k) | **13.57** ✅ | **6.99** ✅ | **5.61** ✅ | — |
| 320x180 (58k) | **9.25** ✅ | **4.79** ✅ | **3.89** ✅ | 8.46 @320x180 |

读图：

- **harness 交叉验证 ✅**：`ZeroDCE_Lite_ref @1024x640 = 97.3ms` 与既有记录(AGENTS.md "≈96ms")一致 →
  本套 benchmark 数字可信，不是工具假象。
- **耗时≈像素数线性**（访存带宽主导）：shared_n8 从 1024x576→320x180，像素降 10.2×、耗时降 10.3×，
  截距≈0。**实时与否本质是"像素预算 vs 每像素工作量"的线性权衡**。
- **每像素工作量由 niter 与"独立/共享曲线"决定**：`shared` 在所有分辨率上都≈`distinct` 的一半。
- **本网络 /4-backbone + 共享曲线优于全分辨率 Zero-DCE Lite**：640x360 上 `shared_n8=19.5ms` < `ZDCE_Lite=33.9ms`；
  但 `distinct_n8=37.4ms` 反而略慢于 ZDCE_Lite —— 24 通道全分辨率上采样+8 次独立施加的代价超过了 /4 backbone 省下的。
- **33ms 实时档可达上限**：`shared_n8` 一路顶到 **768x432（27.2ms，niter 仍满 8）**；`distinct_n8` 只能到
  512x288。**1024x576 无任何变体进预算**（最低 shared_n4=39.8ms）。

#### 对照：SCI（Self-Calibrated Illumination, CVPR 2022）—— 全分辨率超轻量基线

此前候选路线未包含 SCI；这里按其推理结构
（IEM 5 层 conv + Retinex 除法 `x/illu`，全分辨率、无降采样）实现并实测。算子探测干净：
`RealDiv` 落 AICore，**无 AICPU/Cast**（但全分辨率带来 **TransData x12**，比本网络还多）。

| 分辨率 | SCI channels=3（420 参数） | SCI channels=16 |
| --- | ---: | ---: |
| 1024x576 | 80.09 | 60.83 |
| 768x432 | 44.13 | 33.52 |
| 640x360 | **30.90** ✅ | **23.47** ✅ |
| 512x288 | **19.70** ✅ | **14.95** ✅ |
| 320x180 | **7.72** ✅ | **5.91** ✅ |

两条强结论：

- **"参数少 ≠ 快"被钉死**：SCI 仅 **420 参数**却在 1024x576 要 **80ms**——因为 5 个 conv + 12 个 TransData
  全在全分辨率跑，**访存主导、与参数量无关**。再次印证"按片上算子亲和度选型，而非按参数量"。
- **通道数要对齐 16（C0）**：`channels=16` 比 `channels=3` **更快**（1024x576：60.8 < 80.1ms），尽管算力多 5×。
  通道=3 不对齐 5HD 格式的 C0=16 → 补零浪费 + TransData 开销。**别用 <16 的通道数。**（与 ExpoCurveNet
  砍 filters 反而变慢同因。）
- **SCI 不占优**：同分辨率下 SCI（全分辨率）比本网络 `shared` 变体慢（640x360：SCI 23–31ms vs 本网络
  shared_n8 19.5ms），实时上限约 640x360，低于本网络的 768x432。且 **SCI 单向提亮**（`x/illu` 只能拉亮、
  不能压高光），**不满足双向曝光需求**，仅作速率对照。

#### 对照：MSEC（多尺度 U-Net, CVPR 2021）与 CoTF（3D-LUT, CVPR 2024）

两者都是**双向**曝光校正（满足任务），但代表两条相反的工程结局。均为结构/速率代理实现（随机权重，
NNN 适配：ConvTranspose 替 bilinear），见 [networks/msec.py](networks/msec.py) /
[networks/cotf.py](networks/cotf.py)。

| 模型 | 1024x576 | 768x432 | 640x360 | 512x288 | 384x216 | 算子探测 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| **MSEC** U-Net(base24, 1.1M 参数) | 282.4 | 140.8 | 95.8 | 55.5 | **31.6** ✅ | 干净落 AICore，无 AICPU/Cast |
| **CoTF** param-net（NN-only, 207k） | **4.68** | — | **1.94** | — | — | 干净落 AICore（GlobalAvgPool→ReduceMeanD） |

- **MSEC = 全分辨率 U-Net，最重**：1024x576 要 **282ms（~3.5fps）**，实时上限只到 384x216（83k px，太小）。
  结论：**MSEC 的价值是它的过+欠曝配对数据集（训练用），不是它的架构（部署用）。**
- **CoTF = 唯一突破访存地板的结构**：它的 NN 只在低分辨率"出 3D-LUT 系数"，1024x576 仅 **4.68ms**
  （比任何全分辨率输出模型快 ~20×）。**但全分辨率 3D-LUT 施加（三线性插值）连 ONNX 都导不出**——
  `torch.onnx.export` 直接报 `Unsupported: GridSample with 5D volumetric input`。即 **3D-LUT 施加无法走
  ONNX→OM→NPU，必须交给 ISP 硬件 CLUT（`ss_mpi_isp_set_clut`）或自定义算子**。这给出了
  architecture.md §4.1/§6 的确定答案：**当前 NPU 导出路径不可行，NN 出参数 + ISP 硬件施加可行**。

## 解读

1. **结构可干净上板：确认 ✅。** OM 无 AICPU/无 Cast、全落 AICore，融合健康。"能不能干净上 NNN"
   这一半风险已消除，曲线网络这条结构路线成立。

2. **Config-R 全分辨率 1024x576 ≤33ms：当前结构不成立 ❌。** canonical `niter=8` 实测 94.86ms（≈2.9×
   超预算）；即便砍到 **niter=1 仍 38.87ms**。瓶颈不是 /4 的卷积 backbone，而是**全分辨率的访存密集
   操作**（曲线施加迭代 + ConvTranspose 上采样 + 11 个 TransData 布局转换）——`filters` 砍小反而变慢
   即为佐证。把 backbone 放低分辨率"省下的"算力，被全分辨率逐像素/搬运吃掉了。

3. **有效杠杆是分辨率与"共享曲线"，不是 filters：**
   - **分辨率**：`768x432 niter=4 = 30.5ms`、`640x360 niter=8 = 37.3ms`、`640x360 niter=4 = 20.7ms`
     已进入 33ms 预算内。
   - **共享曲线**（Zero-DCE-Lite 式，单套 3 通道复用 niter 次，而非 architecture §2 原义的 3·niter
     独立通道）：全分辨率只需驻留/上采样 3 通道而非 24 通道，**niter 标度成本约减半**
     （1024x576 niter=8：94.86→49.13ms）。`640x360 niter=8 共享 = 19.5ms`，画质迭代与裕量兼得。

4. **AIPP（§6 点 2）几乎零开销 ✅。** 全分辨率挂 AIPP 与不挂同速（差值在 ±0.2ms 噪声内，甚至略快）：
   CSC+/255 由 NNN 前端专用预处理硬件完成，仅替换掉一次输入端 TransData，输入 DMA 还从 3.5MB(FP16 RGB)
   缩到 0.88MB(NV21)。含义：**预处理可整体卸到 OM 前端、零 CPU 逐像素搬运**，直接吃 VPSS chn1 的 NV21；
   但它**只消掉 1/11 个 TransData，救不回 1024x576 的预算**——瓶颈仍是全分辨率曲线施加 + 其余 TransData。

5. **给后续的判决（据此决定是否投入训练）：**
   - **不要**在 `1024x576` 上投入认真训练——无任何变体进 33ms（§6 点 1 的预设结论触发）。
   - **整图增强备选建议 `768x432 + 共享曲线 niter=8`（27.2ms）**——这是进预算的最高分辨率且
     niter 不打折；若要更大裕量给后处理/合成则退到 `640x360 shared_n8`（19.5ms）。
     Config-R 主线已转为 CoTF + ISP CLUT，1024x576 整图输出归 Config-Q 高画质/低帧路径。
   - **像素线性规律**给了快速估算法：`实时分辨率像素 ≈ 33ms / 每像素系数`（shared_n8 ≈ 0.083µs/px →
     ~400k px；distinct_n8 ≈ 0.16µs/px → ~200k px）。换分辨率不必每次都板测。
   - 模型侧 §6 **点 1 + 点 2 均已出数**；余下待验证回到板端工程侧（点 3 后处理 DSP/IVE 比选、点 5 VGS 合成）。
   - **横评 5 个架构后的总论**：1024x576 实时的瓶颈是**全分辨率访存的像素线性地板**，凡"输出整图"的模型
     （本网络/Zero-DCE Lite/SCI/MSEC）都撞它，且参数量无关（SCI 420 参数仍 80ms、MSEC 1.1M 要 282ms）。
     **唯一突破口是 CoTF 式「NN 低分辨率出参数（实测 4.7ms）+ 全分辨率施加交给 ISP 硬件 CLUT」**——
     代价是 3D-LUT 三线性施加无法上 NPU（grid_sample 5D 连 ONNX 都导不出），必须走 `ss_mpi_isp_set_clut`。
     若要 1024x576 真实时，这是后续唯一值得投入的方向；否则维持「本网络 768x432 + 共享曲线（27ms）」。

## 交付物与产物（大件不入库，仅此指针；见 .gitignore）

- 代码：[networks/expo_curve.py](networks/expo_curve.py)（参数化网络）、
  [exporters/expo_curve_onnx.py](exporters/expo_curve_onnx.py)（导出+清理+FP16+校验+算子打印）、
  [build_expo_curve_fp16_om.sh](build_expo_curve_fp16_om.sh)（ATC，NCHW，可选 `AIPP_CFG` 挂 AIPP）、
  [configs/aipp_nv21_1024x576.cfg](configs/aipp_nv21_1024x576.cfg)（NV21→RGB→/255 静态 AIPP）、
  [tests/test_expo_curve_net.py](tests/test_expo_curve_net.py)（pytest，11 passed）。
- 速率对照模型：[networks/sci.py](networks/sci.py) / [networks/msec.py](networks/msec.py) /
  [networks/cotf.py](networks/cotf.py)，导出入口位于 `exporters/`。
- 速率矩阵原始数据：`artifacts/expo_curve_speed_matrix.csv`
  （ExpoCurveNet 变体 + ZeroDCE_Lite_ref + SCI + MSEC + CoTF param-net）。
- 板端 benchmark 复用 `experiments/zero-dce-npu-om/src/acl_om_benchmark.cpp`（原样，未改）。
- 生成物（`models/weights/`，gitignore）：`expo_curve_1024x576{,_fp16}.onnx`、`expo_curve_1024x576_fp16{,_aipp}.om`、
  `.fusion_result.json`、`.om.json`，以及各 sweep 配置 OM。
- 借用来源/改动见各源码文件头注释（Zero-DCE 骨架 / EnhanceNet 16008 低分辨率布局与双态上采样 /
  Zero-DCE-Lite filters 参数化与共享曲线）；AIPP cfg 复用 `artifacts/zero-dce-svp-nnn/lite-160-aipp/` 的 NV21 配置。

> 口径提醒：耗时表是**离线单算 OM 执行耗时**（`aclmdlExecute` 墙钟，warmup 后 30 次），含 AIPP 与否已分列；
> 不含后处理、VGS 合成与显示；不要与端到端实时帧率混用。OM 文件大小不代表参数量。
