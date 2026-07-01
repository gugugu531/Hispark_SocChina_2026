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
| Phase 1 | 可微 ISP 模拟器 (WDR+Gamma+DRC+LDCI+Dehaze) | 📋 待实施 |
| Phase 2 | ISP ParamNet 训练 + 消融 | 📋 待实施 |
| Phase 3 | ONNX→OM 导出 + 板端集成 + A/B 测试 | 📋 待实施 |

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

## 3. 已关闭路线

- ❌ Config-Q（整图模型）、严格同帧分屏、MJPEG/CPU 视频转码
- ❌ 1024×576 全分辨率整图 OM（94.9ms，访存地板，INT8 无效）
- ❌ Route C（per-frame whole-image network，17.5fps）
- ❌ CTBG per-pixel apply 作为主增强路径（89ms/帧，仅 ~1fps，无实用价值）
- ❌ AIPP OM（v9 纯 elementwise 图缺 TransData，当前 ATC 版本无法解决）

## 4. 待完成项

- 📋 ISP 参数自动调优（Phase 1-3，见 §0）
- ❌ 板端测试自动化
- ❌ `LICENSE` 内容待定
- 🟡 面板 flicker 观感需现场目视确认
- 🟡 WDR 强逆光场景画质细调、运动鬼影、长时稳定性

## 5. 风险与依赖

- **AE 统计单消费者（已知限制）**：cross-process `get_ae_stats` 与占路进程 3A 冲突（0xa01c8045），独立控制面无法读 AE → 场景自适应走集成式路径（已实测 OK）。
- 板端媒体状态易残留：新模块冒烟前确认无其他媒体进程，失败后优先干净重启。
- 曾遇相机不出帧（sensor MIPI 上电瞬态故障），判断出流看 `VI int_cnt`/`MIPI freq`。
- 曾遇 ACL `aclmdlExecute` 失败 + SMMU `CMD_SYNC`/`CMDQ timeout` → 重启板卡是可靠恢复路径。
