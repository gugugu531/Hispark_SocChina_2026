# 模型路线稳定结论

## 目标

汇总阶段 B 的模型侧稳定结论，为后续整链实现提供单一决策入口。具体命令、算子清单和完整测速表仍保留在
[`models/expo-curve-network.md`](../models/expo-curve-network.md) 与
[`models/cotf-route-verification.md`](../models/cotf-route-verification.md)。

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
- CoTF 参数网络使用 `256x144` 缩略图时约 `0.8ms`，输出 LUT 后由 ISP CLUT 对全分辨率帧施加。
- CoTF 的参数网络、主机 LUT 打包和板端 CLUT API 已分别验证；相机链联机、mesh 顺序、热刷新与画质尚未验证。

## 解读

Config-R 主线应采用“低频参数网络 + ISP CLUT 每帧施加”。这能把 NPU 从每帧关键路径移出，并复用已经验证
达到 30fps 的 VI/ISP/VPSS/VO 硬件链。

`768x432` ExpoCurveNet 保留为整图增强备选和对照路径；`1024x576` 整图增强归入 Config-Q 或低帧率场景。

## 下一步

1. 核对 ISP CLUT 的 5508 节点 mesh 维度、轴序和遍历顺序。
2. 用 identity LUT 和诊断 LUT 在相机链上联机点亮。
3. 验证运行中热刷新是否闪烁，并测量刷新耗时。
4. 接入 CoTF 参数网络和 LUT 刷新控制线程。
5. 正式训练后使用 20–50 张代表性相机帧做画质评估。
