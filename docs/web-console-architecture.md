# 板端一体化 Web 控制台技术路线

## 1. 目标与范围

目标是在不改变 Config-R 图像处理主链、不增加第二次视频编码的前提下，让开发板独立提供：

- 浏览器实时查看处理后的 1024x576 H.264 视频；
- 调整图像处理开关、参数和安全预设；
- 控制 HDMI 开关；
- 查看流水线状态、错误、帧率、丢帧和推理耗时；
- 对需要重启的配置执行校验、应用、健康检查和自动回滚。

该路线对应“方案 A”：视频网关、Web 后端、静态页面和控制代理全部运行在开发板。上位机或手机只需要
现代浏览器，不承担常驻媒体或控制服务。

明确不做：

- 不用 CPU/FFmpeg 重新编码 H.264；
- 不新增 MJPEG、整图 NN、Config-Q 或严格同帧分屏；
- 不允许 HTTP 请求线程直接调用 ISP/VPSS/ACL 接口；
- 不允许 Web 服务执行任意 shell 命令或修改任意文件；
- 第一版不提供网页重启整块开发板的按钮。

## 2. 总体架构

```text
OS08A20 -> VI/ISP -> VPSS chn1 -> VENC H.264
                                      |
                                      v
                           socchina_app 内部 RTSP
                           127.0.0.1:8555/internal
                                      |
                                      v
                                  MediaMTX
                         +------------+------------+
                         |            |            |
                       WebRTC       LL-HLS       RTSP
                       :8889        :8888        :8554
                         |            |            |
                         +------ 浏览器/播放器 -----+

浏览器 -> socchina-web :8080
             |  静态页面 / REST / SSE / 媒体反向代理
             |
             +-> /run/socchina/app-control.sock
             |      -> socchina_app 控制命令队列
             |
             +-> /run/socchina/admin.sock
                    -> socchina-admin
                    -> 配置事务 / systemd / 健康检查 / 回滚
```

核心隔离原则：

1. `socchina_app` 继续独占相机、ISP、VPSS、VENC 和 ACL 生命周期。
2. MediaMTX 是内部 RTSP 的唯一客户端，负责向多个外部客户端转发。
3. `socchina-web` 使用非特权用户运行，只能访问两个受限 Unix socket。
4. 热参数最终由 control worker 串行应用，不能由 Web 线程直接写 ISP。
5. 冷参数通过单一配置事务修改，失败恢复上一份有效配置。
6. Web 或 MediaMTX 崩溃不能触发核心相机服务重启。

## 3. 视频通路

### 3.1 现有能力复用

当前生产链已经提供：

- VPSS chn1：1024x576 NV21；
- VENC H.264 CBR：30fps、约 3Mbps；
- 单客户端 RTSP/RTP over TCP；
- 播放时请求 IDR、断开重连和无客户端持续 drain；
- HDMI 与 RTSP 并行时主链约 30fps。

Web 路线不改变 VPSS/VENC，不增加颜色转换或图像拷贝，只调整 RTSP 的暴露方式。

### 3.2 内外 RTSP 分层

`socchina_app` 的内建 RTSP 首版只支持单客户端，因此改为内部源：

```text
监听：127.0.0.1:8555
路径：/internal
唯一客户端：MediaMTX
```

MediaMTX 对外提供：

```text
RTSP：   rtsp://<BOARD_HOST>:8554/live
WebRTC： http://<BOARD_HOST>:8889/live
LL-HLS： http://<BOARD_HOST>:8888/live/index.m3u8
```

MediaMTX 只做协议解析、RTP/SRTP 封装和网络转发，不进行视频转码。现有 VLC/GStreamer 使用方式迁移到
MediaMTX 的 8554 端口，浏览器首选 WebRTC。

### 3.3 浏览器播放策略

前端状态机：

```text
连接 WebRTC
  -> 成功：低延迟播放
  -> 超时或失败：重试一次
  -> 仍失败：切换 LL-HLS，并标记“高延迟模式”
```

目标延迟：

| 模式 | 目标端到端延迟 | 用途 |
| --- | --- | --- |
| WebRTC | 约 200–800ms | 参数调节、日常实时查看 |
| LL-HLS | 约 1.5–4s | ICE/UDP/网络策略失败时的兼容回退 |

