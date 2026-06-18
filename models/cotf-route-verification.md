# CoTF 路线可行性验证 + 代码实现 —— NN 出 LUT + ISP 硬件施加

> 状态：2026-06-16。**分环验证与接口代码已完成**（host 桥 + 板端 CLUT API + 控制策略）；
> **板端相机链联机点亮已完成**——相机→ISP+CLUT→HDMI 实时整链 **30fps 跑通、点屏 toggle CLUT 开/关、
> 零 NPU**（见下「板端联机点亮（2026-06-16）」）。回答 [architecture.md](../docs/architecture.md) §4.1 / §6
> 中 CoTF 3D-LUT 能否落地的问题：
> **NN 低分辨率出 3D-LUT（NPU）+ ISP 硬件全分辨率施加（零 NPU）** 这条路能不能成立。
> 这是 [expo-curve-network.md](expo-curve-network.md) 横评后唯一能突破"全分辨率访存地板"的架构。
> 四段式：目标 / 命令路径 / 结果 / 解读 / 板端联机点亮。
>
> **一句话进展**：CoTF 硬件施加端已在真板上以 30fps 实时点亮（相机→ISP CLUT→HDMI，触摸切换），
> 唯一仍开放的是 **CLUT mesh 几何标定**——主机 `cotf_lut_pack` 按 17×18×18 打包的 LUT 灌进去出彩色乱码
> （连恒等表都不透传），证实文档早标注的轴序/遍历待标定问题。当前演示用**几何无关**的绕过法
> （读硬件默认表→在输出值上叠 gamma 提亮）拿到干净可见的曝光校正。

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
- 板端实时演示：[`board/tests/test_cotf_live.c`](../board/tests/test_cotf_live.c)（相机→ISP+CLUT→HDMI 30fps +
  触摸 toggle + `--auto`/`--dumpdir`/`--probe`/`--tone`/`--lockexp`，2026-06-16）。
- 演示用 LUT 与可视化（主机）：[tools/cotf_make_demo_lut.py](tools/cotf_make_demo_lut.py)（tone-curve LUT 打包）、
  [tools/cotf_demo_apply.py](tools/cotf_demo_apply.py)（经同一桥把 LUT 施加到真实图像，输出 input/校正/gt 对比图与 PSNR）。
- mesh 标定辅助（主机）：[tools/cotf_clut_analyze.py](tools/cotf_clut_analyze.py)（分析板端回读的默认 CLUT 表结构，
  反推外层轴/内层交织/点阵均匀性，见下「CLUT mesh 几何标定调查」）。

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

4. **板端联机点亮已完成（2026-06-16，见下专节）**：相机→ISP+CLUT→HDMI 实时整链 30fps 跑通、
   触摸屏点击 toggle CLUT 开/关、零 NPU、零掉帧。`isp_set_clut`/`isp_load_clut_lut` 在**运行中的** ISP pipe0
   上即时生效，新增板端测试 [`board/tests/test_cotf_live.c`](../board/tests/test_cotf_live.c)（交叉编译，
   复用现成 capture/isp/display 模块）。

5. **施加端选块（2026-06-19 已厘清）**。最初把主机 `cotf_lut_pack`（误用 `17×18×18`）打包的 LUT 直接灌
   `isp_load_clut_lut` → **出彩色乱码**（连恒等都不透传）。后查到厂商文档:**CLUT 实为 `17×17×17` 线性 RGB
   3D-LUT,`lut[5508]=17×18×18` 是带填充存储**——是我方把填充当格点、用错维度,mesh 现可正确打包(见
   「CLUT mesh 几何标定调查」更新)。但更重要:**曝光/色调的原生块是 Gamma/DRC,不是 CLUT**。故施加端按用途分:
   - **全局色调/曝光 → ISP `Gamma`**（1D 1025 节点,`isp_gamma_apply_tone`,**已板端 31fps 实测、出图干净无伪色**,
     见「Gamma 原生色调施加」）——CoTF 曝光用例的首选,零 mesh。
   - **双向动态范围 → ISP `DRC`**（原生,已接 `isp_set_drc`）。
   - **颜色相关 3D → CLUT**（17³,现可正确打包,供色彩偏好/肤色等）。
   说明:`isp_clut_apply_tone` 的几何无关 CLUT-tone 仍保留作对照(`--block clut`),但默认与推荐是 Gamma。

