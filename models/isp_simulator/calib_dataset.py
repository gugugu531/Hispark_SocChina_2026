#!/usr/bin/env python3
"""阶段 1 残差校准数据集——LHS 参数采样、板端批量采集、残差量化基线。

工作流（docs/isp-param-tuning-research.md §4.4 阶段 1）:
    # 1. LHS 采样 K 组参数 → blob 目录（c000_neutral + c001..cK）
    python -m models.isp_simulator.calib_dataset gen --outdir models/weights/calib --num 128

    # 2. 板端批量采集（合成场景 × 全部参数，单会话）
    ./test_raw_replay --settle 8 --out 512x288 --outdir out_calib \
        --raw-file synth_*.raw ... --blob-dir /root/socchina-2026/fidelity/calib

    # 3. 残差量化基线：sim(中性帧, θ) vs hw 输出的系统性偏差
    python -m models.isp_simulator.calib_dataset report \
        --calib-dir models/weights/calib --board-dir <拉回目录> --scenes 8

采样空间 = 板端 blob 覆盖范围（DRC tone/mix/ctrl/blend/strength + LDCI，31 有效维），
偏向"提亮"用途（曝光校正）；tone 控制点 cummax 保单调。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import torch

from models.isp_simulator import ISPPipeline, make_identity_params
from models.isp_simulator.isp_blob import sim_params_to_blob
from models.isp_simulator.params import get_offset

SEED = 20260704


def lhs(n: int, dims: int, rng: np.random.Generator) -> np.ndarray:
    """拉丁超立方采样 (n, dims) ∈ [0,1)。每维分层 + 独立洗牌。"""
    u = (rng.random((n, dims)) + np.arange(n)[:, None]) / n
    for d in range(dims):
        u[:, d] = u[rng.permutation(n), d]
    return u


def sample_params(num: int, rng: np.random.Generator, version: int = 2) -> torch.Tensor:
    """LHS → (num, 97) 参数向量。

    v1: tone 偏提亮 [-0.10,+0.30]，无 Gamma —— §5.9 归因证实压暗域覆盖不足。
    v2: tone 对称 ±0.30 + Gamma γ 幂曲线维（θ* 诊断后的修正，2026-07-04）。
    """
    # 采样维：tone(4) + strength(1) + ldci(8) + mix(12) + ctrl(3) + blend(1) + gamma γ(1) = 30
    u = lhs(num, 30, rng)
    params = make_identity_params(num)

    off_t = get_offset("drc_tone")
    base = torch.linspace(0.0, 1.0, 6)
    lo, span = (-0.30, 0.60) if version >= 2 else (-0.10, 0.40)
    for i in range(num):
        cp = base.clone()
        delta = torch.from_numpy(u[i, 0:4]).float() * span + lo
        cp[1:5] = (cp[1:5] + delta).clamp(0.0, 1.0)
        cp = torch.cummax(cp, dim=0).values
        cp[0], cp[5] = 0.0, 1.0
        params[i, off_t:off_t + 6] = cp

    if version >= 2:
        # Gamma：γ ∈ [0.45, 1.55] 幂曲线（γ<1 提亮），64 节点写入参数向量
        off_g = get_offset("gamma")
        gam = 0.45 + u[:, 29] * 1.10
        nodes = torch.linspace(0.0, 1.0, 64)
        for i in range(num):
            params[i, off_g:off_g + 64] = nodes.pow(float(gam[i]))

    params[:, get_offset("drc_strength")] = torch.from_numpy(u[:, 4]).float()  # [0,1] 全程

    off_l = get_offset("ldci")
    ld = np.empty((num, 8), dtype=np.float32)
    ld[:, 0] = u[:, 5] * 0.90            # he_pos wgt
    ld[:, 1] = 0.2 + u[:, 6] * 0.6       # he_pos sigma
    ld[:, 2] = 0.1 + u[:, 7] * 0.5       # he_pos mean
    ld[:, 3] = u[:, 8] * 0.60            # he_neg wgt
    ld[:, 4] = 0.2 + u[:, 9] * 0.6       # he_neg sigma
    ld[:, 5] = 0.4 + u[:, 10] * 0.5      # he_neg mean
    ld[:, 6] = u[:, 11] * 0.5            # blc_ctrl
    ld[:, 7] = 0.2 + u[:, 12] * 0.6      # gauss_lpf_sigma
    params[:, off_l:off_l + 8] = torch.from_numpy(ld)

    off_m = get_offset("drc_mix")
    params[:, off_m:off_m + 12] = torch.from_numpy(0.2 + u[:, 13:25] * 0.7).float()
    off_c = get_offset("drc_ctrl")
    params[:, off_c:off_c + 3] = torch.from_numpy(0.2 + u[:, 25:28] * 0.6).float()
    params[:, get_offset("drc_blend")] = torch.from_numpy(0.2 + u[:, 28] * 0.6).float()
    return params


def cmd_gen(args) -> int:
    outdir = Path(args.outdir)
    (outdir / "blobs").mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(SEED)
    params = sample_params(args.num, rng)

    # c000 = 中性（DRC/LDCI 显式关闭 + Gamma strength=0 还原默认）
    neutral = make_identity_params(1)
    (outdir / "blobs" / "c000_neutral.bin").write_bytes(
        sim_params_to_blob(neutral, drc_on=False, ldci_on=False,
                           gamma_on=True, gamma_strength=0.0))
    for i in range(args.num):
        (outdir / "blobs" / f"c{i + 1:03d}.bin").write_bytes(
            sim_params_to_blob(params[i:i + 1], gamma_on=True, gamma_strength=1.0))

    torch.save(params, outdir / "params.pt")
    (outdir / "manifest.json").write_text(json.dumps(
        {"num": args.num, "seed": SEED, "note": "c000=neutral, c<i>=params[i-1]"},
        indent=2))
    print(f"{args.num} 组参数 + 中性 blob -> {outdir}/blobs")
    print("板端: ./test_raw_replay --settle 8 --out 512x288 --raw-file ... "
          f"--blob-dir <板端路径>/blobs --outdir out_calib")
    return 0


# ── 残差量化 ──────────────────────────────────────────────────


def cmd_report(args) -> int:
    from models.isp_simulator.fidelity_gate import load_nv21_rgb, luma

    calib_dir = Path(args.calib_dir)
    board_dir = Path(args.board_dir)
    params = torch.load(calib_dir / "params.pt")
    num = params.shape[0]
    w, h = args.width, args.height
    pipeline = ISPPipeline()

    def frame(fi: int, si: int, tag: str) -> np.ndarray:
        p = board_dir / f"out_f{fi:02d}_{si:02d}_{tag}.nv21"
        if not p.exists():
            sys.exit(f"缺板端输出: {p}")
        return load_nv21_rgb(p, w, h)

    print(f"{'scene':>6} | {'PSNR中位':>8} {'PSNR p5':>8} | {'|Δluma|中位':>10} {'最差参数':>8}")
    all_res = []
    per_scene = []
    for fi in range(args.scenes):
        # sweep 索引：0=baseline，1=c000_neutral，2..num+1=c001..cNNN
        neutral = frame(fi, 1, "blob_c000_neutral.bin")
        x = torch.from_numpy(neutral).float().permute(2, 0, 1).unsqueeze(0)
        psnrs, dlumas = [], []
        for i in range(num):
            hw = frame(fi, i + 2, f"blob_c{i + 1:03d}.bin")
            with torch.no_grad():
                sim = pipeline(x, params[i:i + 1], enable_wdr=False, enable_gamma=False,
                               enable_dehaze=False)["output"]
            sim_np = sim.squeeze(0).permute(1, 2, 0).numpy()
            err = hw.astype(np.float32) - sim_np
            mse = float((err ** 2).mean())
            psnrs.append(10.0 * np.log10(1.0 / max(mse, 1e-10)))
            dlumas.append(float(np.abs(luma(hw) - luma(sim_np)).mean()))
            all_res.append({"scene": fi, "param": i, "psnr": psnrs[-1], "dluma": dlumas[-1],
                            "in_luma": float(luma(neutral).mean())})
        psnrs_np = np.array(psnrs)
        worst = int(np.argmin(psnrs_np))
        per_scene.append({"scene": fi, "psnr_med": float(np.median(psnrs_np)),
                          "psnr_p5": float(np.percentile(psnrs_np, 5)),
                          "dluma_med": float(np.median(dlumas))})
        print(f"  f{fi:02d}  | {np.median(psnrs_np):>8.2f} {np.percentile(psnrs_np, 5):>8.2f} "
              f"| {np.median(dlumas):>10.4f} c{worst + 1:03d}")

    psnr_all = np.array([r["psnr"] for r in all_res])
    print(f"\n总体: n={len(all_res)}  PSNR 中位={np.median(psnr_all):.2f} dB  "
          f"p5={np.percentile(psnr_all, 5):.2f}  p95={np.percentile(psnr_all, 95):.2f}")

    # 残差与输入亮度的关系（极暗域外推问题的量化）
    in_lumas = np.array([r["in_luma"] for r in all_res])
    dluma_all = np.array([r["dluma"] for r in all_res])
    print("按场景输入亮度分组的 |Δluma| 中位:")
    for lo, hi in ((0.0, 0.1), (0.1, 0.25), (0.25, 1.0)):
        m = (in_lumas >= lo) & (in_lumas < hi)
        if m.any():
            print(f"  输入亮度 [{lo:.2f},{hi:.2f}): {np.median(dluma_all[m]):.4f} (n={m.sum()})")

    out = calib_dir / "residual_report.json"
    out.write_text(json.dumps({"per_scene": per_scene,
                               "overall": {"psnr_med": float(np.median(psnr_all)),
                                           "psnr_p5": float(np.percentile(psnr_all, 5))}},
                              indent=2))
    print(f"报告: {out}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("gen", help="LHS 采样参数集 → blob 目录")
    g.add_argument("--outdir", default="models/weights/calib")
    g.add_argument("--num", type=int, default=128)

    r = sub.add_parser("report", help="残差量化基线（sim vs hw）")
    r.add_argument("--calib-dir", default="models/weights/calib")
    r.add_argument("--board-dir", required=True)
    r.add_argument("--scenes", type=int, default=8)
    r.add_argument("--width", type=int, default=512)
    r.add_argument("--height", type=int, default=288)

    args = ap.parse_args()
    return cmd_gen(args) if args.cmd == "gen" else cmd_report(args)


if __name__ == "__main__":
    sys.exit(main())
