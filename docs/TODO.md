# TODO — 未完成部分与开发规划

> 状态快照日期：2026-07-01。CTBG v9 已达到数据驱动架构上限 (val_psnr 19.83)。
> 新的总路线：**ISP 参数自动调优**——神经网络预测 Gamma/DRC/LDCI/Dehaze 参数，
> ISP 硬件 30fps 全帧施加。详见 [isp-auto-tuning-prompt.md](isp-auto-tuning-prompt.md)。

## 0. ISP 参数自动调优（当前主线）

NN 从场景图像直接预测 SS928 ISP 全部可用参数（~99 维连续值：WDR + Gamma + DRC + LDCI + Dehaze），
替代人工启发式规则。ISP 硬件对每一帧施加。

核心策略：**接受"弱化版"光照统一**。SS928 ISP 不支持 per-block 参数（119 个 MPI 函数审计确认，
所有 DRC/LDCI/Gamma/Dehaze 接口均为全局寄存器）。通过 WDR（多帧融合，时间维）+
DRC（S-curve 色调映射）+ LDCI（9×9 局域直方图均衡）三级级联，可将 20:1 的大尺度光照
差异压缩至 ~2:1，配合暗部纹理增强，从观感上显著改善非均匀照明。

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase 1 | 可微 ISP 模拟器 (WDR+Gamma+DRC+LDCI+Dehaze) | ✅ 已完成 (2026-07-01) |
| Phase 1.5 | 保真度闸门：RAW 回灌 harness + 合成场景，tone/strength/LDCI/Gamma 组内秩相关全 +1.000 | ✅ PASS (2026-07-03) |
| Phase 1.8 | 残差校准网络 v1：sim+R 校准代理，留出集 15.5→27.4 dB | ✅ PASS (2026-07-03) |
| Phase 2 | ISP ParamNet 训练 + 消融（训练环境 = 校准代理） | 🔶 首轮完成 (2026-07-03)：352K 全卷积，LCDP valid 15.3→20.6 dB，超 CTBG v9 上限(19.83) |
| Phase 3 | ONNX→OM 导出 + 板端集成 + A/B 测试 | 🔶 A/B 冒烟打通 (2026-07-03)：NN→θ→blob→回灌施加，真实场景暗部 +35% 零裁剪 |
| Phase 3.5 | **实时闭环板端验证**（socchina_app `--paramnet`：chn2→OM→u→θ→blob→ISP 热刷新） | ✅ **端到端跑通 (2026-07-06)**：OM 推理 ~1.6ms，DRC/LDCI/Gamma 场景相关施加，stream 646 帧 **0 drops / 0 error / 无 AICPU**，exit=0。"能否 30fps 实时"这一架构成立前提坐实。C 侧：`board/src/paramnet_map.c`(u→θ/θ→blob，主机字节级单测) + `paramnet_ctrl.c` + main.c `--paramnet`。OM 由 `models/isp_simulator/build_paramnet_om.sh` 产。 |
| Phase 4 | 扩蒸馏标签（Route A）：177 图候选池 `distill_expand/` 已备；板端 labels | 🔶 **pilot 通 (2026-07-06)**：3 图小样全链跑通，θ* 均超 ParamNet +0.28 dB；**全量 177 图(~3h 板端)未开** |

> 保真度闸门、blob v1→v3、批量校准与残差网络的完整实验记录见
> [isp-param-tuning-research.md](isp-param-tuning-research.md) §5。

技术依据: Qin ECCV 2022 / ACamera-Net / DynamicISP；完整 Prompt 见 `docs/isp-auto-tuning-prompt.md`。

---

## 1. CTBG 试验记录（已完成的模型训练与板端验证）

### 1.1 模型训练结果

| 版本 | 策略 | best val_psnr | @epoch | 结论 |
|---|---|---|---|---|
| v8 (18ch) | 从头训练, per-channel 系数 | 19.82 | 41 | 参考基线 |
| **v9 (6ch)** | 从头训练, 标量系数 | **19.83** | 116 | **当前部署** |
| v13 | MobileIE warmup + Dropout2d | **19.83** | 116 | 持平 v9，无早峰退化 |
| v10 | v8 backbone warm-start | 18.80 | 13 | 证伪——backbone 跨尾不兼容 |
| v11 | 从头, samples_per_epoch=500 | 16.01 | 7 | 证伪——梯度噪声过大 |
| v12 | v9基底 + weight_decay=1e-4 | 18.02 | 77 | 证伪——压制拟合能力 |

