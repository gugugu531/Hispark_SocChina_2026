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

## 8. RTSP 开机自启动与 HDMI 选择

### 8.1 设计

板端由 `socchina-stream.service` 管理生产程序：

```text
systemd
  -> /usr/local/sbin/socchina-start
     -> /etc/socchina/runtime.conf
        -> /root/socchina-2026/socchina_app --stream [--no-display]
```

- 服务随 `multi-user.target` 启动，排序在厂商 `rc-local.service` 媒体模块加载之后，但不等待
  Portal 认证完成。相机/VENC/RTSP 可先初始化并监听 IPv4/IPv6 通配地址，网络地址就绪后客户端再连接。
- 默认 `ENABLE_HDMI=0`：只启用 VPSS chn1、VENC 和 RTSP，不创建 chn0，不初始化 VO/HDMI。
- `ENABLE_HDMI=1`：同时创建 chn0，启用 VO/HDMI，并保留 RTSP。
- `Restart=on-failure`，异常退出后 3 秒重试；启动频率不封顶，避免开机早期 sensor 短暂未就绪后
  服务永久停在 failed。
- 包装器在启动媒体链前最多等待 `/dev/ot_mipi_rx` 30 秒，避免驱动设备节点尚未创建时过早调用
  MIPI/VI 并留下短暂 VB 冲突；超时后退出并交给 systemd 重试。
- 正常 `stop/restart` 发送 SIGTERM，应用按 stream/VENC、display、VPSS、capture、SYS/VB 的逆序清理。
- 第一版 HDMI 切换需要重启媒体服务，通常中断 RTSP 数秒；不做运行时热插拔。

仓库文件：

| 文件 | 用途 |
| --- | --- |
| `deploy/systemd/socchina-stream.service` | systemd unit 模板 |
| `deploy/systemd/runtime.conf` | 首次安装的默认配置 |
| `scripts/board/socchina-start` | 配置校验与应用参数拼装 |
| `scripts/board/socchina-display` | HDMI on/off/status 控制 |
| `scripts/install_board_service.sh` | 主机侧幂等安装/更新入口 |

### 8.2 构建与安装

先按开发规范完成 SDK Release 构建，再安装：

```sh
export CROSS_COMPILE_ROOT=/path/to/aarch64-mix210-linux
export ISL_LIB_DIR=/path/to/libisl
export SS928_SDK_ROOT=/path/to/ss928-mpp-sample
scripts/build_board.sh Release

BOARD=hispark-remote scripts/install_board_service.sh
```

安装器会：

1. 停止旧的 `socchina-stream.service`（不存在时忽略）。
2. 更新 `/root/socchina-2026/socchina_app`。
3. 安装包装脚本到 `/usr/local/sbin/`。
4. 安装 unit 到 `/etc/systemd/system/`。
5. **仅当配置不存在时**创建 `/etc/socchina/runtime.conf`，更新版本不会覆盖现场选择。
6. `daemon-reload`，然后 `enable --now`。

首次安装默认仅 RTSP：

```sh
ENABLE_HDMI=0
RTSP_PORT=8554
STREAM_PATH=live
BITRATE_KBPS=3000
SENSOR_INDEX=1
TONE_STRENGTH=0.7
```

### 8.3 日常使用

服务管理：

```sh
systemctl status socchina-stream.service
systemctl restart socchina-stream.service
systemctl stop socchina-stream.service
systemctl start socchina-stream.service
journalctl -u socchina-stream.service -f
```

切换 HDMI：

```sh
socchina-display status
socchina-display on
socchina-display off
```

命令会持久化 `ENABLE_HDMI` 并优雅重启服务；下一次开机沿用最后选择。

直接编辑其它参数后重启：

```sh
vi /etc/socchina/runtime.conf
systemctl restart socchina-stream.service
```

RTSP 地址：

```text
rtsp://[<BOARD_IPV6>]:8554/live
```

Haruna/VLC 中选择“打开 URL”并输入上面的地址。GStreamer 验证：

```sh
gst-launch-1.0 -q \
  rtspsrc location='rtsp://[<BOARD_IPV6>]:8554/live' protocols=tcp latency=100 \
  ! rtph264depay ! fakesink sync=false
```

### 8.4 健康检查与恢复

```sh
systemctl is-enabled socchina-stream.service
systemctl is-active socchina-stream.service
ss -lntp | grep 8554
grep -A5 "venc chn attr 1" /proc/umap/venc
grep -A4 "vpss chn output status" /proc/umap/vpss
grep -E "run status|rsen|phy output enable" /proc/umap/hdmi0
```

