#!/bin/sh
# board_distill_run_all.sh — 路线 A 全量蒸馏标签:板端评选一键脚本。
#
# 在板端(/root/socchina-2026/distill_run 已备好 img*.raw + pool/z*.bin)运行:
#     sh board_distill_run_all.sh
#
# 做什么:停 socchina-stream → 按 ≤8 图/会话调用 test_raw_replay_b2 跑完全部
#         NIMG 张 raw × 候选池 → 把每会话本地 f00..f07 重命名为全局 fi(供
#         distill.py cmd_labels 的 out_f<fi>_<k+1>_blob_z<k>.bin.nv21 契约)→ 复流。
#
# ⚠️ 关键:test_raw_replay_b2 绝不能中途被 kill/timeout——SIGTERM 不清 MPP/VB,
#    残留会让后续 ss_mpi_vb_set_conf failed 0xffffffff、连 socchina_app 都起不来、
#    视频挂死。本脚本让每会话自然跑完;某会话失败则自动 MPP 恢复并中止,便于排查。
#    手动恢复:cd /opt/ko && ./load_ss928v100 -a && systemctl start socchina-stream
#
# 产出:distill_run/out/*.nv21(全局命名,约 NIMG×306 个,~11GB);拉回主机后跑
#     python -m models.isp_simulator.distill labels --distill-dir models/weights/distill_expand --board-dir <拉回目录>
#     python -m models.isp_simulator.distill finetune --distill-dir models/weights/distill_expand --check-valid

set -u

RUN="${DISTILL_RUN:-/root/socchina-2026/distill_run}"
BIN="${REPLAY_BIN:-/root/socchina-2026/test_raw_replay_b2}"
NIMG="${NIMG:-177}"
BATCH=8
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-/opt/lib/npu}"

OUT="$RUN/out"
recover() { echo "[distill] MPP 恢复中…"; ( cd /opt/ko && ./load_ss928v100 -a ) >/dev/null 2>&1; }

[ -x "$BIN" ] || { echo "[distill] 缺 test_raw_replay_b2: $BIN"; exit 2; }
[ -d "$RUN/pool" ] || { echo "[distill] 缺候选池 $RUN/pool"; exit 2; }
got=$(ls "$RUN"/img*.raw 2>/dev/null | wc -l)
[ "$got" -ge "$NIMG" ] || { echo "[distill] raw 不足: 有 $got 需 $NIMG"; exit 2; }

echo "[distill] 停 socchina-stream(视频将黑屏至全部完成)"; systemctl stop socchina-stream; sleep 2
mkdir -p "$OUT"

s=0; i=0; t0=$(date +%s)
while [ "$i" -lt "$NIMG" ]; do
    args=""; base=$i; n=0
    while [ "$n" -lt "$BATCH" ] && [ "$i" -lt "$NIMG" ]; do
        args="$args --raw-file $(printf 'img%02d.raw' "$i")"
        i=$((i+1)); n=$((n+1))
    done
    echo "[distill] 会话 $s: 图 $base..$((i-1)) ($n 图) $(date +%T)"
    rm -rf "$RUN/out_tmp"; mkdir -p "$RUN/out_tmp"
    ( cd "$RUN" && "$BIN" --settle 8 --out 512x288 --exptime 8000 --again 1024 \
        --outdir out_tmp $args --blob-dir pool ) > "$RUN/sess_$s.log" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "[distill] 会话 $s 失败 rc=$rc(见 $RUN/sess_$s.log 尾部）:"; tail -3 "$RUN/sess_$s.log"
        recover; echo "[distill] 已恢复 MPP;排查后可重跑(已完成会话的 out/ 保留)"; exit 1
    fi
    L=0
    while [ "$L" -lt "$n" ]; do
        g=$((base+L)); lp=$(printf '%02d' "$L"); gp=$(printf '%02d' "$g")
        for f in "$RUN"/out_tmp/out_f${lp}_*; do
            [ -e "$f" ] || continue
            bn=$(basename "$f")
            mv "$f" "$OUT/$(echo "$bn" | sed "s/^out_f${lp}_/out_f${gp}_/")"
        done
        L=$((L+1))
    done
    rm -rf "$RUN/out_tmp"
    s=$((s+1))
done

nv=$(ls "$OUT"/*.nv21 2>/dev/null | wc -l)
echo "[distill] 完成 $s 会话,NV21=$nv,用时 $(( ($(date +%s)-t0)/60 )) 分钟 $(date +%T)"
echo "[distill] 复流"; systemctl start socchina-stream
echo "[distill] 下一步(主机):拉回 $OUT → distill labels/finetune(见脚本头注释)"