浏览器截图在客户端从视频帧绘制到 canvas，不请求板端另行编码图片。

### 3.4 MediaMTX 配置边界

建议文件：

```text
/usr/local/bin/mediamtx
/etc/socchina/mediamtx.yml
/etc/systemd/system/socchina-mediamtx.service
```

配置原则：

- 使用 Linux ARM64 单文件发行版或可复现交叉构建产物；
- 固定经过验收的 1.x 版本，不使用运行时自动升级；
- source 使用 `rtsp://127.0.0.1:8555/internal` 和 TCP；
- Control API、metrics、pprof 仅监听 loopback；
- WebRTC/HLS/外部 RTSP 按部署网络开放；
- 第一版限制总视频客户端数量，防止网络和内存无界增长；
- 外部二进制不入库，仓库只保留版本、校验值和部署说明。

MediaMTX 示例配置的目标形态：

```yaml
logLevel: info

rtspAddress: :8554
hlsAddress: :8888
webrtcAddress: :8889
webrtcLocalUDPAddress: :8189

api: true
apiAddress: 127.0.0.1:9997
metrics: true
metricsAddress: 127.0.0.1:9998

paths:
  live:
    source: rtsp://127.0.0.1:8555/internal
    sourceProtocol: tcp
```

实际字段以固定版本的配置参考为准，升级版本时必须重新做浏览器、IPv6、CPU/内存和多客户端验收。

## 4. Web 与控制组件

### 4.1 `socchina-web`

推荐使用 Go 标准库实现单一静态 ARM64 二进制：

```text
/usr/local/bin/socchina-web
/usr/share/socchina-web/index.html
/usr/share/socchina-web/assets/
/etc/socchina/web.conf
```

原因：

- 不依赖板端 Python、Node.js 或包管理器；
- 可交叉编译为单文件；
- 标准库覆盖 HTTP、JSON、SSE、Unix socket 和静态文件；
- 便于做严格类型校验、并发限制和超时；
- 内存和部署复杂度可控。

前端第一版使用原生 HTML/CSS/JavaScript，不引入 Node 构建链。必要的 HLS 播放库固定版本并随静态资源
部署，不使用公共 CDN。

`socchina-web` 职责：

- 提供静态页面；
- 提供 REST API；
- 提供 SSE 状态推送；
- 反向代理或生成 WebRTC/HLS 播放端点；
- 连接应用控制 socket；
- 连接特权管理 socket；
- 聚合 MediaMTX API/metrics 中的媒体会话状态。

### 4.2 应用控制 socket

在 `socchina_app` 中新增：

```text
/run/socchina/app-control.sock
```

建议协议为一行一个 JSON 请求/响应：

```json
{"id":12,"op":"status"}
{"id":13,"op":"set","params":{"tone_strength":0.25}}
{"id":14,"op":"set","params":{"nn_clut_enabled":false}}
```

```json
{"id":13,"ok":true,"applied":{"tone_strength":0.25}}
```

实现约束：

1. socket 线程只负责解析、鉴权后的命令校验和入队；
2. 使用固定容量队列，队列满时拒绝请求，不能阻塞图像线程；
3. 同一参数的新值可以覆盖尚未执行的旧值；
4. control worker 在原有控制周期内串行应用 ISP 参数；
5. 状态读取从原子指标生成 `pipeline_metrics_t` 快照；
6. 请求和响应设置有限超时；
7. socket 客户端断开不能影响 control worker。

### 4.3 `socchina-admin`

冷配置、systemd 和回滚需要特权，单独提供：

```text
/usr/local/bin/socchina-admin
/run/socchina/admin.sock
/var/lib/socchina/config/
```

只允许固定操作：

```text
config.validate
config.apply
config.rollback
hdmi.set
service.restart
service.status
```

禁止：

- 任意 shell 命令；
- 用户指定任意 systemd unit；
- 用户指定任意文件路径；
- 输出配置文件中的凭据；
- 直接修改 MediaMTX 或系统网络配置。

## 5. 参数模型

### 5.1 热参数

热参数不重建媒体链：

