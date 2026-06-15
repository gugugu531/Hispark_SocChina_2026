# artifacts — 生成结果索引

本目录保存实验汇总、板端日志、profiling 数据、采集结果和对比输出。除本索引外，目录内容默认不入 Git。

## 当前产物

| 路径 | 来源 | 对应结论 |
| --- | --- | --- |
| `expo_curve_speed_matrix.csv` | ExpoCurveNet、Zero-DCE Lite、SCI、MSEC、CoTF 板端测速汇总 | `docs/model-route-summary.md`、`models/expo-curve-network.md` |

## 约定

- 小型汇总数据也默认保留在本目录，不直接混入源码目录。
- 稳定结论必须写入 `docs/`，实验命令和实现细节写入对应模块 README 或报告。
- 文档引用产物时应同时说明生成命令、测量条件和口径；产物本身不能替代结论记录。
- 原始模型、长日志、profiler 文件夹和相机采集不得提交。
