# Param-net 训练与部署

> 状态：2026-06-19。本文记录受 CoTF 启发、面向 SS928 AICore 的轻量参数网络训练闭环。
> 该网络保留现有板端接口：输入 `256x144` RGB 缩略图，输出 `3×17³=14739` 个 FP16 LUT 系数。

## 目标

- 用成对曝光图像训练现有 AICore 友好 param-net，而非继续部署随机权重。
- 训练图可使用较高分辨率 crop；网络始终只读取 `256x144` 缩略图，符合 VPSS chn2 + AIPP 路径。
- 训练时在主机 GPU 上做可微 3D-LUT 三线性施加；部署 ONNX **只导出 param-net**，不包含 NNN
  不支持的 `grid_sample`。
- 未训练模型以恒等 LUT 初始化，初始输出近似透传，避免随机 LUT 在板端造成不可控画面。

这不是官方完整 CoTF 的复现。官方自适应采样、协同变换和 Transformer 路径没有进入部署网络；
当前画质上限是全局 3D-LUT。全局曝光/色调在生产链上仍优先由 ISP Gamma/DRC 施加，颜色相关
3D 变换才走 CLUT。

## 数据与命令路径

输入和目标按相对路径严格配对：

```text
dataset/
├── train/
│   ├── input/scene_a/frame.png
│   └── target/scene_a/frame.png
└── val/
    ├── input/frame.png
    └── target/frame.png
```

建议把训练集、板端采集和生成 checkpoint 放在仓库外或 `models/weights/`；图像、权重和日志均不提交。
同一 pair 必须是对齐内容，不能只凭文件名把不同视角或不同 resize 策略的图片配在一起。

```sh
# 训练；默认 100 epoch、batch 4、384 crop、AMP、余弦学习率
python -m models.trainers.cotf_paramnet \
  --train-input /path/to/train/input \
  --train-target /path/to/train/target \
  --val-input /path/to/val/input \
  --val-target /path/to/val/target \
  --output-dir models/weights/cotf_paramnet_train

# 恢复训练
python -m models.trainers.cotf_paramnet \
  --train-input /path/to/train/input \
  --train-target /path/to/train/target \
  --epochs 100 \
  --resume models/weights/cotf_paramnet_train/last.pt

# 导出训练权重；结构参数须与训练时一致
python -m models.exporters.cotf_onnx \
  --height 144 --width 256 --lut-dim 17 --down 8 --ch 32 \
  --checkpoint models/weights/cotf_paramnet_train/best.pt

# 用真实场景图在主机侧生成 CLUT 二进制
python -m models.tools.cotf_make_lut \
  --checkpoint models/weights/cotf_paramnet_train/best.pt \
  --input /path/to/frame.png \
  --thumb-h 144 --thumb-w 256
```

训练损失为：

- 对增强图与 target 的 L1 像素损失；
- 水平/垂直梯度 L1，减少边缘和局部对比漂移；
- LUT 邻域平滑正则；
- LUT 单调性违例惩罚，降低颜色反转和 banding 风险。

### 训练进度与断点恢复

默认每 20 个 batch 输出一次：

```text
epoch=0007/0100 batch=00020/02500 overall=6.01% loss=... \
elapsed=12m30s eta=3h15m20s finish=2026-06-20 02:35:10 CST
```

- `--epochs` 表示训练结束时的**总轮数**，不是“再训练多少轮”。
- `--log-every N` 控制 batch 进度输出频率；设为 `0` 时只输出每轮汇总。
- ETA 依据当前运行和 checkpoint 中累计的实际耗时估算，包含已经发生的验证开销；数据刚开始加载时
  波动较大，跑过若干 batch/epoch 后才稳定。
- 每轮结束写 `last.pt`；指标改善时写 `best.pt`；`--save-every` 控制周期快照。
- checkpoint 保存并恢复模型、optimizer、scheduler、AMP GradScaler、Python/NumPy/PyTorch/CUDA
  RNG、历史最佳指标、已完成 epoch 和累计训练时间。
