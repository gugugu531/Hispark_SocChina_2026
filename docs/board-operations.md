# 板端操作手册

板卡：海思 SS928 / SD3403（海鸥派）。SSH 目标：`root@192.168.1.168`。
运行根目录约定：`/root/socchina-2026/`。

> 本手册沉淀板端部署/运行/恢复的稳定操作。具体命令在各模块实现后补全；下面给出约定与已知注意事项。

## 1. 部署

- 主机交叉编译产物 + 模型 OM + 运行脚本，通过 `scripts/deploy_board.sh` 同步到 `/root/socchina-2026/`。
- 模型 OM 与可执行文件、运行期配置一同部署到运行目录。

## 2. 启动 / 重启

- 启动顺序：媒体管线（VI/ISP/VPSS/VO/HDMI）→ 增强应用（AIPP/NNN 推理 + 合成 + VENC/RTSP）。
- 仅重启增强应用：在媒体链健康时，kill 旧进程后重启应用即可。
- 重启完整链路：媒体或 HDMI 异常时，按"创建逆序"停止后重新拉起；启动前清理残留 ISP/VB 状态。

## 3. 健康检查

部署/启动后，逐项确认（命令在实现后补全，参考 `/proc/umap/*`）：

```sh
# 进程
ps -ef | grep -E "socchina|sample_vio" | grep -v grep
# VPSS 各通道输出
grep -A8 "vpss chn output status" /proc/umap/vpss
# HDMI 链路
grep -E "run status|tmds mode|rsen|phy output enable" /proc/umap/hdmi0
# GFBG 图层
grep -E "open_count|show_state|graphic_enable" /proc/umap/gfbg0
# NPU 利用率
cat /proc/umap/svp_nnn | grep -i hw_utilization
```

链路健康判据：HDMI `rsen` / `phy output enable` 均为 `YES`，`tmds mode: DVI`（当前 Waveshare 面板）。

## 4. 故障恢复

- **ACL 执行失败 + SMMU/CMDQ 超时**：日志出现 `aclmdlExecute` 失败 + `arm-smmu-v3 ... CMD_SYNC timeout` / `CMDQ timeout` 时，NPU/SMMU 上下文已坏，**重启板卡**是可靠恢复路径。
- **`ISP[0] already inited` / VB 冲突**：上一次异常退出残留状态，启动前调用 ISP 退出清理（如 `ss_mpi_isp_exit(0)`）再重新初始化。
- **媒体栈脏（进程进 D 态、无法 kill）**：清醒重启板卡，比反复重试省时间。
- **无显示**：确认未有第二个进程争用 SYS/VB；按"先媒体后应用"的顺序重启。

## 5. 注意事项

- **不要**用 `pkill` 停厂商 `sample_hdmi`；用其 FIFO 停止脚本，否则残留 MPP/VB/HDMI 状态。
- 不要在相机/增强链运行时启动厂商 `sample_hdmi`（争用 SYS/VB，会导致 `ss_mpi_vb_set_conf failed`）。
- 供电不稳或刚复位后，先确认进程/媒体状态干净，再启动新的媒体/ACL 任务。
- 长时运行前做短 smoke test（有限帧数）确认链路与计时正常。
