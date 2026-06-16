# Config-R 数据通路接口与协作设计

> 状态快照：2026-06-16。本文固定阶段 C 的模块边界和协作契约；具体 SDK 调用仍以实现和板端实测为准。
> **命名说明**：文中「CoTF」指受官方 CoTF 启发、只保留「预测 3D-LUT + ISP 硬件施加」的子集，**非官方 CoTF**
> （协同变换/自适应采样/注意力融合等红名单部件已丢弃，画质 ≈ 全局 3D-LUT 级）。详见
> [architecture.md §4.1](architecture.md) 与 [../models/cotf-route-verification.md](../models/cotf-route-verification.md)。

## 1. 目标与验收边界

目标：

- 保持 `OS08A20 -> VI/ISP -> VPSS -> VO/HDMI` 主链 30 fps。
- CoTF 只在低频控制旁路读取缩略图、预测 LUT；全分辨率增强由 ISP CLUT 每帧完成。
- 让显示、推理、LUT 桥、控制和串流可由不同成员并行开发，避免共享帧和生命周期互相耦合。

阶段 C（M3）验收目标：

| 指标 | 目标 |
| --- | --- |
| 主链帧率 | 连续 10 分钟平均不低于 29.5 fps |
| 控制轮询 | 默认每 3 帧一次，约 10 Hz |
| 256x144 推理 | p95 不高于 3 ms；已有独立 benchmark 约 0.8 ms |
| 单次 LUT 刷新事务 | p95 不高于 10 ms，不阻塞显示线程 |
| 内存行为 | 稳态无逐帧/逐次刷新 `malloc`，无借用帧泄漏 |
| 故障行为 | 推理或 CLUT 刷新失败时保留旧 LUT，显示主链继续运行 |

这里的“30 fps”指相机帧链，不是 HDMI 的 60 Hz 扫描刷新率。M3 不要求严格同帧的原图/增强图分屏：
ISP CLUT 位于公共 ISP 输出上，同一 ISP 输出天然都是已增强帧。

## 2. 固定通道配置

Config-R 使用一个 VPSS 组（`grp0`）和三个固定角色通道：

| 通道 | 尺寸/格式 | 所有者 | 用途 | 建议 depth |
| --- | --- | --- | --- | --- |
| `chn0` | 1024x600 NV21，无压缩 | display worker | 本地 HDMI 显示 | 2 |
| `chn1` | 1024x576 NV21，无压缩 | stream worker | VENC/RTSP；整图模型备选可互斥复用 | 2 |
| `chn2` | 256x144 NV21，无压缩 | control worker | CoTF AIPP/ACL 输入 | 1 |

约束：

- Config-R 中 `chn1` 默认可关闭；启用串流后不得再由整图模型线程取帧。
- 每个通道只有一个取帧所有者。禁止一个 VPSS 借用帧在多个线程之间扇出。
- 若未来需要同一帧供多个消费者，优先增加硬件绑定/额外通道；不得只复制
  `ot_video_frame_info` 结构后分别归还。
- VB 块大小、stride 和池数量必须用 SDK 计算接口生成，不手算并固化字节数。

代码常量定义在 `board/include/pipeline.h`，`vpss.h` 直接复用该契约。

## 3. 模块边界

| 模块 | 输入 | 输出/副作用 | 不负责 |
| --- | --- | --- | --- |
| `main/pipeline` | `pipeline_config_t`、退出信号 | SYS/VB 与模块生命周期、线程创建/回收、汇总指标 | 图像算法和 SDK 细节 |
| `capture` | sensor/WDR 配置 | VI/ISP 启停、VI→VPSS 绑定 | VPSS 取帧 |
| `vpss` | 三通道配置 | 借出/归还 SDK 帧 | 跨线程队列 |
| `display` | `chn0` 借用帧 | VO/HDMI 送显 | 归还 VPSS 帧 |
| `infer` | `chn2` NV21 借用帧 | 调用者提供的 float LUT | 决策、CLUT 写入、保留输入帧 |
| `lut_bridge` | float LUT | 5508 个 ISP packed u32 节点 | ISP 调用、动态分配 |
| `control` | AE 统计、历史状态 | 模式和是否刷新 LUT 的纯逻辑决策 | 取帧、推理、线程 |
| `isp` | ISP 参数和 packed LUT | 读取统计、更新 DRC/dehaze/CLUT | 刷新策略 |
| `stream` | `chn1` 借用帧 | VENC H.264 + RTSP | 与其他消费者共享帧 |