> 口径：第 1 环是离线单算 OM 执行耗时（`aclmdlExecute`，warmup 后），不含 ISP CLUT 施加（那是硬件内联、
> 零 NPU，需联机测但不占 NPU 预算）。OM 文件大小不代表参数量。

## 板端联机点亮（2026-06-16）

把 CoTF 的**硬件施加端**在真板上实时点亮：相机 → ISP(+CLUT 3D-LUT 全分辨率施加) → VPSS → VO/HDMI，
触摸屏点击切换 CLUT 开/关。这回答了 §6 点 3「CoTF CLUT 相机链加载 + 热刷新」。

### 链路与交互

```
OS08A20(8M30 linear) → VI(online) → ISP(+CLUT 三线性, 硬件内联) → VPSS chn0 1024x600 NV21 → VO/HDMI(1024x600 DVI)
                              ▲ 点屏 toggle en                                              30fps
   触摸屏 /dev/input/event0 ──┘  (BTN_TOUCH 按下 → ss_mpi_isp_set_clut_attr en 翻转)
```

- 新增 [`board/tests/test_cotf_live.c`](../board/tests/test_cotf_live.c)（交叉编译，复用现成
  `capture`/`isp`/`display`/`control` 模块与 `isp_set_clut`/`isp_load_clut_lut`）：
  整链起链 + `isp_load_clut_lut(5508)` + 逐帧轮询触摸事件翻转 CLUT；
  `--auto <秒>` 自动翻转取证、`--dumpdir` 落盘 NV21、`--probe` 回读默认表、`--tone <gamma>` 几何无关提亮、
  `--lockexp <us> <again>` 锁曝光排除 AE 干扰。
- 交叉编译用仓库正规链路：`CROSS_COMPILE_ROOT=tools/local/aarch64-mix210-linux`、
  `SS928_SDK_ROOT=tools/local/mpp_sample/SS928V100_SDK_V2.0.2.2_MPP_Sample-master`、
  `ISL_LIB_DIR` 指向含 `libisl.so.19` 的目录（本机 conda env 里有），`scripts/build_board.sh Release`。

### 实测结论

| 项 | 结论 | 证据 |
| --- | --- | --- |
| **相机→ISP+CLUT→HDMI 实时整链** | ✅ **30.2fps 稳定**，CLUT 开关全程不掉帧 | `test_cotf_live` 日志 `fps=30.2`、`frames=` 连续累计 |
| **CLUT 在运行中 ISP 上即时生效** | ✅ 独立逻辑在 pipe0 上 `set_clut_attr/coeff` 立即改变出图 | toggle 后下一帧即变（落盘帧对比） |
| **点屏 toggle** | ✅ 触摸 `/dev/input/event0` `BTN_TOUCH` → 翻转，实测连续 15+ 次稳定 | 日志 `toggles=` 随点击递增、`CLUT ON/OFF` 交替 |
| **CLUT 施加零 NPU、不被 AE 抵消** | ✅ AE 在 CLUT **之前**测光：开关 CLUT 时 AE 直方图 luma 不变（恒 ~53） | 日志 `luma mean` 不随 `clut=ON/OFF` 变 |
| **热刷无 flicker** | 🟡 整链稳定无错，flicker 观感需现场目视最终确认 | 进程长跑稳定、无 MPI 报错 |
| **任意 NN-LUT 精确落地** | ❌ 受阻于 **mesh 几何标定**（见解读 §5） | 主机打包 LUT/恒等 LUT 均不透传，出伪色 |

### 几何无关绕过法（拿到干净可见效果的关键）

直接灌主机打包的 LUT 出乱码后，改用**不依赖 mesh 几何**的办法做出可见校正：

1. `ss_mpi_isp_get_clut_coeff(pipe0)` 读回硬件**默认 CLUT 表**——它的节点↔坐标映射、位打包都是硬件原生正确的。
2. 对每个节点解包出 `(R,G,B)`（各 10bit），施加 `out = 1023·(out/1023)^γ`（γ<1 提亮暗部），再打包回去。
   这是逐节点**输出值**的点变换，与"哪个 index 对应哪个 (r,g,b)"完全无关 → 几何错不影响。
3. `isp_load_clut_lut` 写回、`set_clut_attr en=1` 启用。

实测（锁曝光排除 AE 后）：默认表本身即一条提亮曲线（开 CLUT 后 luma ~+14）；再叠 γ=0.45 暗部进一步抬亮，
出图干净、无伪色、暗部细节找回。**局限**：这是 gamma 全局色调，不是训练后的 CoTF LUT；
真正的「NN 出任意 3D-LUT」必须先解决 mesh 标定。

