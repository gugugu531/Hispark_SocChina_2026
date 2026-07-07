# Agent Prompt: 板端陈旧二进制与工程文件精准清理

## 任务
清理板端 `/root/socchina-2026/` 中已无实用价值的陈旧二进制、OM 和过期备份，释放整洁空间。

## 连接方式
板端直连以太网:`ssh hispark-direct`(= `root@192.168.1.168`)

## 清理清单(按类)

### 1. 闭合路线的 CTBG OM(4 个,均已无实用价值)
CTBG 作为主增强路径已正式关闭(§3),per-pixel apply ~89ms/帧无实用价值,AIPP OM 因 TransData 缺位不可部署:
```
/root/socchina-2026/ctbg6ch_apply_1024x576.om       # v9 6ch apply(无 AIPP)
/root/socchina-2026/ctbg6ch_apply_aipp_1024x576.om   # v9 AIPP apply(ATC 缺陷,不可用)
/root/socchina-2026/ctbg6ch_estimator_256x144.om     # v9 estimator
/root/socchina-2026/ctbg_apply_twostage_nn_aipp_1024x576.om  # 两阶段 AIPP(同上缺陷)
```

### 2. 陈旧实验构建 socchina_app(4 个)
这些是 CTBG/早期开发阶段的实验二进制,活动 `socchina_app`(Jul 7)已覆盖全部功能:
```
/root/socchina-2026/socchina_app_new       # Jun 24  CTBG 早期实验
/root/socchina-2026/socchina_app_recal     # Jun 24  校准实验
/root/socchina-2026/socchina_app_ctbg      # Jul 1   CTBG 实验
/root/socchina-2026/socchina_app.orig      # Jun 30  原始基线
```

### 3. 陈旧备份(2 个,本轮 ~/.bak-<今日> 系列保留不动)
```
/root/socchina-2026/socchina_app.bak       # Jun 24(旧)
/root/socchina-2026/socchina_app.bak2      # Jul 1(旧)
```

## 🛡️ 明确保留(绝不删除)

| 文件 | 原因 |
|---|---|
| `socchina_app` | ⭐ **活动二进制**(Jul 7,最新构建,socchina-stream 引用) |
| `socchina_app_b2` | B2 ParamNet 实时闭环二进制(Jul 6),**主线 B2 runtime 深验待用** |
| `socchina_app_lvgl` | LVGL 触摸 UI 二进制(Jul 6),**LVGL 上板收尾待用** |
| `*.bak-<今日>` | 本次会话部署的回滚点(socchina_app/web/auth/admin 各一份) |
| `socchina-web` / `socchina-web.bak` | Web 后端当前 + 上一版 |
| `cotf_paramnet_256x144_lcdp_best_e0167_fp16.om` | 当前 MODEL_PATH(runtime.conf 引用) |
| `cotf_paramnet_256x144_lcdp_best_e0167_fp16_aipp.om` | AIPP 部署版(路线 B2 用) |
| `paramnet_256x144_aipp.om` | B2 冒烟验证 OM |
| `test_raw_replay` / `test_raw_replay_b2` | 路线 A 板端评选工具 |
| `distill_run/` | 路线 A 全量数据(raw + pool + 脚本) |
| `*.json` / `*.sh` | 配置文件/脚本 |

## 执行
通过 SSH 逐条 `rm` 或一次性:
```sh
ssh hispark-direct 'cd /root/socchina-2026 && rm -v \
  ctbg6ch_apply_1024x576.om \
  ctbg6ch_apply_aipp_1024x576.om \
  ctbg6ch_estimator_256x144.om \
  ctbg_apply_twostage_nn_aipp_1024x576.om \
  socchina_app_new \
  socchina_app_recal \
  socchina_app_ctbg \
  socchina_app.orig \
  socchina_app.bak \
  socchina_app.bak2'
```

## 验证
```sh
ssh hispark-direct 'ls -la /root/socchina-2026/socchina_app*'
# 应仅保留: socchina_app, socchina_app_b2, socchina_app_lvgl, socchina_app.bak-<TS>
```
