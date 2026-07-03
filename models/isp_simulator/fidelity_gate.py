#!/usr/bin/env python3
"""保真度闸门——解析模拟器 vs SS928 硬件 ISP 的秩相关裁决（阶段 0，
docs/isp-param-tuning-research.md §4.4）。

工作流:
    # 1. 主机生成 sweep 参数集与 blob 文件
    python -m models.isp_simulator.fidelity_gate gen --outdir models/weights/fidelity

    # 2. 板端采集（独占媒体链；命令由 gen 打印，形如）
    ./test_raw_replay --blob b00_neutral.bin --blob b01_t1.bin ... --outdir replay

    # 3. 拉回 out_*.nv21 后主机分析
    python -m models.isp_simulator.fidelity_gate analyze \
        --sweep-dir models/weights/fidelity --board-dir <拉回的 replay 目录>

判决口径:
    - 模拟器输入 = 板端"中性帧"（blob 显式关闭 DRC/LDCI 后的输出），
      模拟器只开 DRC+LDCI 施加同一参数（与 blob 覆盖范围一致）。
    - 对每个质量特征，计算 K 组参数下 硬件特征序列 vs 模拟器特征序列 的
      Spearman 秩相关；训练需要的是排序一致而非逐像素一致（>0.7 视为可用）。
    - 另报告逐参数方向一致率 sign(Δfeature vs 中性帧)。
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

# ── sweep 设计 ────────────────────────────────────────────────


def build_sweep() -> list[dict]:
    """构造 12 组参数（+1 中性帧），覆盖 DRC tone 梯度 / LDCI 梯度 / mix / 组合。

    梯度序列是秩相关有效性的前提：每个维度至少 3 档单调变化。
    """
    items = []

    def make(tag, mutate, drc_on=True, ldci_on=True, strength=512):
        p = make_identity_params(1)
        mutate(p)
        # strength 写进参数向量：blob 生成端读它，模拟器 drc_strength_apply 施加它
        p[0, get_offset("drc_strength")] = strength / 1023.0
        items.append({"tag": tag, "params": p, "drc_on": drc_on, "ldci_on": ldci_on,
                      "strength": strength})

    # 中性帧：blob 显式关闭 DRC/LDCI（模拟器输入的采集口径）
    make("neutral", lambda p: None, drc_on=False, ldci_on=False, strength=0)

    # T1-T5: DRC tone 暗部提升梯度（保单调，亮部渐压；strength 固定 512 与 v1 可比）
    off_t = get_offset("drc_tone")
    for i, a in enumerate([0.05, 0.10, 0.15, 0.20, 0.25], start=1):
        def mut(p, a=a):
            cp = torch.linspace(0.0, 1.0, 6)
            cp[1] += a
            cp[2] += a * 0.6
            cp[3] += a * 0.3
            p[0, off_t:off_t + 6] = cp.clamp(0.0, 1.0)
        make(f"t{i}", mut)

    # L1-L4: LDCI he_pos_wgt 梯度。strength=0 + 恒等 tone 让 DRC 近似直通，
    # 隔离 LDCI 净效应（v1 教训：strength=512 时 DRC 效果淹没 LDCI ±0.015 的信号）。
    off_l = get_offset("ldci")
    for i, w in enumerate([0.15, 0.35, 0.60, 0.85], start=1):
        def mut(p, w=w):
            p[0, off_l:off_l + 3] = torch.tensor([w, 0.5, 0.25])
        make(f"l{i}", mut, strength=0)

    # S1-S3: DRC manual strength 梯度（恒等 tone、LDCI 关）——硬件独有维度，
    # 模拟器未建模；采集响应曲线供残差校准/后续建模。
    for i, s in enumerate([128, 512, 896], start=1):
        make(f"s{i}", lambda p: None, strength=s)

    # M1-M2: DRC local mixing 梯度（tone 固定中档提升）
    off_m = get_offset("drc_mix")
    for i, m in enumerate([0.25, 0.85], start=1):
        def mut(p, m=m):
            cp = torch.linspace(0.0, 1.0, 6)
            cp[1] += 0.15
            cp[2] += 0.09
            cp[3] += 0.045
            p[0, off_t:off_t + 6] = cp.clamp(0.0, 1.0)
            p[0, off_m:off_m + 12] = m
        make(f"m{i}", mut)

    # C1: 组合（强 tone + 强 LDCI）
    def mut_c1(p):
        cp = torch.linspace(0.0, 1.0, 6)
        cp[1] += 0.20
        cp[2] += 0.12
        cp[3] += 0.06
        p[0, off_t:off_t + 6] = cp.clamp(0.0, 1.0)
        p[0, off_l:off_l + 3] = torch.tensor([0.60, 0.5, 0.25])
    make("c1", mut_c1)

    return items


def cmd_gen(args) -> int:
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    items = build_sweep()

    manifest = []
    blob_args = []
    all_params = []
    for i, it in enumerate(items):
        fname = f"b{i:02d}_{it['tag']}.bin"
        blob = sim_params_to_blob(it["params"], drc_on=it["drc_on"], ldci_on=it["ldci_on"])
        (outdir / fname).write_bytes(blob)
        manifest.append({"idx": i, "tag": it["tag"], "blob": fname,
                         "drc_on": it["drc_on"], "ldci_on": it["ldci_on"],
                         "strength": it["strength"]})
        blob_args.append(f"--blob {fname}")
        all_params.append(it["params"])

    torch.save(torch.cat(all_params, dim=0), outdir / "sweep_params.pt")
    (outdir / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False))

    print(f"生成 {len(items)} 个 blob -> {outdir}")
    print("\n板端采集命令（先 systemctl stop socchina-stream）:")
    print(f"  scp {outdir}/b*.bin build/test_raw_replay <BOARD>:/root/socchina-2026/fidelity/")
    print("  cd /root/socchina-2026/fidelity && mkdir -p out && LD_LIBRARY_PATH=/opt/lib/npu \\")
    print("  ./test_raw_replay --outdir out --save-raw " + " ".join(blob_args))
    return 0


# ── 分析 ──────────────────────────────────────────────────────


def load_nv21_rgb(path: Path, w: int, h: int) -> np.ndarray:
    """NV21 → RGB float [0,1]（BT.601 full-range 近似；秩相关对仿射差异不敏感）。"""
    d = np.fromfile(path, dtype=np.uint8)
    assert d.size == w * h * 3 // 2, f"{path}: size {d.size} != NV21 {w}x{h}"
    y = d[:w * h].reshape(h, w).astype(np.float32)
    vu = d[w * h:].reshape(h // 2, w // 2, 2).astype(np.float32)
    v = np.repeat(np.repeat(vu[..., 0], 2, 0), 2, 1) - 128.0
    u = np.repeat(np.repeat(vu[..., 1], 2, 0), 2, 1) - 128.0
    r = y + 1.402 * v
    g = y - 0.344136 * u - 0.714136 * v
    b = y + 1.772 * u
    return np.clip(np.stack([r, g, b], axis=-1) / 255.0, 0.0, 1.0)


def luma(img: np.ndarray) -> np.ndarray:
    return 0.299 * img[..., 0] + 0.587 * img[..., 1] + 0.114 * img[..., 2]


def features(img: np.ndarray, dark_mask: np.ndarray, bright_mask: np.ndarray) -> dict:
    """帧级质量特征（mask 由中性帧决定，跨参数组固定，保证可比）。"""
    y = luma(img)
    gy, gx = np.gradient(y)
    return {
        "mean": float(y.mean()),
        "shadow": float(y[dark_mask].mean()) if dark_mask.any() else 0.0,
        "highlight": float(y[bright_mask].mean()) if bright_mask.any() else 0.0,
        "std": float(y.std()),
        "grad": float(np.hypot(gx, gy).mean()),
    }


def spearman(a: np.ndarray, b: np.ndarray) -> float:
    ra = np.argsort(np.argsort(a)).astype(np.float64)
    rb = np.argsort(np.argsort(b)).astype(np.float64)
    ra -= ra.mean()
    rb -= rb.mean()
    denom = np.sqrt((ra ** 2).sum() * (rb ** 2).sum())
    return float((ra * rb).sum() / denom) if denom > 0 else 0.0


def cmd_analyze(args) -> int:
    sweep_dir = Path(args.sweep_dir)
    board_dir = Path(args.board_dir)
    w, h = args.width, args.height

    manifest = json.loads((sweep_dir / "manifest.json").read_text())
    all_params = torch.load(sweep_dir / "sweep_params.pt")

    # 板端输出命名：定格模式 out_<idx+1>_blob_<b>.nv21（第 0 项是 harness 基线）；
    # 文件回灌模式 out_f<fi>_<idx>_blob_<b>.nv21（无基线偏移，--file-idx 选场景）
    def board_frame(idx: int, blob: str) -> Path:
        if args.file_idx is not None:
            p = board_dir / f"out_f{args.file_idx:02d}_{idx + 1:02d}_blob_{blob}.nv21"
        else:
            p = board_dir / f"out_{idx + 1:02d}_blob_{blob}.nv21"
        if not p.exists():
            sys.exit(f"缺板端输出: {p}")
        return p

    neutral_entry = manifest[0]
    assert neutral_entry["tag"] == "neutral"
    neutral_hw = load_nv21_rgb(board_frame(0, neutral_entry["blob"]), w, h)

    # mask 由中性帧固定
    ny = luma(neutral_hw)
    dark_mask = ny < 0.25
    bright_mask = ny > 0.60

    # 模拟器：中性帧为输入，只开 DRC+LDCI
    pipeline = ISPPipeline()
    x = torch.from_numpy(neutral_hw).float().permute(2, 0, 1).unsqueeze(0)

    rows = []
    for entry in manifest[1:]:
        hw = load_nv21_rgb(board_frame(entry["idx"], entry["blob"]), w, h)
        p = all_params[entry["idx"]:entry["idx"] + 1]
        with torch.no_grad():
            sim = pipeline(x, p, enable_wdr=False, enable_gamma=False,
                           enable_dehaze=False)["output"]
        sim_np = sim.squeeze(0).permute(1, 2, 0).numpy()
        rows.append({
            "tag": entry["tag"],
            "hw": features(hw, dark_mask, bright_mask),
            "sim": features(sim_np, dark_mask, bright_mask),
        })

    neutral_feat = features(neutral_hw, dark_mask, bright_mask)

    print(f"\n{'tag':>8} | {'hw_shadow':>9} {'sim_shadow':>10} | {'hw_mean':>8} {'sim_mean':>8}")
    for r in rows:
        print(f"{r['tag']:>8} | {r['hw']['shadow']:>9.4f} {r['sim']['shadow']:>10.4f} "
              f"| {r['hw']['mean']:>8.4f} {r['sim']['mean']:>8.4f}")

    print(f"\n中性帧: shadow={neutral_feat['shadow']:.4f} mean={neutral_feat['mean']:.4f} "
          f"(dark {dark_mask.mean() * 100:.0f}% / bright {bright_mask.mean() * 100:.0f}%)")

    # 模拟器已建模 strength（板端标定幂函数，见 drc.drc_strength_apply），
    # s 组一并纳入对比。
    cmp_rows = rows

    print("\n== Spearman 秩相关（硬件排序 vs 模拟器排序，全部 sweep 项）==")
    verdicts = {}
    for key in ("mean", "shadow", "highlight", "std", "grad"):
        hw_v = np.array([r["hw"][key] for r in cmp_rows])
        sim_v = np.array([r["sim"][key] for r in cmp_rows])
        rho = spearman(hw_v, sim_v)
        # 方向一致率（相对中性帧）
        dh = np.sign(hw_v - neutral_feat[key])
        ds = np.sign(sim_v - neutral_feat[key])
        agree = float((dh == ds).mean())
        verdicts[key] = rho
        print(f"  {key:>9}: rho={rho:+.3f}  direction-agree={agree * 100:.0f}%")

    # 分组秩相关（tone 梯度 / LDCI 梯度是主判据：单模块维度内的排序保真。
    # 全体混合 rho 在模拟器建模 strength 之前预期偏低——硬件 strength 是跨组
    # 幅度错位的已知根因，仅作参考指标。）
    print("\n== 分组秩相关（单调梯度组内，主判据）==")
    group_rho = {}
    for group, prefix in (("DRC tone", "t"), ("LDCI", "l")):
        sub = [r for r in rows if r["tag"].startswith(prefix)]
        if len(sub) >= 3:
            hw_v = np.array([r["hw"]["shadow"] for r in sub])
            sim_v = np.array([r["sim"]["shadow"] for r in sub])
            rho = spearman(hw_v, sim_v)
            group_rho[prefix] = rho
            print(f"  {group:>9} (shadow, n={len(sub)}): rho={rho:+.3f}")

    # s 组：硬件 strength 响应 vs 模拟器标定模型（gamma=exp(-1.4*s/1023)）
    s_rows = [r for r in rows if r["tag"].startswith("s")]
    if s_rows:
        print("\n== DRC strength 响应（硬件 vs 板端标定幂函数模型）==")
        for r in s_rows:
            print(f"  {r['tag']}: hw shadow={r['hw']['shadow']:.4f} "
                  f"sim shadow={r['sim']['shadow']:.4f} "
                  f"(中性 {neutral_feat['shadow']:.4f})")
        hw_v = np.array([r["hw"]["shadow"] for r in s_rows])
        sim_v = np.array([r["sim"]["shadow"] for r in s_rows])
        print(f"  strength 组内 rho={spearman(hw_v, sim_v):+.3f}")

    gate = group_rho.get("t", -1.0) > 0.7 and group_rho.get("l", -1.0) > 0.7
    print(f"\n闸门判定（主判据：tone/LDCI 组内 rho>0.7）: {'PASS' if gate else 'FAIL'}")
    print(f"  参考：全体混合 shadow rho={verdicts['shadow']:+.3f}")
    return 0 if gate else 2


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("gen", help="生成 sweep blob 集与板端命令")
    g.add_argument("--outdir", default="models/weights/fidelity")

    a = sub.add_parser("analyze", help="板端输出 vs 模拟器秩相关分析")
    a.add_argument("--sweep-dir", default="models/weights/fidelity")
    a.add_argument("--board-dir", required=True)
    a.add_argument("--width", type=int, default=1024)
    a.add_argument("--height", type=int, default=576)
    a.add_argument("--file-idx", type=int, default=None,
                   help="文件回灌模式的场景序号（out_f<NN>_ 前缀）")

    args = ap.parse_args()
    return cmd_gen(args) if args.cmd == "gen" else cmd_analyze(args)


if __name__ == "__main__":
    sys.exit(main())
