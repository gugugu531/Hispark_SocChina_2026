# Web 控制台

板端一体化 Web 视频与控制台 —— 浏览器实时查看处理后的视频、调整图像处理参数、查看流水线状态。

## 架构

完整技术路线见 [docs/web-console-architecture.md](../docs/web-console-architecture.md)。

```text
OS08A20 → VI/ISP → VPSS chn1 → VENC H.264
                                     |
                              socchina_app 内部 RTSP
                              127.0.0.1:8555/internal
                                     |
                                 MediaMTX (无转码)
                        +------------+------------+
                        |            |            |
                      WebRTC       LL-HLS       RTSP
                      :8889        :8888        :8554
                        |            |            |
                        +------ 浏览器/播放器 -----+

浏览器 -> socchina-web :8080
             |  静态页面 / REST / SSE
             |
             +-> /run/socchina/app-control.sock
             |      -> socchina_app 控制命令队列
             +-> /run/socchina/admin.sock
                    -> socchina-admin (冷配置事务 / systemd / 回滚)
```

## 目录

| 路径 | 用途 |
| --- | --- |
| `backend/` | Go Web 服务（REST / SSE / Unix socket 客户端） |
| `admin/` | Go 特权管理服务（冷配置事务 / systemd 操作） |
| `ui/` | 原生 HTML/CSS/JavaScript 前端（无 Node 构建链） |

## 实施阶段

| 阶段 | 状态 | 内容 |
| --- | --- | --- |
| W0 视频 PoC | ✅ 已完成 | MediaMTX ARM64 + WebRTC/HLS/RTSP 三协议 |
| W1 只读控制台 | ✅ 已完成 | Go 后端 + 原生前端 + SSE + HLS 反向代理 |
| W2 冷配置 | ✅ 已完成 | socchina-admin + generation/ETag + 回滚 |
| W3 热控制 | ✅ 已完成 | 8 参数实时调节 + 3 预设 + 200ms 防抖 |
| W4 安全交付 | ✅ 已完成 | 认证/CSRF/限流 + authproxy 全代理 + sandbox |

板端已部署验收（2026-06-24）：Gamma 亮度 + NN CLUT 色彩增强均可通过浏览器实时调节。

## 构建与部署

```sh
# 交叉编译 Go 二进制到 ARM64
scripts/build_web.sh

# 部署二进制、配置和静态资源到板端
BOARD=hispark-remote scripts/install_board_web.sh

# 验证部署正确性
BOARD=hispark-remote scripts/validate_board_web.sh
```

## 技术选型

- **后端**：Go 标准库（单一 ARM64 二进制，交叉编译，无运行时依赖）
- **前端**：原生 HTML/CSS/JavaScript（不引入 Node 构建链；HLS 播放库本地部署）
- **视频网关**：MediaMTX ARM64 固定版本（不做转码，只做协议转发）
- **协议**：JSON-over-Unix-socket（一行一个请求/响应）

## 权限模型

```text
socchina-web  用户：非特权（只能访问受限 Unix socket）
socchina 组：允许访问受限 socket
socchina-admin：root（仅暴露固定操作白名单）
```

## 板端依赖

Web 控制台依赖以下板端组件（不在 `web/` 范围内）：

| 组件 | 路径 | 说明 |
|------|------|------|
| app-control socket | `/tmp/socchina-app-control.sock` | `socchina_app` 内 Unix socket，解析热参数并串行应用到 ISP |
| admin socket | `/tmp/socchina-admin.sock` | `socchina-admin` 冷配置事务服务 |
| MediaMTX | `:8888` (HLS) `:8889` (WebRTC) `:8554` (RTSP) | 视频网关，无转码协议转发 |
| NN 推理 | `ENABLE_NN_CONTROL=1` in `runtime.conf` | 必须设 1 否则 `--no-nn` 导致 VPSS chn2 不创建 |

## 已知坑点

- **`nn_high_clip_guard` 解析 bug**：早期 C 代码偏移量错 1 字节，永远解析为 0.0。
  修在 `board/src/app_control_sock.c:182`（`p+20`→`p+21`）。
- **socket 路径不要 symlink**：systemd `ProtectSystem=strict` + `ReadWritePaths`
  下 `/run/socchina/` 目录权限 750 可能导致 symlink 不可达，直接用 `/tmp/`。
- **authproxy 的 SameSite**：IPv6 字面量地址下 `SameSite=Strict` 可能导致浏览器
  不发送 cookie；当前版本已移除该属性。
- **冷配置覆盖 `ENABLE_NN_CONTROL`**：冷配置 apply 会重写整个 `runtime.conf`，
  如果表单中没有 `ENABLE_NN_CONTROL` 字段会被删除。冷配置表单已精简，NN 开关
  只在热控制面板中。