**关键发现**：19.83 是 6ch 标量系数 + down=4 架构在此数据集规模下的收敛上限。Warmup 可防止早峰退化但无法突破。突破需架构改进（如 3D Bilateral Grid、per-cell MLP）。

### 1.2 CTBG 板端管线性能记录

| 路径 | 帧率/延迟 | 瓶颈 |
|---|---|---|
| Display (ISP Gamma) | 30fps | 无 |
| Stream (ISP Gamma) | 30fps | 无 |
| Estimator (NPU, v9 6ch) | 3.8ms, ~0.3Hz | 场景变化触发 |
| Apply OM 诊断 (NPU) | 26ms, 上限5次 | 异步，不阻塞管线 |
| Apply OM 写回 | 89ms, ~1fps | CPU YUV 44ms + NPU 26ms |
| CTBG→DRC 启发式映射 | 60ms CPU, ~0.3Hz | fp16 解码 6.6M 系数 |

### 1.3 关键技术验证记录

- ✅ NPU 逐像素施加在 Ascend 310 上不可达 30fps（1024×576 elementwise 20.5ms，加前后处理 89ms）
- ✅ SS928 DRC/LDCI/Dehaze 支持 ISP API 热刷新（`ss_mpi_isp_set_drc_attr()` 等，<1ms）
- ✅ `isp_gamma_apply_curve(curve[64], strength)` 已封装，接受 64 节点浮点曲线
- ✅ ATC 构建环境已修复（ccec 编译器路径），ONNX→OM 流程跑通
- ✅ AIPP OM 构建成功（182KB）但 Ascend 310 执行失败（aclError=507900）——根因是纯 elementwise 图缺 TransData 格式桥接，v8 OM（含 ConvTranspose）可正常执行
- ✅ fp16→8-bit 整数 BT.601 YUV 转换，L1 缓存 LUT 方案
- ✅ 场景门控写回 + mmap 独立映射避免 VPSS DMA 竞争
- ✅ `systemctl stop socchina-stream` 释放 VB 池（独占式 MPP 资源）

---

## 2. 已完成的生产基线

- 仓库骨架、`.gitignore`、`.clang-format`、文档体系（开发规范 / 架构 / 板端操作 / AGENTS）。
- 统一构建闭环：CMake + `aarch64-mix210-linux` toolchain file，`scripts/build_board.sh`、`scripts/test_host.sh`。
- `control` 模块：场景曝光模式判决（纯逻辑 + 主机单测）。
- `display` 模块：VO→HDMI 1024x600 DVI 驱动，启动序含黑场预帧消除面板锁定杂线。
- `capture` + `vpss` 模块：OS08A20 → VI → ISP → VPSS 多路缩放，30.2fps 稳定。
- `isp` 模块：WDR 2to1 模式 30fps、AE 统计→`control_decide` 闭环、dehaze/DRC/LDCI 三态控制、CLUT 代码就绪。
- `stream` 模块：VENC H.264 CBR + 自带 RTSP/RTP over TCP，1024x576@30fps/3Mbps，断线重连。
- `infer.c`：LCDP param-net OM，生产 control worker 推理总耗时 2.4-2.7ms。
- `lut_bridge.c`：17v2 CLUT 安全桥（打包、限幅、单调性、端点保护、逐次步长护栏）。
- `main.c`：生产整链 SYS/VB/capture/vpss/display/stream/control/NN 生命周期，支持 linear/WDR 与目标帧率。
- systemd 开机自启动（`socchina-stream.service`），runtime config schema v1，`socchina-health` 只读健康检查。
- CTBG 诊断测试：`test_ctbg_apply_aipp.c`、`test_ctbg_apply_debug.c`（v9 vs v8 OM 对比诊断）。
- CTBG→DRC 启发式映射：`board/src/ctbg_isp_map.c`（17×15 块聚合 → DRC/LDCI 参数，已验证热刷新）。
- CoTF CLUT/Gamma 相机链加载和热刷新验证，CLUT 17v2 几何确认。
- OS08A20 WDR 2to1 实测正面：高光裁剪 5.7%→1.6%、暗部 36.5%→5.3%。
- 触摸输入开箱即用（Waveshare 7 寸面板 USB，`/dev/input/event0`，1:1 像素映射）。
- 板端 Web 控制台（MediaMTX + socchina-web + socchina-admin + socchina-auth），浏览器实时视频 + 参数控制。
- Web 控制链加固（2026-07-07，已上板验证）：**编码码率热更新**（VENC `set_chn_attr` 运行时改 CBR，
  不再走冷配置重启）；控制 socket 三处路径统一 `/run/socchina/admin.sock`（admin 监听 / socchina-web /
  authproxy 一致，`SOCCHINA_ADMIN_SOCK` 可覆盖）；`configtx.Apply` 合并写入保留 `CTRL_SOCK` 等运维键
  （修复冷 apply 后控制 socket 永久消失）；adminclient/authproxy 冷事务读超时按 op 放宽；app_control
  accept 加 `SO_RCVTIMEO`。板端另加 `RuntimeDirectoryPreserve=yes` drop-in（运维配置，不在仓库）解决
  admin/stream 共用 `RuntimeDirectory=socchina` 互删 socket。

