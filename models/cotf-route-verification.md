# CoTF 路线可行性验证 + 代码实现 —— NN 出 LUT + ISP 硬件施加

> 状态：2026-06-15。**分环验证与接口代码已完成**（host 桥 + 板端 CLUT API + 控制策略）；
> 尚未完成相机链端到端联机。回答 [architecture.md](../docs/architecture.md) §4.1 / §6
> 中 CoTF 3D-LUT 能否落地的问题：
> **NN 低分辨率出 3D-LUT（NPU）+ ISP 硬件全分辨率施加（零 NPU）** 这条路能不能成立。
> 这是 [expo-curve-network.md](expo-curve-network.md) 横评后唯一能突破"全分辨率访存地板"的架构。
> 四段式：目标 / 命令路径 / 结果 / 解读。

## 目标

全分辨率输出的模型都撞像素线性地板（1024x576 ≥33ms，见 expo-curve-network.md 速率矩阵）。CoTF 思路
是把"出参数"和"施加"解耦：NN 只在低分辨率预测 3D-LUT，全分辨率的 LUT 施加交给 ISP 硬件 CLUT。
本验证要逐一坐实这条链的每一环是否真能在 SS928 上成立。

## 命令路径

```sh
# === 验证用命令 ===
# A. NN 端（出 LUT 参数）—— 实测 NPU 耗时
python -m models.networks.cotf                              # 结构自检（param-net + LUT-apply）
python -m models.exporters.cotf_onnx --height 144 --width 256
#   → ATC → OM → 板端 acl_om_benchmark（耗时见结果）
# B. NPU 能否做全分辨率 3D-LUT 施加？
#    torch.onnx.export(LUTApply via F.grid_sample 3D) → 直接报错（见结果）
# C. 硬件 CLUT 端（核对自 SDK 头 + 板端 lib）
grep ss_mpi_isp_.*clut  tools/local/.../include/hisilicon/ss_mpi_isp.h
ssh root@board 'grep -ao ss_mpi_isp_set_clut_coeff /root/display-test/lib/libss_isp.so'
# E. 板端代码集成（isp.c）+ 交叉编译/链接验证
aarch64-mix210-linux-gcc -DWITH_SS928_SDK -c board/src/isp.c ...   # 编译 OK
scripts/build_board.sh Release                              # 默认(SDK-free)整包构建 OK
scripts/test_host.sh                                        # control 主机单测（含 LUT 刷新策略）

# === 跑通整条 NN→硬件 LUT 桥（host 端，已可端到端运行）===
python -m models.tools.cotf_make_lut                        # param-net→立方LUT→5508节点u32→.bin
#   → models/weights/cotf_clut.bin（22032B = 5508×u32），板端 fread → isp_load_clut_lut(lut,5508)
python -m pytest models/tests/test_cotf_lut_pack.py -q      # 打包桥单测（9 passed）
```

## 结果

链上 6 环逐一验证（板端实测 / 编译链接 / SDK 头核对）：

| # | 环节 | 结论 | 证据 |
| --- | --- | --- | --- |
| 1 | **NN 出 LUT 的 NPU 耗时** | ✅ **4.68ms**(LUT 9³) / **5.01ms**(LUT 17³) @1024x576 | acl_om_benchmark；干净落 AICore，无 AICPU/Cast（GlobalAvgPool→ReduceMeanD, head→FullyConnection）|
| 2 | NN 耗时与 LUT 大小无关 | ✅ 9³→17³ 仅 4.68→5.01ms | head 是 1x1 卷积@1x1 空间，可忽略；成本在低分辨率 encoder |
| 3 | **NPU 能否做全分辨率 LUT 施加** | ❌ **连 ONNX 都导不出** | `torch.onnx.export`：`Unsupported: GridSample with 5D volumetric input`（3D-LUT 三线性=grid_sample，NNN 无此算子）|
| 4 | **硬件 CLUT 存在且格式可对接** | ✅ `ss_mpi_isp_set_clut_coeff`，**5508 节点**，每节点 u32=3×10bit RGB，+全局增益；三线性插值在硬件内做 | SDK 头 `ot_common_isp.h`/`ot_isp_define.h` |
| 5 | CLUT API 在**本板**真实可链 | ✅ 板端 `/root/display-test/lib/libss_isp.so` 导出全部 4 个 clut 函数 | `nm -D` 显示 `T ss_mpi_isp_{set,get}_clut_{attr,coeff}` |
| 6 | **板端集成编译+链接** | ✅ `isp_set_clut`/`isp_load_clut_lut` 加进 isp.c，`-DWITH_SS928_SDK` 编译 OK；链接 libss_isp.so 出 ARM 可执行，clut 符号解析；SDK-free 整包构建 OK | 见命令路径 E |
| — | NN→硬件 LUT 打包 | ✅ `tools/cotf_lut_pack.py` 立方 LUT→u32，10bit 量化往返到 `2^30-1` 吻合硬件 range | LUT 打包单测 |