新增接口：

- `board/include/pipeline.h`：通道、尺寸、状态、错误码和指标。
- `board/include/infer.h`：同步单实例推理契约。
- `board/include/lut_bridge.h`：模型 LUT 到 ISP mesh 的纯函数契约。
- `board/include/stream.h`：串流生命周期和逐帧发送契约。

接口头保持 SDK-free，SDK 类型继续通过 `void *` 传入；实现文件必须在入口处转换并校验。

## 4. 线程与帧所有权

建议线程模型：

```text
main thread
  ├─ display worker: chn0 get -> display_send -> chn0 release
  ├─ control worker: AE stats -> decide -> [chn2 get -> infer -> release
  │                                      -> pack -> ISP CLUT set]
  └─ stream worker:  chn1 get -> VENC/RTSP -> chn1 release（可选）
```

借用帧规则：

1. `vpss_get_frame` 成功后，调用者获得一个借用帧。
2. 借用帧只在当前线程、当前函数事务内有效。
3. 调用者必须向相同 `grp/chn` 恰好执行一次 `vpss_release_frame`。
4. `display_send_frame`、`infer_run_nv21` 和 `stream_send_frame` 都不接管帧所有权。
5. 即使发送、推理或编码失败，也必须先归还帧，再进入重试或退出路径。

`infer_run_nv21` 是同步调用，返回时不得保留输入帧地址或输出缓冲区地址。ACL 模型实例由 control
worker 独占，第一版不支持并发调用。

## 5. LUT 刷新事务

control worker 默认每 3 帧执行一次控制轮询，但只有 `control_should_refresh_lut` 判定需要刷新时才取
`chn2` 和运行 NPU：

1. `isp_get_luma_stats` 读取 AE 统计。
2. `control_decide` 更新曝光场景模式。
3. `control_should_refresh_lut` 根据变化量、最小间隔和迟滞决定是否刷新。
4. 从 `chn2` 取一帧，超时则本轮跳过。
5. `infer_run_nv21` 同步输出 14,739 个 float。
6. 无论推理成功与否，立即归还 `chn2` 帧。
7. 成功时由 `lut_bridge_pack` 生成 5,508 个 packed u32。
8. 调用 `isp_set_clut`/等价实现热更新 CLUT。
9. 仅在 CLUT 写入成功后提交“当前 LUT 版本”和成功时间。

模型输出布局固定为 `[RGB][R][G][B]`，B 轴最快，共 `3 * 17^3 = 14,739` 个 float。当前硬件候选
mesh 为 `17x18x18 = 5,508` 节点，遍历顺序和通道位序仍是板端标定项；因此集中在
`lut_bridge_cfg_t`，不得散落到推理或 ISP 模块。

## 6. 内存与数据搬运

第一版使用明确的 copy 模式：

- VPSS `chn2` 帧仍由 VPSS/VB 管理。
- `infer` 初始化时一次性分配 ACL 输入、ACL 输出和必要的 host staging buffer。
- 每次刷新把 256x144 NV21 有效数据复制到 ACL 输入；标称图像数据为 55,296 字节，实际逐行复制必须
  尊重 SDK frame stride。
- 模型输出为 14,739 个 float，约 58,956 字节。
- LUT bridge 输出为 5,508 个 u32，22,032 字节。
- 所有缓冲在初始化时分配，在退出时释放；稳态路径不分配内存。

`PIPELINE_INPUT_PHYS_IMPORT` 仅为实验开关。在 ACL 对 VB 物理地址导入、cache 一致性和生命周期全部
板端验证前，不得把它设为默认，也不得在文档中宣称 ACL 输入零拷贝。

## 7. 生命周期顺序

启动顺序：

1. 计算并初始化 SYS/VB。
2. `capture_init`。
3. `vpss_init`。
4. `capture_bind_vpss`。
5. `display_init`。
6. `infer_init` 和 LUT bridge 缓冲准备。
7. 创建 display/control worker。
8. 按配置初始化 stream 并创建 stream worker。
9. 状态切换为 `PIPELINE_RUNNING`。