- HDMI off 时应看到 `run status: CLOSE`、`phy output enable: NO`，VPSS 仅 chn1。
- HDMI on 时应看到 `run status: OPEN START`、`phy output enable: YES`，VPSS 同时有 chn0/chn1。
- 当前面板使用 DVI 模式，`hdmi enable: NO` 在 HDMI 输出已启动时也可能成立，不能单独作为开关判据。
- 配置错误时 `socchina-start` 会以状态 2 退出，错误可从 journal 查看；修正配置后执行 restart。
- 媒体栈进入 D 态或 ACL/SMMU 超时仍按 §4 干净重启板卡，不依赖 systemd 无限重试修复坏硬件上下文。

### 8.5 开发与验收记录

#### 2026-06-20 systemd 服务与 HDMI 双态冒烟

目标：

- 安装并启用开机 RTSP 服务；验证默认 HDMI off、运行时切换 on/off、RTSP 连续播放和资源状态。

命令/路径：

```sh
systemctl enable --now socchina-stream.service
socchina-display on
socchina-display off
gst-launch-1.0 -q \
  rtspsrc location='rtsp://[<BOARD_IPV6>]:8554/live' protocols=tcp latency=100 \
  ! rtph264depay ! fakesink sync=false
```

结果：

- unit 为 `enabled` 且 `active (running)`，主进程参数来自 `/etc/socchina/runtime.conf`。
- 默认 HDMI off：应用带 `--no-display`，VPSS 仅 chn1，HDMI `run status: CLOSE`、PHY NO；
  RTSP GStreamer 连续 8 秒，到测试超时才退出。
- HDMI on：配置持久化为 `ENABLE_HDMI=1`，VPSS chn0/chn1 均为 30fps，HDMI
  `run status: OPEN START`、RSEN/PHY YES；显示平均约 30.1–30.5fps，stream drops=0，
  同时 RTSP 连续 8 秒通过。
- 再切回 HDMI off：服务重启后配置为 0，HDMI CLOSE/PHY NO，VPSS 仅 chn1，8554 保持监听。
- systemd 启停均由 SIGTERM 触发应用逆序清理；`NRestarts=0`，无异常自动重启。
- 第一次完整重启中，unit 在 boot 后约 8 秒首次运行，当时 `/dev/ot_mipi_rx` 尚未创建；
  systemd 自动重试 3 次并在 boot 后约 19 秒稳定启动，随后 10 秒 RTSP 拉流通过。依据该证据，
  包装器增加了最多 30 秒的 MIPI 设备等待门禁。
- 第二次完整重启中，MIPI 节点在 boot 后约 10 秒出现，但厂商 `rc-local.service` 仍在加载
  VPSS/ISP/VENC 等模块，SYS/VB 尚未稳定，仍发生 3 次重试。`systemd-analyze blame` 显示
  `rc-local.service` 耗时约 12.8 秒，kernel journal 也确认媒体模块由该阶段加载。因此 unit
  增加 `After/Wants=rc-local.service`，以真实模块加载完成作为启动顺序门。
- 第三次完整重启终验：SSH 约 20 秒恢复，unit 本次 boot 仅启动 1 次，`NRestarts=0`；
  `ActiveEnterTimestamp` 为 boot 后约 20 秒。VPSS chn1 30fps、VENC 队列无积压、
  stream 每 5 秒约 147–152 帧且 drops=0；GStreamer TCP RTSP 连续 10 秒至测试超时。
  持久配置仍为 HDMI off，HDMI `run status: CLOSE`、PHY NO。开机自启动验收通过。
- journal 中可能出现 `/var/log/npu/conf/slog/slog.conf` 或 slogd 缺失告警；当前二进制链接 ACL
  运行库，即使生产规则 Gamma/RTSP 路径未调用 NPU也会初始化其日志设施。该告警非致命，
  不影响相机、VENC、RTSP 或服务 active 状态。

解读：

- 服务安装、配置持久化、RTSP/HDMI 双态和完整重启自恢复均已通过。
- `hdmi enable: NO` 是当前 DVI 输出模式属性，不能表示服务是否启用了显示；以 run status/PHY 为准。

推荐后续每次修改服务时继续记录：

```markdown
目标：

- 服务启停、异常恢复、开机自启、RTSP 与 HDMI 配置。

命令/路径：

- systemctl / journalctl / socchina-display / 播放命令。

结果：

- 启动耗时、RTSP 帧率、重连、HDMI 状态、退出后 VENC/VPSS 状态。

解读：

- 是否满足无人值守启动；仍开放的限制。
```