### CLUT mesh 几何标定调查（2026-06-17）

为解出主机 LUT 的 linear-index↔(r,g,b) 映射，新增 `test_cotf_live --dumplut <bin>` 回读硬件**默认 CLUT 表**
（正确几何，但格式未知），离线用 [tools/cotf_clut_analyze.py](tools/cotf_clut_analyze.py) 分析结构：

| 观察 | 结论 |
| --- | --- |
| lag-324(=18²) 的 mean\|diff\| 骤降（~255 vs 其它 ~450） | **外层轴 = 17（stride 324）确认** |
| 内层 18×18 平面呈嵌套 period-2 交织（`node[4]≈node[6]`…） | 内层非简单 row-major；去交织仅把 TV 1076→814，**未干净收敛** |
| **灰轴节点 (R==G==B) = 617 个**（均匀 RGB 立方应 ~17） | **CLUT 不是均匀立方 RGB 点阵**，而是厂商特定非均匀/感知点阵 |
| 默认表 `node[0]=(0,0,0)`、`node[1..3]` 像轴端点、整体是一条已调好的色调表 | 默认表是**非恒等**的 tuned LUT，无法直接读出坐标 |

**判定**：在没有厂商《ISP CLUT 调优说明》的前提下，从默认表结构**无法可靠反推**精确 index↔(r,g,b)
（点阵非均匀、内层交织、默认表非恒等三重障碍叠加），强行猜测会得到细微错误的标定。

**重要推论（影响落地策略）**：
- **曝光/色调类校正（CoTF 的核心用例，本质是全局亮度/luma 操作）→ 走「几何无关」法即可**：读默认表→对每节点
  **输出值**叠 tone 曲线（gamma / 对比 / 逐通道曲线 / 白平衡增益），与点阵几何无关，已板端跑通、出图干净。
- **仅当需要任意「与颜色相关的 3D LUT」才必须做完整几何标定**。

**更新（2026-06-19，厂商文档已找到，结论修正）**：在 SDK ReleaseDoc 找到
《ISP 图像调优指南》§4.20 与《ISP 颜色调优说明》§4.2：
- **CLUT 是 `17×17×17` 均匀分格的线性 RGB 3D-LUT**（17³=4913 逻辑节点；`lut[5508]=17×18×18` 是
  **带填充存储**，外层 17 平面、每平面 17×17 填进 18×18）——与上面 strace-free 测到的"外层 17、stride 324=18²"
  完全吻合。**mesh 几何不再是无解之谜**：我方 `cotf_lut_pack` 用 `(17,18,18)` 把填充当成了格点，故恒等不透传；
  修法明确（重采样到 17³ + 按 18 填充写存储 + 恒等验证），**不再需要外部资料**。
- 但**更关键的认知**：CLUT 在文档里属 **AWB+CCM+CLUT+Gamma+CA 颜色链**，是给**颜色准确性/偏好色**用的，
  靠 PQ Tools 离线从 RGB 色对生成。CoTF 的**曝光/色调**用例，原生块是 **Gamma**（§4.11，1D、1025 节点均匀、
  `USER_DEFINE` 可写）与 **DRC**（§4.6，双向动态范围压缩）。→ **施加端应走 Gamma/DRC，而非 CLUT**
  （见下「Gamma 原生色调施加（首选路径）」），从根上绕开 CLUT mesh。

复现：`./test_cotf_live --dumplut /root/.../clut_default.bin` → 拉回 →
`python -m models.tools.cotf_clut_analyze clut_default.bin`。

### 复现命令（板端）

```sh
# 交叉编译（仓库根，先 source 出三个环境变量）
scripts/build_board.sh Release
scp build/test_cotf_live root@<board>:/root/socchina-2026/

# 板端：实时演示，点屏切换（gamma 0.45 暗部提亮，几何无关）
./test_cotf_live 1800 --tone 0.45                  # 1800s，触摸 toggle
# 取证：自动每 3s 翻转并落盘 on/off 帧
./test_cotf_live 9 --auto 3 --tone 0.5 --dumpdir dumps
# 诊断：回读硬件默认 CLUT 表
./test_cotf_live 4 --probe
```

## 场景自适应曝光校正（2026-06-17，模型无关版整链）