## 3. 已关闭路线

- ❌ Config-Q（整图模型）、严格同帧分屏、MJPEG/CPU 视频转码
- ❌ 1024×576 全分辨率整图 OM（94.9ms，访存地板，INT8 无效）
- ❌ Route C（per-frame whole-image network，17.5fps）
- ❌ CTBG per-pixel apply 作为主增强路径（89ms/帧，仅 ~1fps，无实用价值）
- ❌ AIPP OM（v9 纯 elementwise 图缺 TransData，当前 ATC 版本无法解决）

## 4. 待完成项

**ISP 参数自动调优主线（部署闭环已通，剩画质裁决与扩标签）**
- 🟡 **画质裁决（路线 C，最高优先级）**：B2 easy 场景 A/B 显示 paramnet 中度压暗、非强改善；
  需**夜间/强逆光/过曝真实场景** A/B（paramnet vs vendor-auto/WDR），并与离线 sim 预测对账。
- 🔶 **扩蒸馏标签全量（路线 A）**：pilot 已通（+0.28dB），全量 177 图（`distill_expand/`，~3h 板端多会话）
  未开 → `distill labels` → `distill finetune`（排练式）。见 [isp-param-tuning-agent-prompt.md](isp-param-tuning-agent-prompt.md) §4。
- 🟡 **B2 runtime 深验**：显示/HDMI 路径未测、10 分钟长稳未测、多场景切换反馈环稳定性未压、WDR 模式未测。
- 🟡 已知模拟器残差（蒸馏免疫，走代理路线才需修）：极暗域 DRC strength 外推低估、`ldci.py` CLAHE 暗纹理反序。

**板端交互 / 工程**
- 🟡 **authproxy 未鉴权端点**：`web/authproxy/main.go` 的 `/api/v1/config*`、`/api/v1/status` 处理器注册在
  `/` 鉴权兜底之外，不校验 session cookie → 可无凭证直连 :8080 改冷配置/读状态。需给这些处理器补 session 校验
  （与 `/` 一致），保留浏览器同源 cookie 正常放行。
- 🟡 **LVGL 板端 UI 上板收尾**：交叉编译已通（`board/ui/`, `--DENABLE_LVGL`）；上板 flicker/触摸标定/
  透明叠加（GFBG G0 视频透出）待验证（当前 ui_lvgl 屏幕不透明，首次上板会盖住视频，属预期）。
- 🟢 SDK-free **全量**板端构建在既有 `ctbg_isp_map.c`/`infer_ctbg.c` 处失败（无条件 include SDK 头）；
  正规 SDK-free 验证 `test_host.sh` 全绿。若需全量 SDK-free 可加 `#ifdef WITH_SS928_SDK` 守卫。
- ❌ 板端测试自动化（触硬件 `test_<名>` 目前手动跑）
- ❌ `LICENSE` 内容待定
- 🟡 面板 flicker 观感需现场目视确认
- 🟡 WDR 强逆光场景画质细调、运动鬼影、长时稳定性

## 5. 风险与依赖

- **AE 统计单消费者（已知限制）**：cross-process `get_ae_stats` 与占路进程 3A 冲突（0xa01c8045），独立控制面无法读 AE → 场景自适应走集成式路径（已实测 OK）。
- 板端媒体状态易残留：新模块冒烟前确认无其他媒体进程，失败后优先干净重启。
- 曾遇相机不出帧（sensor MIPI 上电瞬态故障），判断出流看 `VI int_cnt`/`MIPI freq`。
- 曾遇 ACL `aclmdlExecute` 失败 + SMMU `CMD_SYNC`/`CMDQ timeout` → 重启板卡是可靠恢复路径。