| 参数 | 范围 | 生效路径 |
| --- | --- | --- |
| `enhancement_enabled` | bool | 总增强旁路策略 |
| `nn_clut_enabled` | bool | 启用或旁路 NN CLUT |
| `tone_enabled` | bool | 启用或旁路规则 Gamma |
| `tone_strength` | 0–1 | 规则 Gamma 强度 |
| `nn_high_clip_guard` | 0–100 | NN CLUT 高光旁路门限 |
| `drc_mode` | off/auto/manual | ISP DRC 策略 |
| `drc_strength` | 0–1023 | DRC 手动强度 |
| `ldci_mode` | off/auto | ISP LDCI 策略 |
| `preset` | 枚举 | 一组经过验收的安全参数 |

目标：

- API 到 control worker 接受命令小于 100ms；
- ISP 参数生效通常小于 300ms；
- 加上 WebRTC 视频延迟，用户看到结果约 300ms–1.1s。

滑块必须做前端防抖：拖动期间只更新本地显示，停止 150–250ms 后发送；同一参数最多约 5 次/秒。

### 5.2 冷参数

以下参数需要重启 `socchina-stream.service`：

| 参数 | 原因 |
| --- | --- |
| Linear/WDR 2to1 | 需要重建 sensor/VI/ISP |
| 目标 FPS | 影响 sensor、ISP 与 VENC |
| HDMI 开关 | 当前需要创建/销毁 chn0、VO 与 HDMI |
| VENC 码率 | 当前在编码通道初始化时确定 |
| 内部 RTSP 端口/路径 | 影响 MediaMTX source |
| 模型加载开关/路径 | 需要初始化或释放 ACL 模型 |

网页必须明确提示冷配置会造成数秒视频中断。

### 5.3 安全预设

第一版优先提供预设，再开放底层参数：

| 预设 | 主要行为 |
| --- | --- |
| 稳定 | NN CLUT off；规则 Gamma on；strength 0.25；DRC/LDCI auto |
| 暗光增强 | NN CLUT 受高光门控；Gamma on；DRC/LDCI auto |
| 逆光/WDR | WDR 2to1；DRC auto；严格高光门控；需要重启 |
| 旁路 | NN CLUT off；增强 Gamma off；ISP 块回安全基线 |
| 自定义 | 只开放经过白名单和范围校验的参数 |

预设值必须随多场景画质验收更新，不能仅凭单场景结果作为最终默认。

## 6. 冷配置事务

冷配置采用 generation/ETag 防止多个浏览器相互覆盖：

```text
1. GET 当前配置与 generation
2. 提交携带同一 generation 的候选配置
3. Web 层做类型和范围校验
4. admin 写 runtime.conf.pending
5. 调用 socchina-start --check-config
6. 当前配置保存为 last-good/previous
7. 原子 rename pending -> runtime.conf
8. systemctl restart socchina-stream.service
9. 最多等待 15 秒
10. 执行 socchina-health
11. 成功：递增 generation
12. 失败：恢复 last-good 并再次启动
```

建议状态文件：

```text
/etc/socchina/runtime.conf
/var/lib/socchina/config/last-good.conf
/var/lib/socchina/config/previous.conf
/var/lib/socchina/config/generation
```

同一时间只允许一个冷配置事务。客户端断开不能中断已经进入原子提交阶段的事务。

## 7. API 草案

### 7.1 查询

```text
GET /api/v1/status
GET /api/v1/config
GET /api/v1/health
GET /api/v1/media/status
GET /api/v1/events
```

`/api/v1/events` 使用 SSE，每秒推送一次普通状态；状态进入 DEGRADED/FAILED、配置事务完成或媒体会话变化
时立即推送。

状态响应示例：

```json
{
  "pipeline": {
    "state": "RUNNING",
    "capture_mode": "linear",
    "target_fps": 30,
    "display_fps": 30.12,
    "stream_drops": 0,
    "timeouts": 0,
    "transient_errors": 0,
    "fatal_errors": 0
  },
  "processing": {
    "nn_enabled": true,
    "nn_degraded": false,
    "tone_strength": 0.25,
    "high_clip_guard": 3.0,
    "infer_p95_ms": 1.35,
    "transaction_p95_ms": 4.35
  },
  "outputs": {
    "hdmi": false,
    "rtsp": true,
    "webrtc": true,
    "hls": true,
    "viewers": 1
  }
}
```

