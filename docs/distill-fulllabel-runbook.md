# 路线 A 全量蒸馏标签 — 启动 Runbook

> 目的:在专属窗口(直播全程黑屏 ~4h)一键跑完 `distill_expand` 的 177 图板端评选,
> 产出 θ\* 硬件标签,离线微调 ParamNet。gen 已完成,文件已备好(见下"已就绪")。

## 成本与前提(务必知悉)
- **纯 replay ~3.9h**(实测 261 ms/blob × 306 sweep × 177 图),期间 **socchina-stream 必停 → 直播黑屏全程**。
- **不可中断**:test_raw_replay_b2 被 kill/timeout 会残留 MPP/VB(`ss_mpi_vb_set_conf failed 0xffffffff`),
  连 socchina_app 都起不来、视频挂死。脚本让每会话自然跑完;失败会自动 MPP 恢复并中止。
- 选一个不需要看视频的时段(如夜间)。SSH 会话要稳(建议 `tmux`/`nohup`,断连不杀脚本)。

## 已就绪(本会话已准备)
- 板端 `/root/socchina-2026/distill_run/`:**已齐**(2026-07-07 本会话):`pool/z000..304.bin`(305 候选)
  + `img00..176.raw`(177 张,2.1GB)。直连以太网推入(IP 192.168.1.168,~10.6 MB/s)。
- 板端评选工具:`/root/socchina-2026/test_raw_replay_b2`(支持 `--raw-file`;旧 `test_raw_replay` 不支持)。
- 一键脚本:`/root/socchina-2026/distill_run/board_distill_run_all.sh`(源在仓库 `scripts/board_distill_run_all.sh`)。
- 主机:`distill_expand/` 的 `img*_input.png`/`img*_gt.png`/`pool.pt`/`meta.json` 齐备;GPU + LCDP 数据集可离线微调。

## 启动(三步)

### ① 板端跑评选(~4h,黑屏)
```sh
# 用 tmux/nohup 保证断连不中断
ssh <board>
tmux new -s distill      # 或: nohup sh ... &
cd /root/socchina-2026/distill_run
sh board_distill_run_all.sh 2>&1 | tee run_all.log
# 结束打印 "完成 23 会话,NV21=...";脚本会自动复流
```

### ② 拉回 NV21(~11GB,~25min)到主机
```sh
mkdir -p /tmp/distill_board_out
rsync -a <board>:/root/socchina-2026/distill_run/out/ /tmp/distill_board_out/    # 或 scp -r
```

### ③ 离线打分 + 微调(主机,GPU)
```sh
cd <repo>
python -m models.isp_simulator.distill labels \
    --distill-dir models/weights/distill_expand --board-dir /tmp/distill_board_out
python -m models.isp_simulator.distill finetune \
    --distill-dir models/weights/distill_expand --check-valid --lam-d 1.0
# 产物: models/weights/distill_expand/{labels.pt,labels_report.json,paramnet_distill.pt}
```

## 判据
- `labels` 报告:θ\* 中位 vs ParamNet 中位,平均增益 > 0(尤其 over/逆光桶趋近 θ\* 上限)。
- `finetune --check-valid`:LCDP valid 代理口径**不回归**(排练式防遗忘)。
- 真验收:把 `paramnet_distill.pt` 导 OM 上板复跑,跨场景一致提升(见 `docs/isp-param-tuning-agent-prompt.md` §4)。

## 故障恢复
- 某会话失败:脚本已自动 `load_ss928v100 -a` 恢复 MPP。查 `distill_run/sess_<s>.log` 尾部定位;
  已完成会话的 `out/` 保留,修好后可重跑(会覆盖/补齐)。
- 视频没回来:手动 `cd /opt/ko && ./load_ss928v100 -a && systemctl start socchina-stream`。
- 磁盘:raw 2.15GB + NV21 11GB,板端需 ≥14GB 空闲(当前 ~20GB 空闲够;跑前 `df -h /` 确认)。
