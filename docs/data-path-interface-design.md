# Config-R 数据通路接口与协作设计

> 状态快照：2026-06-20。本文保留阶段 C 的模块边界和帧所有权契约；具体 SDK 调用以实现和板端实测为准。
> 当前实现：display + stream + 规则 Gamma + chn2/AIPP/param-net + 安全 bridge + CLUT 已进入生产
> `main.c`。连续三次刷新失败进入 sticky DEGRADED 并回退 Gamma；高光/端点/单调/最大步长护栏、
> 统计低通、变化持续确认和刷新后冷却均已实现。尚未完成的是 10 分钟正式验收和闭环画质评估。
> **命名说明**：文中「CoTF」指受官方 CoTF 启发、只保留「预测 3D-LUT + ISP 硬件施加」的子集，**非官方 CoTF**
> （协同变换/自适应采样/注意力融合等红名单部件已丢弃，画质 ≈ 全局 3D-LUT 级）。详见
> [architecture.md §4.1](architecture.md) 与 [../models/cotf-route-verification.md](../models/cotf-route-verification.md)。

## 1. 目标与验收边界

目标：

- 保持 `OS08A20 -> VI/ISP -> VPSS -> VO/HDMI` 主链 30 fps。
- CoTF-inspired 参数网只在低频控制旁路读取缩略图、预测参数；全分辨率增强由 ISP 参数块每帧完成。
- 让显示、推理、LUT 桥、控制和串流可由不同成员并行开发，避免共享帧和生命周期互相耦合。

阶段 C（M3）验收目标：

| 指标 | 目标 |
| --- | --- |
| 主链帧率 | 连续 10 分钟平均不低于 29.5 fps |
| 控制轮询 | 默认每 3 帧一次，约 10 Hz |
| 256x144 模型执行 | `aclmdlExecute` p95 不高于 3 ms；输入/输出复制计入完整刷新事务 |
| 单次 LUT 刷新事务 | p95 不高于 10 ms，不阻塞显示线程 |
| 内存行为 | 稳态无逐帧/逐次刷新 `malloc`，无借用帧泄漏 |
| 故障行为 | 推理或 CLUT 刷新失败时保留旧 LUT，显示主链继续运行 |

这里的“30 fps”指相机帧链，不是 HDMI 的 60 Hz 扫描刷新率。M3 不要求严格同帧的原图/增强图分屏：
ISP Gamma/DRC/CLUT 位于公共 ISP 输出上，同一 ISP 输出天然都是已增强帧。

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
| `main/pipeline` | `pipeline_config_t`、退出信号 | 配置默认值/校验、SYS/VB 与模块生命周期、线程创建/回收、汇总指标 | 图像算法和 SDK 细节 |
| `capture` | sensor/WDR 配置 | VI/ISP 启停、VI→VPSS 绑定 | VPSS 取帧 |
| `vpss` | 三通道配置 | 借出/归还 SDK 帧 | 跨线程队列 |
| `display` | `chn0` 借用帧 | VO/HDMI 送显 | 归还 VPSS 帧 |
| `infer` | `chn2` NV21 借用帧 | 调用者提供的 float 参数 | 决策、ISP 写入、保留输入帧 |
| `lut_bridge` | NN 参数 | Gamma/DRC/CLUT 安全输入 | ISP 调用、动态分配 |
| `control` | AE 统计、历史状态 | 模式和是否刷新 LUT 的纯逻辑决策 | 取帧、推理、线程 |
| `isp` | ISP 参数和 packed LUT | 读取统计、更新 Gamma/DRC/dehaze/CLUT | 刷新策略 |
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
  │                                      -> safe bridge -> ISP parameter set]
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

## 5. 参数刷新事务

control worker 默认每 3 帧执行一次控制轮询，但只有 `control_should_refresh_lut` 判定需要刷新时才取
`chn2` 和运行 NPU：

1. `isp_get_luma_stats` 读取 AE 统计。
2. `control_decide` 更新曝光场景模式。
3. `control_should_refresh_lut` 根据变化量、最小间隔和迟滞决定是否刷新。
4. 从 `chn2` 取一帧，超时则本轮跳过。
5. `infer_run_nv21` 同步输出 14,739 个 float。
6. 无论推理成功与否，立即归还 `chn2` 帧。
7. 成功时由安全参数桥做有限值检查、clamp、单调/变化量护栏和格式转换。
8. 按用途更新 Gamma/DRC/CLUT；当前 17³ 输出对应颜色 CLUT，曝光生产路径优先 Gamma/DRC。
9. 仅在 ISP 写入成功后提交“当前参数版本”和成功时间；失败时保留旧参数并回退规则控制。

当前模型输出布局固定为 `[RGB][R][G][B]`，B 轴最快，共 `3 * 17^3 = 14,739` 个 float。
厂商文档确认逻辑 CLUT 为 17³；PQTools `17v2` 实现进一步确认 5,508 项是 8 个奇偶 bank 的
4 路交织存储，不是 `17×18×18` 逻辑采样网格或边界填充。RGB 轴序和 R高/G中/B低位序必须
集中在参数桥，不得散落到推理或 ISP 模块。

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

生产程序对 ACL/SMMU 致命错误返回 70；systemd 将 70 列为 `RestartPreventExitStatus`，避免坏上下文
上的无限重启。配置错误返回 2，同样等待运维修正后显式 restart。其它启动期瞬态失败保留自动重试。

## 9. CLUT 上板验证门

训练模型接入的验证门及当前状态：

1. ✅ Identity LUT：开关 CLUT 后无历史伪色；相邻 OFF/ON MAE 1.025、UV MAE 0.116。
2. ✅ 轴序/位序诊断：36 组 sweep 确认 RGB 轴序、R高/G中/B低与 17v2 交织。
3. 🟡 热刷新：生产线程 30 秒测试为 30.11fps、6/6 更新、0 drops；推理 p95 2.86ms、
   事务 p95 4.86ms。10 分钟正式验收与现场 flicker 待测。
4. 🟡 固定模型 LUT：Python/C identity 已对齐；仍需多组随机 LUT 逐项对拍。
5. 🟡 动态模型 LUT：生产 worker、降级和基础护栏已完成；待多场景画质终验。

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

- CLUT 逻辑网格和 17v2 存储已确认；Python/C bridge 已实现有限值、范围、identity 强度、
  高光、端点、单调性和变化量护栏，并通过 identity/轴序/位序门禁。仍需随机 LUT 对拍和多场景画质终验。
- chn2 位于 post-CLUT 路径，动态模型会读取自身增强结果。当前已加入 1/4 EMA、连续 3 次变化确认、
  刷新后 10 个控制周期冷却和 packed LUT 最大步长；仍需用动态场景验证收敛参数。
- ACL 是否能安全直接导入 VPSS/VB 物理地址尚未验证；当前基线是缩略图 copy。
- ISP CLUT 主线不能直接提供严格同帧原图/增强图分屏；需要额外 ISP/旁路能力或整图备选。
- RTSP server 已选仓库内轻量实现：单客户端、RTP/RTSP over TCP interleaved、无第三方运行依赖；
  厂商 MPP sample 仅提供 VENC/落文件代码，没有可小范围复用的 RTSP server。实现已通过主机协议
  单测、SDK Release 交叉编译、纯串流播放/重连/资源验收和 HDMI 并行门禁；并行显示约
  30.1–30.5fps、stream drops=0，systemd 开机自动恢复也已通过。
- `chn1` 若用于整图模型备选，必须在配置层禁用 stream，第一版不做动态抢占。