新增代码（CoTF 路线完整代码集）：
- NN：[networks/cotf.py](networks/cotf.py)（param-net + LUT-apply 探针）、
  [exporters/cotf_onnx.py](exporters/cotf_onnx.py)（可复现导出）。
- NN→硬件桥：[tools/cotf_lut_pack.py](tools/cotf_lut_pack.py)（resample→pack→write_bin）、
  [tools/cotf_make_lut.py](tools/cotf_make_lut.py)
  （端到端 param-net→`cotf_clut.bin`）、[tests/test_cotf_lut_pack.py](tests/test_cotf_lut_pack.py)（9 单测）。
- 板端 CLUT API：`isp_set_clut`/`isp_load_clut_lut`（[isp.c](../board/src/isp.c)/[isp.h](../board/include/isp.h)）。
- 控制策略：`control_should_refresh_lut`（[control.c](../board/src/control.c)/[control.h](../board/include/control.h)，主机单测）。

### NN 端耗时 vs 输入分辨率（param-net, lut_dim=17, 板端实测）

NN 只预测**全局 LUT**，输入不必全分辨率——喂缩略图即可，输出分辨率由 ISP 施加决定、与 NN 无关。

| param-net 输入 | NPU ms | 说明 |
| --- | ---: | --- |
| 1024x576 | 4.99 | 即便喂全分辨率也只要 ~5ms |
| 768x432 | 3.06 | |
| 640x360 | 2.28 | |
| 512x288 | 1.64 | |
| 384x216 | 1.17 | |
| **256x144（缩略图）** | **0.80** | 喂 VPSS chn2 级缩略图，NN 成本≈1ms，**与显示分辨率解耦** |

耗时随**输入**像素增长（AvgPool 要读全图），但非线性（有固定 encoder 底 ~0.5ms）；关键是 NN 可只吃缩略图
→ **不论最终显示是 1024x576 还是更高，NN 都只要 ~1ms**。

## 解读

1. **CoTF 路线在 SS928 上成立，且是唯一突破访存地板的架构**：NN 端只要 **~5ms**（比任何全分辨率输出
   模型快 ~20×，因为只在低分辨率出参数），全分辨率的重活（3D-LUT 三线性施加）交给 ISP **专用硬件**做，
   **零 NPU、零 NPU 侧全分辨率访存**。这与 architecture.md §4.1 的主线一致，现已完成分环验证。

2. **"NPU 做 LUT 施加"被彻底排除**：3D-LUT 三线性 = grid_sample，**连 ONNX 都导不出**，更别说落 AICore。
   所以施加**必须**走 ISP CLUT 硬件——而硬件恰好提供了这块缺失算子（内部三线性插值）。
   对 architecture.md §6 的结论是：**当前 NPU 导出路径不可行，ISP 硬件接口可用**。

3. **分环代码已完成（host 桥 + 板端 API + 控制策略）**，但尚不能称为端到端跑通：

   | 层 | 文件 | 状态 |
   | --- | --- | --- |
   | NN（出 LUT） | `networks/cotf.py` / `exporters/cotf_onnx.py` | ✅ 导出干净、板端 ~1–5ms |
   | NN→硬件桥 | `tools/cotf_lut_pack.py` / `tools/cotf_make_lut.py` | ✅ 可跑、9 单测过 |
   | 板端 CLUT API | `isp.c`/`isp.h`：`isp_set_clut` / `isp_load_clut_lut` | ✅ 编译+链接验证 |
   | 控制策略 | `control.c`/`control.h`：`control_should_refresh_lut`（限流/迟滞，把 NN 移出每帧路径） | ✅ 纯逻辑、主机单测过 |

   `python -m models.tools.cotf_make_lut` 已能
   `param-net(缩略图)→立方 LUT→5508 节点 u32→cotf_clut.bin`；板端 `fread(.bin)`
   即可喂 `isp_load_clut_lut`。控制环用 `control_should_refresh_lut` 决定何时重跑 NN 刷 LUT。

4. **剩两步（板端联机，属阶段 C/D 整链）**：
   - **mesh 标定**：`cotf_lut_pack.HW_MESH_DIMS`（默认 17×18×18）与遍历顺序需对照 SDK《ISP CLUT 调优说明》
     最终确认——**只改常量，不动主流程**。10bit 位打包格式已坐实。
   - **联机点亮**：相机链（capture_init→ISP pipe0）上 `isp_set_clut(AUTO,..)` + `isp_load_clut_lut(.bin,5508)`，
     目视确认出图随 LUT 改变、热刷无 flicker、零 NPU。需 `infer.c`（板端 ACL 跑 param-net）就位后并入整链。