把 CoTF 路线接成真正的闭环：**AE 统计 → `control_decide` 判决 → ISP CLUT tone 施加**，即
architecture.md §2「场景自适应控制大脑」的实现。暂以"AE 统计→规则判决"替代"NN 出 LUT"（NN 的全局
色调本就可表达为这套曲线），先验证 控制环→CLUT→显示 的端到端实时校正。

### 几何无关的 tone 施加（绕开未标定 mesh）

新增 ISP 接口（[isp.c](../board/src/isp.c)/[isp.h](../board/include/isp.h)）：
- `isp_clut_get_coeff`：回读运行中 ISP 的默认 CLUT 表作为**几何正确基线**。
- `isp_clut_apply_tone(base, len, tone, strength)`：对基线表的**每个节点输出值**叠 tone 曲线
  （`BRIGHTEN` gamma<1 提暗部 / `COMPRESS` gamma>1 压高光 / `BIDIR` log 曲线压缩动态范围 / `BYPASS` 关），
  再 `isp_load_clut_lut`+`isp_set_clut(en)`。逐节点点变换 ⇒ **与 mesh 几何无关**，绕开未标定的
  index↔(r,g,b)（见上「CLUT mesh 几何标定调查」）。`control_decide` 的 4 个模式 1:1 映射到 4 条 tone 曲线。

### Gamma 原生色调施加（首选路径，2026-06-19 板端实测）

依据厂商文档，曝光/色调的原生块是 **Gamma**（非 CLUT）。新增 [isp.c](../board/src/isp.c)/[isp.h](../board/include/isp.h)：
- `isp_gamma_apply_tone(tone, strength)`：首次缓存上电默认 Gamma 表作基线，把 tone 曲线叠在其**输出**上，
  `curve_type=USER_DEFINE` 写回 `ss_mpi_isp_set_gamma_attr`；`BYPASS` 还原默认。R/G/B 共用同一 Gamma 表
  ⇒ 等效全局色调。**1D、1025 节点，与 3D mesh 几何完全无关**——比 CLUT-tone 更干净、零标定风险。
- `test_cotf_auto --block gamma`（默认）即走此路；`--block clut` 为对照；`--dumpdir` 落 before/after 帧取证。

板端实测（OS08A20 实时相机，`test_cotf_auto 14 --block gamma --dumpdir`）：

| 项 | 结论 |
| --- | --- |
| 整链 30fps | ✅ **31.2fps**，AE→`control_decide`(实测 BRIGHTEN/BIDIR 随场景)→Gamma tone，零 NPU |
| 基线读取 | ✅ `base=default-table`：成功回读默认 Gamma 表并在其上叠 tone |
| 效果干净（无伪色） | ✅ 落盘帧:OFF mean luma **89.5** → BRIGHTEN **149.0**，RGB 三通道同比例提亮(98,90,80→158,150,139)，**无 CLUT 那种彩色乱码** |

→ **CoTF 施加端按用途分块**：全局色调/曝光 → **Gamma**（干净、零 mesh，已实测）；双向动态范围 → **DRC**；
颜色相关 3D → CLUT（17³，已可正确打包）。NN 只需出"全局色调曲线"直接写 Gamma 表,即 CoTF"低分辨率出参数
+ 硬件全分辨率施加、零 NPU"的本意,且彻底绕开 CLUT mesh 复杂度。

### 两种部署模型（各有板端验证状态）

| 模型 | 文件 | 数据通路 | 控制面 | 板端状态（2026-06-17） |
| --- | --- | --- | --- | --- |
| **集成式** | [test_cotf_auto.c](../board/tests/test_cotf_auto.c) | 自带 capture+VPSS+display | 同进程读 AE 统计→判决→CLUT tone | ✅ **已板端 live 跑通**：相机→ISP+CLUT→HDMI **30.2fps** 稳定，AE 统计→`control_decide`(实测判 BIDIR)→几何无关 CLUT tone，454 帧/15s PASS |
| **独立控制面** | [test_cotf_ctrl.c](../board/tests/test_cotf_ctrl.c) | 由别的进程提供（厂商 sample_vio） | 独立进程只在运行中 ISP pipe0 上施加 CLUT | ✅ **已板端 live 跑通**：sample_vio 出相机→HDMI，本进程 cross-process 注入 tone，4 模式 `apply rc=0`、sample_vio 存活、HDMI 不掉链 |