### 7.2 热控制

```text
PATCH /api/v1/control
POST  /api/v1/presets/{name}
POST  /api/v1/defaults
```

### 7.3 冷配置

```text
PUT  /api/v1/config
POST /api/v1/config/apply
POST /api/v1/config/rollback
POST /api/v1/hdmi
POST /api/v1/service/restart
```

第一版不提供任意命令执行或设备重启 API。

## 8. 页面功能

主页面建议包含：

- WebRTC 实时视频；
- LL-HLS 回退状态；
- 全屏、浏览器截图和手动重连；
- RUNNING/DEGRADED/FAILED 状态；
- FPS、drops、timeouts、infer/transaction p95；
- 当前预设、NN CLUT、Gamma、DRC、LDCI；
- HDMI 与 RTSP 状态；
- 热参数控件；
- 冷配置“需要重启”标识和确认窗口；
- 配置应用、健康检查、回滚进度；
- 明确的错误与恢复建议。

冷配置进度：

```text
正在校验 -> 正在保存 -> 正在重启 -> 等待相机 -> 等待视频 -> 健康检查 -> 完成/回滚
```

## 9. systemd 与故障隔离

新增服务：

```text
socchina-mediamtx.service
socchina-admin.service
socchina-web.service
```

依赖原则：

```text
socchina-stream.service
  -> socchina-mediamtx.service
  -> socchina-web.service

socchina-admin.service
  -> socchina-web.service
```

具体要求：

- MediaMTX 在核心流未就绪时持续重连，不让其失败拉低核心服务；
- Web 服务依赖 admin socket，但不强依赖视频网关；
- Web/MediaMTX 重启不得重启 `socchina-stream.service`；
- 核心流重启后 MediaMTX 自动恢复 source；
- 配置错误状态 2 和 ACL 致命状态 70 的既有语义保持不变；
- systemd sandbox 限制文件系统、设备、能力与可写目录。

故障行为：

| 故障 | 预期行为 |
| --- | --- |
| Web 服务崩溃 | 相机、内部 RTSP 与 HDMI 继续 |
| MediaMTX 崩溃 | 核心流和 HDMI 继续，浏览器暂时断流 |
| 浏览器断开 | 释放会话，不重启媒体链 |
| 热参数非法 | 拒绝并保留旧参数 |
| 冷配置非法 | 不替换当前配置 |
| 新配置启动失败 | 自动恢复 last-good |
| NN 普通失败 | DEGRADED，规则 Gamma 继续 |
| ACL/SMMU 致命错误 | 核心服务 FAILED，网页提示需要设备恢复 |
| WebRTC 失败 | 浏览器切换 LL-HLS |

## 10. 权限与安全

建议：

```text
socchina-web 用户：非特权
socchina 组：允许访问受限 socket
socchina-admin：root，但仅暴露固定操作
```

```text
/run/socchina/app-control.sock  root:socchina 0660
/run/socchina/admin.sock        root:socchina 0660
/etc/socchina/runtime.conf      root:root     0644
/var/lib/socchina/config        root:root     0700
```

最小安全要求：

- 参数严格白名单和范围校验；
- HttpOnly/SameSite 会话 cookie；
- 写操作使用 CSRF token；
- 登录、写配置和重启接口限流；
- 只读视频权限与管理权限分开；
- API、metrics、pprof 只监听 loopback；
- 不把密码、token、真实地址或证书写入仓库；
- 安装时生成随机密钥；
- 跨不可信网络时使用 HTTPS 或可信 VPN/反向代理；
- 操作审计只记录动作和结果，不记录凭据。

## 11. 性能预算

当前已知基线：

- `socchina_app` 完整链曾实测约 9.6% CPU；
- RSS/HWM 约 38MB；
- H.264 约 3Mbps；
- 20 秒 P1 回归为 30.16fps、0 drops/timeout/transient/fatal。

无转码条件下的工程预算：

