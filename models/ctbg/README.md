# CTBG 模型模块

CTBG（Coefficient-based Tone-mapping with Bilateral Grid）是本仓库的**主增强路线**，
替代早期 CoTF param-net → ISP CLUT 方案，实现逐像素空间自适应双向曝光校正。

## 架构

```
VPSS chn2 256×144 NV21 → [CPU NV21→RGB fp16] → estimator OM → 6ch raw 系数
    ↓
host CPU nearest-neighbor 预上采样 144×256 → 576×1024
    ↓
VPSS chn1 1024×576 NV21 → [AIPP NV21→RGB fp16] → apply OM → RGB fp16 → VENC/RTSP
```

### 两代架构

| 版本 | 系数通道 | Apply 上采样 | Apply OM 大小 | 板端耗时 | 帧率 | 状态 |
|---|---|---|---|---|---|---|
| **v8 18ch** | 18（每通道 a/b/g） | OM 内部 ConvTranspose×2 | 269KB | 33.6ms | 29.8fps | 已验证，备选 |
| **v9 6ch** | 6（标量 a/b/g） | host 侧最近邻预上采样 | 140KB | 20.5ms | 48.7fps | ✅ 推荐主线 |

### v9 6ch 模型细节

- **Estimator**：MobileIE backbone（12ch, down=4）+ tail Conv2d(12→6) + FiLM（mean_luma 全局调制）
  - 输入：NCHW fp16 RGB 256×144
  - 输出：6ch raw logits（a_d, b_d, g_d, a_b, b_b, g_b）× 144×256
  - 参数：53.3K（v8 的 74%）
  - 板端实测：3.84ms

- **Apply**：纯 elementwise 操作，无 ConvTranspose
  - 输入 0：NCHW fp16 RGB 1024×576（NV21→RGB 由 AIPP 完成）
  - 输入 1：6ch fp16 预上采样系数 576×1024
  - 输出：NCHW fp16 RGB 1024×576
  - 操作：tanh decode → luma blend → exp(g·log(x)) → clip
  - 板端实测：20.51ms
  - 对拍 PSNR：78.99dB（与训练版逐位一致）

### 执行流程

1. **控制线程（~10Hz）**：场景变化 → estimator 推理 → raw 系数 → 上采样 → 共享缓冲区
2. **流线程（每帧）**：取共享系数 → VPSS chn1 NV21 → AIPP convert → apply 推理 → RGB→NV21 → VENC

## 训练/导出命令

```sh
# 训练（GPU）
python p1_bidir/train.py --config p1_bidir/config_ctbg_v9_6ch.yaml --model ctbg6ch

# 导出 ONNX
python models/ctbg/export_ctbg_6ch.py --ckpt runs/coeff_ctbg_v9_6ch/ckpt/best-*.ckpt

# ATC 转 OM（需 conda atc 环境）
bash models/ctbg/build_ctbg_6ch_om.sh
```

## 已知局限

- **空间自适应性弱**：down=4 使每个系数格点覆盖 16×16 像素，空间变化 CV 仅 1-9%
- **欠曝能力不足（v9）**：标量系数缺少逐通道调节，欠曝场景 luma 92 vs v8 的 127
- **训练早峰过拟合**：samples_per_epoch=2000 导致每样本复用 383×，CosineAnnealingLR 衰减过慢
- 修复方向见 `experiments/lut-curve-eval/PROGRESS.md` §5.1

## 文件清单

| 文件 | 用途 |
|---|---|
| `litmodule_coeff.py` | 模型定义（CoeffNetCTBG v8 / CoeffNetCTBG_6ch v9） |
| `losses.py` | 损失函数（OutlierAware + PSNRLoss + LossLLE） |
| `export_ctbg.py` | v8 导出 + slim reparam |
| `export_ctbg_correct.py` | v8 正确版导出（先上采样再 decode） |
| `export_ctbg_6ch.py` | v9 6ch 导出 |
| `build_ctbg_correct_om.sh` | v8 ATC 构建 |
| `build_ctbg_6ch_om.sh` | v9 6ch ATC 构建 |
