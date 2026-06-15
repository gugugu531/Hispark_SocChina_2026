# models — 模型实现、导出与转换

本目录保存可复现的模型结构、ONNX 导出、ATC 转换配置和模型侧测试。稳定的路线结论汇总在
[`docs/model-route-summary.md`](../docs/model-route-summary.md)，详细实验记录见：

- [`expo-curve-network.md`](expo-curve-network.md)：整图输出模型的算子、耗时和分辨率横评。
- [`cotf-route-verification.md`](cotf-route-verification.md)：CoTF 参数网络、LUT 打包和 ISP CLUT 接口验证。

## 目录内容

| 路径 | 用途 | 是否入库 |
| --- | --- | --- |
| `networks/` | ExpoCurveNet、CoTF、SCI、MSEC 的结构或部署代理实现 | 是 |
| `exporters/` | 静态 NCHW ONNX 导出、清理和 FP16 转换入口 | 是 |
| `tools/` | CoTF LUT 重采样、打包和主机侧生成工具 | 是 |
| `configs/` | AIPP 等模型转换配置 | 是 |
| `build_expo_curve_fp16_om.sh` | ATC 转 FP16 OM 的统一入口 | 是 |
| `tests/` | SDK-free 模型结构与 LUT 打包测试 | 是 |
| `weights/` | ONNX、OM、算子 JSON、LUT 二进制等生成物 | 否，仅跟踪说明文件 |
| `kernel_meta/` | ATC 临时输出 | 否 |

`models` 是 Python 包。导出器和工具统一通过 `python -m models.<子包>.<模块>` 从仓库根目录运行，
不再修改 `sys.path` 或依赖脚本当前目录。

## 当前路线

| 路线 | 状态 | 板端结论 |
| --- | --- | --- |
| ExpoCurveNet 整图输出 | 已完成结构和性能去风险，尚未认真训练 | `768x432 shared niter8 ≈27.2ms`；`1024x576` 不满足 33ms |
| CoTF 参数网络 + ISP CLUT | 接口和离线链已验证，尚未相机联机点亮 | 缩略图输入约 `0.8ms`；主推全分辨率实时路线 |
| Zero-DCE Lite / SCI / MSEC | 性能对照 | 用于证明整图输出的全分辨率访存瓶颈，不作为当前主线 |

“CoTF 已验证”仅表示参数网络可生成输出、主机侧可打包 CLUT、板端 CLUT API 可编译链接。
CLUT mesh 顺序、相机链加载、运行中热刷新和画质效果仍需板端实测。

## 环境

训练/导出与 ATC 转换使用两套独立环境，避免 protobuf、numpy 等依赖冲突。

### 训练与导出

```sh
conda create -n soc-model python=3.10
conda activate soc-model
pip install -r models/requirements-model.txt
```

### ATC 转换

```sh
conda create -n soc-atc python=3.9.2
conda activate soc-atc
pip install -r models/requirements-atc.txt

chmod +x Ascend-cann-toolkit_5.20.t6.2.b060_linux-x86_64.run
./Ascend-cann-toolkit_5.20.t6.2.b060_linux-x86_64.run --install
source "${INSTALL_DIR}/bin/setenv.bash"
```

依据：SS928 SDK《Yolov8 模型转换与部署》和《NNN/SVP_NNN 驱动和开发环境安装指南》。
环境名称只是示例，脚本不得依赖固定 conda 环境名。

## 导出与部署约定

- 部署格式：FP16 OM、静态输入尺寸、`soc_version=OPTG`。
- 用于 AIPP 的模型：NCHW 输入、单 opset、单输出。
- 禁用 `Resize`/插值上采样；需要上采样时使用固定权重 `ConvTranspose`。
- 上板前解析 OM，确认无 `AICPU`、无 `Cast`，计算算子落在 AICore。
- 模型名使用 `<任务>_<宽>x<高>_<变体>_<精度>`，例如
  `expo_curve_768x432_shared_n8_fp16.om`。
- OM 文件大小不代表参数量；报告必须记录权重来源、输入输出 shape、opset 和测量口径。
- 生成的 ONNX、OM、LUT、日志和 profiler 输出不得提交。

完整规范见 [`docs/development-guide.md`](../docs/development-guide.md) §6。

## 常用命令

```sh
# 全部模型侧主机测试
python -m pytest models/tests -q

# ExpoCurveNet 导出
python -m models.exporters.expo_curve_onnx \
  --niter 8 --filters 16 --shared \
  --width 768 --height 432 --opset 13

# CoTF 参数网络导出
python -m models.exporters.cotf_onnx --height 144 --width 256

# 主机侧生成 CLUT 验证二进制
python -m models.tools.cotf_make_lut
```

当前测试基线为 20 项：ExpoCurveNet 11 项，CoTF LUT 打包 9 项。

## 生成物

`models/weights/` 被 Git 忽略。可复现生成物的类型、命名和清理规则见
[`weights/README.md`](weights/README.md)。性能矩阵等小型汇总数据放在 `artifacts/`，并由结论文档链接；
大体积原始日志仍不入库。