| 组件 | CPU 增量 | RSS 增量 | 网络 |
| --- | ---: | ---: | ---: |
| `socchina-web` 空闲 | 通常 <1% | 8–20MB | 可忽略 |
| REST/SSE 单客户端 | 通常 <1% | 少量 | 可忽略 |
| `socchina-admin` | 接近 0 | 2–8MB | 无持续流量 |
| MediaMTX 无外部读者 | 约 1–3% | 15–40MB | 本机 RTSP |
| 单 WebRTC 读者 | 约 2–8% | 数 MB 增量 | 约 3Mbps |
| 单 HLS 读者 | 约 1–5% | 取决于缓存 | 约 3Mbps |
| 每增加一个读者 | 约 1–4% | 少量增加 | 再增加约 3Mbps |

这些是实施前预算，不是板端实测结论。第一版建议：

- 总视频客户端最多 2 个；
- WebRTC 优先，HLS 只作回退；
- HLS 分片与保留窗口设为满足回退的最小值；
- 禁用录制、转码和不需要的协议；
- 运行资源监控与 ACL profiling 分开执行。

验收门：

- 单 WebRTC 客户端下主链平均不低于 29.5fps；
- display/stream drops 不因 Web 服务持续增长；
- infer p95 不高于 3ms，完整 LUT 事务 p95 不高于 10ms；
- 无转码进程；
- Web/MediaMTX 重启不影响核心媒体链；
- 单/双客户端分别记录 CPU、RSS、网络和端到端延迟。

## 12. 目录规划

```text
web/
├── README.md
├── backend/
│   ├── cmd/socchina-web/
│   ├── internal/api/
│   ├── internal/appclient/
│   ├── internal/adminclient/
│   ├── internal/media/
│   └── go.mod
├── admin/
│   ├── cmd/socchina-admin/
│   ├── internal/configtx/
│   └── go.mod
└── ui/
    ├── index.html
    ├── app.js
    ├── styles.css
    └── vendor/

deploy/
├── systemd/
│   ├── socchina-web.service
│   ├── socchina-admin.service
│   └── socchina-mediamtx.service
├── mediamtx.yml
└── web.conf

scripts/
├── build_web.sh
├── install_board_web.sh
└── validate_board_web.sh
```

生成的 ARM64 二进制、MediaMTX 发行包、缓存、日志和录制文件不得提交。

## 13. 实施阶段与验收

### W0：视频 PoC

- 部署固定版本 ARM64 MediaMTX；
- 内部 RTSP 改为 loopback:8555；
- 浏览器 WebRTC 播放和 HLS 回退；
- 单/双客户端资源测量；
- 确认无转码和主链零新增 drops。

### W1：只读控制台

- 静态页面；
- 应用 `status` socket；
- `pipeline_metrics_t` 实时快照；
- REST/SSE；
- MediaMTX 会话状态；
- DEGRADED/FAILED 告警。

### W2：冷配置

- `socchina-admin`；
- HDMI、Linear/WDR、FPS、码率和模型加载配置；
- generation/ETag；
- 校验、原子提交、健康检查和自动回滚。

### W3：热控制

- NN CLUT、规则 Gamma、强度、高光门限、DRC/LDCI；
- control worker 命令队列；
- 安全预设与前端防抖；
- 并发请求和异常客户端测试。

### W4：安全与交付

- 登录、CSRF、限流、权限隔离；
- systemd sandbox；
- HTTPS/VPN 部署说明；
- 开机自启、完整重启和故障恢复；
- 一键安装、验收和卸载脚本；
- 更新操作手册和演示流程。

## 14. 完成定义

方案 A 完成需同时满足：

1. 开发板独立启动核心流、MediaMTX、Web 和管理服务；
2. 浏览器能通过 WebRTC 实时查看处理后画面，失败时可回退 HLS；
3. 热参数无需重启且由 control worker 串行应用；
4. HDMI 等冷配置通过事务应用，失败自动回滚；
5. 页面能展示统一流水线和媒体会话指标；
6. Web/MediaMTX 故障不破坏相机、RTSP 内部源或 HDMI；
7. 单客户端性能通过 Config-R 原有门限；
8. 权限、认证和敏感信息满足仓库安全规范。
