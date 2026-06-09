# models — 模型训练 / 导出 / 转换

本目录包含模型的训练、ONNX 导出与 ATC→OM 转换。所有环境依赖**以海思 SS928 SDK 官方文档为依据**固化（见下），不依赖某台机器上已有的 conda 环境。

子目录（训练 / 导出 / 转换 / 权重指针等）随开发推进按需添加，起步不预先细分。权重等大件不入库，仅留 `.md` 指针。

## 两套独立环境（依据 SS928 SDK 文档）

需分开两个虚拟环境，因 protobuf/numpy 等版本要求不同。

### 1. 训练 + 导出环境

```sh
conda create -n soc-model python=3.10   # 文档要求 python>=3.8
conda activate soc-model
pip install -r models/requirements-model.txt
```

依据：海思 SS928 SDK《Yolov8 模型转换与部署》§2.1。

### 2. ATC 转换环境

```sh
conda create -n soc-atc python=3.9.2
conda activate soc-atc
pip install -r models/requirements-atc.txt
# 再单独安装 CANN 工具包：
chmod +x Ascend-cann-toolkit_5.20.t6.2.b060_linux-x86_64.run
./Ascend-cann-toolkit_5.20.t6.2.b060_linux-x86_64.run --install
source ${INSTALL_DIR}/bin/setenv.bash
```

依据：海思 SS928 SDK《Yolov8 模型转换与部署》§2.2 + 《NNN/SVP_NNN 驱动和开发环境安装指南》。

## 模型导出/转换约定

- 部署精度 **FP16 OM**；静态输入尺寸；`soc_version=OPTG`。
- 用于 AIPP 的模型须导出为 **NCHW**（`--inputs-as-nchw`），清理为单 opset、单输出。
- **禁用 Resize/插值上采样**（NNN 实测异常），上采样用 **ConvTranspose**（固定 bilinear 权重）。
- 上板前做算子探测：确认无 `AICPU`/`Cast` 残留、全部落 AICore。
- 详见 `../docs/development-guide.md` §6。
