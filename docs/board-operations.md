# 板端操作手册

板卡：海思 SS928 / SD3403（海鸥派）。受管网络优先通过动态全局 IPv6/SSH 别名
`hispark-remote` 访问；`root@192.168.1.168` 是电脑网线直连回退地址。
运行根目录约定：`/root/socchina-2026/`。

> 本手册沉淀板端部署/运行/恢复的稳定操作。具体命令在各模块实现后补全；下面给出约定与已知注意事项。

## 1. 部署

- 主机交叉编译产物 + 模型 OM + 运行脚本，通过 `scripts/deploy_board.sh` 同步到 `/root/socchina-2026/`。
- 模型 OM 与可执行文件、运行期配置一同部署到运行目录。
- 网络目标和 SSH 别名见 [network-access.md](network-access.md)。

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

## 6. 触摸输入（USB HID，内核 event0 开箱即用）

Waveshare 7 寸 HDMI LCD (C) 的电容触摸经面板 **USB 口**上报。**板端实测（2026-06-14）
结论：开箱即用，无需任何自定义驱动 / 标定 / 缩放**——内核自带的 `usbhid → hid-generic →
evdev` 已把它识别为标准绝对触摸屏，且控制器直接上报面板像素坐标。

实测事实（板端，面板 USB 触摸线已接；当时使用电脑直连地址 `192.168.1.168`）：

| 项 | 值 |
| --- | --- |
| 设备名 | `WaveShare WS170120 Touchscreen` |
| USB VID:PID | `0eef:0005`（eGalax / D-WAV，单点电容） |
| 内核绑定 | `usbhid` → **`hid-generic`** → `evdev`（dmesg：`hid-generic 0003:0EEF:0005.0001`） |
| 输入节点 | `/dev/input/event0`（另带 `mouse0`） |
| 上报能力 | `EV_ABS`（绝对）+ `BTN_TOUCH`；`ABS_X 0..1024`、`ABS_Y 0..600` |
| 坐标映射 | **= 面板像素 1:1**（与 display 的 1024x600 完全对齐，无需缩放/标定） |
| `/dev/hidraw*` | **无**（本内核未启用 `CONFIG_HIDRAW`） |
| `/dev/uinput` | 有 |

**应用怎么用**：直接读 `/dev/input/event0` 的标准 `input_event`（绝对坐标即面板像素），
或交给 X/Wayland/libinput/Qt-evdev 等输入栈自动识别。无需任何额外进程。

板端核对命令：

```sh
# 1) 确认设备与能力（关注 Name / Handlers=...event0 / B:ABS / B:KEY 含 BTN_TOUCH）
cat /proc/bus/input/devices

# 2) 看实时事件：板上有 evtest 时最直观；否则抓原始字节再解（每条 input_event = 24B）
evtest /dev/input/event0           # 若无 evtest：
timeout 20 cat /dev/input/event0 | hexdump -C   # 触摸时有数据即通

# 3) 查轴范围（确认 0..1024 / 0..600）：板上有原生 gcc，可临时编个 EVIOCGABS 小程序，
#    或用 libevdev/evtest 的设备信息输出。
```

> 备注：曾评估“用户态 HID→uinput 驱动”方案（读 `/dev/hidrawN` 解析 HID 报告再经
> `/dev/uinput` 注入）。但本板**无 hidraw**且内核已正确处理，故该方案在此**既不需要也跑不起来**，
> 不予采用。仅当换到无法自动绑定、却开放了 hidraw 的内核/固件时才需重新考虑用户态方案。

## 7. 受管网络有线接入与 IPv6 SSH

板卡受管网络网线使用 `eth1`。厂商 `search_tool` 仍会按 `/opt/cfg/dev_info.config` 给无链路的
`eth0` 配置 `192.168.1.168/24` 和默认网关；网络服务启动时会删除该无效默认路由，但保留静态地址，
避免影响以后恢复电脑直连调试。

认证与网络安装实现不属于本仓库；仓库不记录或链接其外部存放位置。

2026-06-19 实测：

- DHCPv4/DHCPv6 均成功获取动态地址；稳定 Client ID/DUID 在完整重启后续租到同一地址。
- Web Portal 自动认证成功。
- 电脑到板端 IPv6 ping 为低毫秒级、`0%` 丢包，TCP/22 可达，SSH 公钥登录成功。
- 完整重启验收：约 10 秒获得 DHCPv6、约 14 秒获得 DHCPv4，随后
  `portal-network.service` 自动完成 Portal 认证；无需串口手工命令即可恢复 IPv6 SSH。

Clash Verge TUN 会增加 IPv6 策略路由表。电脑端测试和 SSH 时显式使用
`-I <Wi-Fi IPv6>` / `-o BindAddress=<Wi-Fi IPv6>`，避免连接被 TUN 默认路由接管。
完整地址稳定性、SSH 别名和故障恢复见 [network-access.md](network-access.md)。