> 口径：第 1 环是离线单算 OM 执行耗时（`aclmdlExecute`，warmup 后），不含 ISP CLUT 施加（那是硬件内联、
> 零 NPU，需联机测但不占 NPU 预算）。OM 文件大小不代表参数量。

## 流水线可行性（与 §6 点1 的曲线网络的本质区别）

CoTF 路线**天然适合流水线**，因为它把两件事放在**两个不同硬件块**上：

```
主路径(每帧, 30fps, 全分辨率, 0 NPU):
  VI ─► ISP(+CLUT 三线性施加, 硬件内联) ─► VPSS ─► VO/VENC ─► 显示/串流
            ▲ 当前 LUT                  │ chn2 缩略图(320x180)
            │                           ▼
旁路(低频, 异步, 不在关键路径):
  control 线程: 缩略图 ─► NN(NPU, ~1ms) ─► cotf_lut_pack ─► isp_load_clut_lut(刷新 LUT)
```

三个层次的并行，逐条说明为什么成立：

1. **引擎级并行**：LUT 施加在 **ISP 硬件**（每帧内联，零 NPU、零额外延迟），NN 在 **NPU**。两者是不同
   硬件块，第 N 帧的 ISP 施加与第 N+1 次的 NN 推理**自然重叠**，互不抢占。

2. **速率解耦（关键）**：场景光照变化慢，**LUT 不必每帧更新**。ISP 按 30fps 对每帧施加当前 LUT（全分辨率、
   免费）；NN 只在**控制环速率**（如 5–15Hz 或场景突变时）刷新 LUT。**30fps 显示路径从不触碰 NPU**。
   - NPU 占用：即便每帧（30Hz）都跑 NN，也只有 `30×5ms=150ms/s = 15%` 占空；喂缩略图（~1ms）则 `~3%`。
     **NPU 基本空闲**，剩余算力可留给别的模型/任务。

3. **落到现有架构零改动**：这条旁路就是 architecture §2 的「场景自适应控制大脑」`control.c`——它**已经**在
   低频读 ISP AE 统计、写回 ISP 参数。CoTF 的 NN 只是往这个已有的低频控制环里再加一步「缩略图→NN→
   `isp_load_clut_lut`」。`infer.c` 不再需要把每帧塞进 NPU。

**与曲线网络（输出整图）的本质对比**：

| | 曲线网络 / SCI（输出整图） | CoTF（出 LUT + ISP 施加） |
| --- | --- | --- |
| 每帧是否过 NPU | **是**，全分辨率施加在 NPU | **否**，施加在 ISP 硬件 |
| NPU 占用 | ~100%（27–95ms/帧，gating） | ~3–15%（仅低频刷 LUT） |
| 显示吞吐受限于 | **NPU**（模型耗时） | **ISP/VPSS/VO 硬件**（已实测 30fps） |
| NN 延迟影响 | 直接进每帧关键路径 | 仅影响 LUT **适应速度**，不进每帧路径 |
| 流水线 | 可做 stage 流水，但 NPU 是瓶颈且满载 | NPU 离开关键路径，吞吐由硬件链决定 |

→ **结论：CoTF 不只是"能流水线"，而是把模型整个移出每帧关键路径**，显示吞吐回到纯硬件链（30fps+），
NPU 近乎空闲。这是曲线网络做不到的——后者 NPU 必须为每帧全分辨率施加买单。

待联机验证的两点（板端）：① `isp_load_clut_lut` 在跑流中热刷 LUT **无 flicker**（类比 VGS 合成的目视确认）；
② LUT 刷新率 vs 场景适应延迟的取舍标定。

## 一句话判决

**CoTF 路线可行且独占优势**：要在 1024x576（乃至更高）做真·实时双向曝光，这是唯一不撞访存地板的路——
NN 出 LUT（喂缩略图 ~1ms、喂全分辨率 ~5ms）+ ISP 硬件全分辨率施加（零 NPU）。更关键的是它**把模型整个移出
每帧关键路径**：显示吞吐回到纯硬件链（30fps+），NPU 仅在低频控制环刷 LUT（占空 ~3–15%），天然流水线。
代码与格式已就绪，剩 mesh 表 + 联机点亮两步板端工作。若只需快速上线，仍可先用本网络 768x432+共享曲线
(27ms，但 NPU 每帧满载)；要冲全分辨率实时 + 释放 NPU，则投 CoTF 路线。