退出顺序：

1. 设置统一 stop flag，不再发起新取帧。
2. join stream/control/display worker。
3. `stream_deinit`、`infer_deinit`、`display_deinit`。
4. `capture_unbind_vpss`。
5. `vpss_deinit`、`capture_deinit`。
6. 释放 SYS/VB，状态切换为 `PIPELINE_STOPPED`。

部分初始化失败时，`main` 必须按已完成步骤的逆序回滚。模块 `deinit` 必须可重复调用。

## 8. 错误与降级策略

| 故障 | 行为 |
| --- | --- |
| VPSS 取帧超时 | 计数并丢弃本轮，不重启整链 |
| 单次推理失败 | 保留旧 LUT，归还帧，下一刷新周期重试 |
| 连续 3 次推理失败 | 进入 `PIPELINE_DEGRADED`，暂停自动刷新，显示继续 |
| LUT 打包/CLUT 写入失败 | 保留旧 LUT，不提交版本；显示继续 |
| stream 编码/发送失败 | 丢帧并重连，不能阻塞 display/control |
| ACL SMMU/CMDQ timeout | 视为致命运行时错误，停止整链；板卡干净重启后恢复 |
| display/VO 致命错误 | 进入 `PIPELINE_FAILED` 并按逆序退出 |

建议每秒输出一行稳定指标：状态、display/stream 帧数、VPSS timeout、LUT 请求/成功/失败、最近和最大
推理耗时、最近和最大 CLUT 写入耗时。日志不得逐帧打印。

## 9. CLUT 上板验证门

训练模型接入前必须依次通过：

1. Identity LUT：开关 CLUT 后画面颜色和灰阶无明显变化。
2. 诊断 LUT：分别改变 R/G/B 和各 mesh 轴，确认通道位序、节点坐标和遍历顺序。
3. 热刷新：在 identity/诊断 LUT 间循环，确认无闪屏、撕裂和 ISP 错误。
4. 固定模型 LUT：加载离线生成的已知 LUT，确认 host 与板端 bridge 一致。
5. 动态模型 LUT：最后接入 `infer_run_nv21` 和刷新策略。

第 1–3 步失败时只修改 `lut_bridge_cfg_t`/桥接实现，不改模型输出契约。

## 10. 并行开发包与完成定义

| 工作包 | 交付物 | 完成定义 |
| --- | --- | --- |
| A 生命周期 | `pipeline.c/main.c` | 正常启动退出、任一步失败可逆序回滚、SIGINT 干净退出 |
| B 推理 | `infer.c` | 固定 NV21 输入可重复输出 LUT；无稳态分配；耗时指标可读 |
| C LUT 桥/CLUT | `lut_bridge.c` + ISP 接线 | identity/诊断 LUT 通过，mesh/位序有实测记录 |
| D 控制 | control worker | AE→判决→限流→刷新事务闭环，失败保留旧 LUT |
| E 显示 | display worker | `chn0` 连续 10 分钟不泄漏帧，平均 ≥29.5 fps |
| F 串流 | `stream.c` | 独占 `chn1`，断流不影响显示，RTSP 客户端可持续播放 |
| G 测试/部署 | scripts + README | 一键构建部署、短冒烟、日志与版本可追踪 |

各成员提交前至少运行 `scripts/test_host.sh` 和 `scripts/build_board.sh`；触硬件模块还需在对应 README
记录目标、命令/路径、结果和解读。

## 11. 尚未关闭的设计问题

- ISP CLUT 的准确 mesh 维度、坐标和 packed 通道位序仍需板端诊断 LUT 确认。
- ACL 是否能安全直接导入 VPSS/VB 物理地址尚未验证；当前基线是缩略图 copy。
- ISP CLUT 主线不能直接提供严格同帧原图/增强图分屏；需要额外 ISP/旁路能力或整图备选。
- RTSP server 复用厂商 sample 还是引入轻量实现尚未选型。
- `chn1` 若用于整图模型备选，必须在配置层禁用 stream，第一版不做动态抢占。