### 板端实测结论（独立控制面，2026-06-17）

| 项 | 结论 | 证据 |
| --- | --- | --- |
| **cross-process CLUT 注入** | ✅ 独立进程 `isp_clut_get_coeff`/`apply_tone` 在厂商 sample_vio 运行中的 ISP pipe0 上成功生效 | 4 次 `tone -> {BRIGHTEN,COMPRESS,BIDIR,BYPASS} apply rc=0`，sample_vio(pid) 全程存活、HDMI rsen/phy=YES |
| **几何无关 tone 出干净校正** | ✅ 不再出彩色乱码（绕开 mesh） | tone 施加在默认表输出值上，逐节点点变换 |
| **零 NPU、不掉链** | ✅ CLUT 硬件内联，控制进程只低频写参数 | HDMI DVI 链路稳定 |
| **AE 统计 cross-process 读取** | ❌ 被数据通路进程的 3A 独占（`get_ae_stats` 0xa01c8045） | 故独立控制面用触摸**手动循环** tone 模式；场景自适应（AE 驱动）需集成式或与 3A 协商 |

### 已知限制

- **AE 统计单消费者**：cross-process 读 AE 统计（`get_ae_stats`）与占用数据通路进程的 3A 冲突
  （0xa01c8045）。故**独立控制面**无法读 AE 做场景自适应（改触摸手动循环 tone）；**AE 驱动的场景自适应
  走集成式**（同进程持有 ISP 3A，已实测 OK，见上表）。

> 排查留痕：开发期曾遇相机不出帧（`ss_mpi_vpss_get_chn_frame` 0xa0078016），一度怀疑是 capture 软件
> 起链问题；最终用 `VI int_cnt`/`MIPI freq` 实测定位为 **sensor MIPI 数据通道/上电瞬态故障**
> （I2C+时钟正常、MIPI 数据 lane 无信号），**断电重上电 + 重插排线后恢复**，集成式整链随即 30fps 跑通。
> 教训：判断相机是否出流看 `VI int_cnt`/`MIPI freq`，不能看时钟寄存器。

### 复现命令（板端）

```sh
# 集成式（自带整链 + AE 场景自适应，已板端 30fps）
/root/socchina-2026/test_cotf_auto 600 --strength 0.7                 # 点屏 toggle 自动校正 开/关
# 独立控制面：先起厂商相机预览，再 cross-process 注入 CoTF tone（AE 被 3A 独占，用手动循环）
cd /root/os04a10-camera && SENSOR_INDEX=1 ./start_os04a10_preview.sh   # 相机→HDMI
/root/socchina-2026/test_cotf_ctrl 600 --cycle                        # 点屏循环 4 条 tone
```

## 流水线可行性（与 §6 点1 的曲线网络的本质区别）

CoTF 路线**天然适合流水线**，因为它把两件事放在**两个不同硬件块**上：

```
主路径(每帧, 30fps, 全分辨率, 0 NPU):
  VI ─► ISP(+CLUT 三线性施加, 硬件内联) ─► VPSS ─► VO/VENC ─► 显示/串流
            ▲ 当前 LUT                  │ chn2 缩略图(256x144)
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

板端联机已验证（2026-06-16，见「板端联机点亮」专节）：① `isp_load_clut_lut`/`set_clut_attr` 在跑流中
即时生效、CLUT 开关全程 **30fps 不掉帧、零 NPU**，触摸点屏 toggle 稳定（flicker 观感待现场目视终判）；
② LUT 刷新率 vs 场景适应延迟的取舍标定仍待实测；③ 任意 NN-LUT 精确落地仍卡在 mesh 几何标定。

## 一句话判决

**CoTF 路线可行且独占优势**：要在 1024x576（乃至更高）做真·实时双向曝光，这是唯一不撞访存地板的路——
NN 出 LUT（喂缩略图 ~1ms、喂全分辨率 ~5ms）+ ISP 硬件全分辨率施加（零 NPU）。更关键的是它**把模型整个移出
每帧关键路径**：显示吞吐回到纯硬件链（30fps+），NPU 仅在低频控制环刷 LUT（占空 ~3–15%），天然流水线。
代码与格式已就绪，剩 mesh 表 + 联机点亮两步板端工作。若只需快速上线，仍可先用本网络 768x432+共享曲线
(27ms，但 NPU 每帧满载)；要冲全分辨率实时 + 释放 NPU，则投 CoTF 路线。
