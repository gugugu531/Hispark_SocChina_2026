# 文档索引

| 文档 | 说明 |
| --- | --- |
| `development-guide.md` | 开发规范主文档：目录约定、开发环境与依赖、CMake 构建、编码规范、模型规范、板端运行规范、Git 规范、文档规范、测试规范。 |
| `architecture.md` | 系统架构与完整图像增强链数据通路、逐级明细、两套运行配置、已核实 SDK 接口与约束。 |
| `data-path-interface-design.md` | Config-R 阶段 C 协作契约：通道分配、模块接口、线程/帧所有权、LUT 刷新事务、降级策略和并行开发验收。 |
| `model-route-summary.md` | 模型路线稳定结论：整图访存瓶颈、AIPP 实测、CoTF 主线和待验证边界。 |
| `board-operations.md` | 板端部署、启动/重启、健康检查、故障恢复手册。 |
| `TODO.md` | 未完成部分清单与阶段化开发规划（含里程碑判据与风险提示）。 |

## 更新约定

- 稳定结论、规范、架构写入 `docs/`。
- 各模块实现细节、命令、板端路径写入对应模块目录下的 `README.md`。
- 记录风格统一采用：目标（Goal）/ 命令路径（Command）/ 结果（Result）/ 解读（Interpretation）。
