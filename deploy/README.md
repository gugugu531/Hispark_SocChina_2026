# deploy — 板端部署资源

## 目录

| 路径 | 用途 |
| --- | --- |
| `systemd/` | systemd 服务单元文件 |
| `systemd/runtime.conf` | 核心流运行时配置（Config-R schema v1） |
| `mediamtx/mediamtx.yml` | MediaMTX 视频网关配置（由 `scripts/install_mediamtx.sh` 部署到板端） |
| `web.conf` | Web 控制台配置 |

## systemd 服务

| 服务 | 用户 | 依赖 | 说明 |
| --- | --- | --- | --- |
| `socchina-stream.service` | root | rc-local.service | 核心相机 + ISP + RTSP 流 |
| `socchina-mediamtx.service` | root | socchina-stream | MediaMTX 视频网关（无转码） |
| `socchina-web.service` | socchina-web | socchina-mediamtx | Web 控制台（REST + SSE） |
| `socchina-admin.service` | root | socchina-web | 冷配置事务管理 |

```text
socchina-stream.service
  -> socchina-mediamtx.service
  -> socchina-web.service

socchina-admin.service
  -> socchina-web.service
```

## MediaMTX

- 发行版：ARM64 单文件二进制
- 固定版本：建议使用经过验收的 1.x 版本，不自动升级
- 下载后放置于板端 `/usr/local/bin/mediamtx`
- 配置文件：`/opt/socchina/mediamtx/mediamtx.yml`（由 `deploy/mediamtx/mediamtx.yml` 复制，见 `scripts/install_mediamtx.sh`）
- 日志默认输出到 journald（`journalctl -u socchina-mediamtx`）

### 验收版本记录

| 日期 | 版本 | 校验值 (sha256) | 验收人 | 备注 |
| --- | --- | --- | --- | --- |
| — | — | — | — | 待首次部署验收后填写 |

## 部署步骤

```sh
# 1. 构建板端核心程序
scripts/build_board.sh
BOARD=hispark-remote scripts/deploy_board.sh

# 2. 构建并部署 Web 控制台
scripts/build_web.sh
BOARD=hispark-remote scripts/install_board_web.sh

# 3. 部署 MediaMTX 二进制（手动下载到板端）
# 从 https://github.com/bluenviron/mediamtx/releases 下载 ARM64 版本
# scp mediamtx hispark-remote:/usr/local/bin/
# ssh hispark-remote chmod 755 /usr/local/bin/mediamtx

# 4. 启用所有服务
ssh hispark-remote systemctl enable --now socchina-stream.service
ssh hispark-remote systemctl enable --now socchina-mediamtx.service
ssh hispark-remote systemctl enable --now socchina-web.service
ssh hispark-remote systemctl enable --now socchina-admin.service

# 5. 验证
BOARD=hispark-remote scripts/validate_board_web.sh
```