- 恢复命令应保持模型结构、数据、batch size 和总 epoch 配置一致。恢复发生在 epoch 边界；
  当前不支持从一个 epoch 的中间 batch 继续。

## 结果

本机验证环境：

- GPU：RTX 4060 Laptop，8 GB；
- CPU：8 核 16 线程移动处理器；
- 内存：32 GB；
- 验证环境：PyTorch 2.5.1 + CUDA；仓库锁定环境为 PyTorch 2.1.0，训练 API 保留兼容写法。

功能验证：

- `python -m pytest models/tests -q`：`25 passed`。
- 单 pair、`crop=256`、2 epoch GPU 冒烟训练成功，`best.pt/last.pt` 可正常恢复和导出。
- checkpoint 导出的 FP16 ONNX 算子为
  `AveragePool/Conv/GlobalAveragePool/Clip/Constant`，无 `Resize/GridSample/Cast` 等 ONNX 红名单；
  正式权重仍须再经 ATC 转 OM 和板端 profiling，才能确认无 AICPU 回退。

纯计算基准（AMP，预热后 50 step；不含磁盘解码、验证和 checkpoint）：

| batch | crop | 每 step | 图像吞吐 | 峰值训练张量显存 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 256 | 2.23 ms | 448 img/s | 24 MiB |
| 2 | 384 | 2.65 ms | 755 img/s | 60 MiB |
| 4 | 384 | 3.73 ms | 1073 img/s | 104 MiB |
| 2 | 512 | 3.30 ms | 606 img/s | 93 MiB |

这里的显存只统计 PyTorch 分配的训练张量，不含 CUDA runtime、驱动和显示占用；不能把它当作整卡
占用。真实训练通常受 PNG/JPEG 解码、worker 数量和验证频率影响。

## 训练时间与平台评估

以当前默认 `batch=4, crop=384` 粗估：

- `10,000` 对图像、100 epoch，共 `250,000` step：纯 GPU 计算约 16 分钟；
  加数据解码、验证和保存，建议按 **0.5–2 小时**预留。
- 若改为官方 CoTF 风格的固定 `100,000` iteration：纯 GPU 约 6 分钟；
  实际建议按 **15–45 分钟**预留。
- 首次认真训练还应包含数据检查、至少 2–4 组正则/学习率实验和 20–50 张代表性板端帧评估；
  单次训练很快，但完整实验轮次建议按 **半天到一天**安排。

平台建议：

1. **首选本机 RTX 4060 8 GB**：当前网络和 384–512 crop 远未触及显存上限，避免上传数据和环境迁移，
   足以完成单次训练、调试与小规模超参搜索。
2. **多组并行实验选 RTX 4090 24 GB 或同级云 GPU**：价值主要在更大 batch 和并行 sweep，
   不是单次训练的必要条件；预计单 run 约为本机的 `0.4–0.7×`，但数据解码占比会限制加速。
3. **A100/H100 不建议作为首选**：对此约 102 万参数、低显存占用的网络明显过度配置，除非同时做
   大规模数据增强、感知损失或多实验并行。

## 解读与下一步

- 训练代码解决的是“随机权重 param-net”问题，但不会自动解决数据质量问题。双向曝光校正必须包含
  欠曝、过曝、混合高动态范围和正常曝光样本；只用低光数据会把网络训练成单向提亮器。
- 先用公开成对曝光数据预训练，再用 OS08A20 同机位采集做微调。板端数据至少保留 20–50 张独立
  验证图，且不能与训练 crop 泄漏。
- 训练后先做主机 PSNR/SSIM、LUT 单调性和高光裁剪检查，再转 FP16 OM。最后用 VPSS chn2 的
  `256x144 NV21 + AIPP` 上板，比较 checkpoint→ONNX→OM 的输出容差。
- 当前 `socchina_app` 的生产控制线程仍使用规则判决→Gamma。把 NN 真正接入生产路径还需完成：
  chn2 取帧、AIPP 模型转换、NN 输出到 Gamma/DRC/CLUT 的安全桥，以及失败时回退到现有规则控制。
