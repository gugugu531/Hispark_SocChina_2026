#!/usr/bin/env python3
"""阶段 3 蒸馏——θ* 硬件标签绕开代理保真度上限（docs/isp-param-tuning-research.md §5.13）。

代理保真度 ~22dB 是硬约束（§5.12/5.13），代理训练的 ParamNet 在过曝方向离硬件 θ*
上限差 2.7–6.8dB 且会过压微调图。蒸馏：真实 ISP 上逐图搜 θ*，`scene→u*` 参数空间
回归微调 ParamNet（训练环无 ISP，梯度全程可微；硬件只在离线产标签）。

工作流:
    # 1. 选图 + 候选池(LHS+γ + 每图 ParamNet warmstart 保底 θ*≥当前网络)
    python -m models.isp_simulator.distill gen --outdir models/weights/distill --num 16 --cands 128
    # 2. 板端(单会话, ≤16 图): 停 stream → test_raw_replay --raw-file ... --blob-dir pool
    # 3. 逐图 θ* 标签 + 报告(θ* vs ParamNet)
    python -m models.isp_simulator.distill labels --distill-dir models/weights/distill --board-dir <拉回>
    # 4. scene→u* 回归微调(L2-SP 防遗忘)+ LCDP valid 不回归检查
    python -m models.isp_simulator.distill finetune --distill-dir models/weights/distill
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F

from models.isp_simulator.calib_dataset import lhs
from models.isp_simulator.isp_blob import sim_params_to_blob
from models.isp_simulator.paramnet import (ParamNet, u_to_theta, U_DIM, NET_W, NET_H,
                                           PROXY_W, PROXY_H, LCDP_DIR)
from models.isp_simulator.synth_raw import rgb_to_sensor_raw, SENSOR_W, SENSOR_H
from models.isp_simulator.fidelity_gate import load_nv21_rgb

SEED = 20260705
EVAL_W, EVAL_H = 512, 288


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    return 10.0 * np.log10(1.0 / max(float(((a - b) ** 2).mean()), 1e-10))


# ── 选图 + 候选池 ─────────────────────────────────────────────


def _select_images(num: int, rng: np.random.Generator) -> list[str]:
    """LCDP train 按亮度桶选图（含极暗/暗/中/亮，覆盖曝光谱）。"""
    from PIL import Image
    names = sorted(p.name for p in (LCDP_DIR / "input").glob("*.png"))
    lum = np.array([
        np.asarray(Image.open(LCDP_DIR / "input" / n).convert("L").resize((48, 48)),
                   np.float32).mean() / 255.0
        for n in names])
    # 桶: 极暗/暗/中/亮/较亮，各取 num/5
    buckets = [(0.0, 0.06), (0.06, 0.15), (0.15, 0.35), (0.35, 0.55), (0.55, 0.9)]
    per = max(1, num // len(buckets))
    sel = []
    for lo, hi in buckets:
        idx = np.where((lum >= lo) & (lum < hi))[0]
        if len(idx):
            sel += [names[j] for j in rng.choice(idx, min(per, len(idx)), replace=False)]
    return sel[:num]


def cmd_gen(args) -> int:
    from PIL import Image
    out = Path(args.outdir)
    (out / "pool").mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(SEED)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    names = _select_images(args.num, rng)
    print(f"选图 {len(names)} 张")

    # ParamNet（warmstart 保底：θ* 至少不差于当前网络）
    net = ParamNet().to(device)
    net.load_state_dict(torch.load(args.paramnet, map_location=device)["state_dict"])
    net.eval()

    warm_u = []
    meta = []
    for fi, n in enumerate(names):
        im = Image.open(LCDP_DIR / "input" / n).convert("RGB")
        g = Image.open(LCDP_DIR / "gt" / n).convert("RGB")
        W, H = im.size
        cw, chh = (int(H * 16 / 9), H) if W / H > 16 / 9 else (W, int(W * 9 / 16))
        box = ((W - cw) // 2, (H - chh) // 2, (W + cw) // 2, (H + chh) // 2)
        im_c, g_c = im.crop(box), g.crop(box)
        im_c.resize((EVAL_W, EVAL_H), Image.BILINEAR).save(out / f"img{fi:02d}_input.png")
        g_c.resize((EVAL_W, EVAL_H), Image.BILINEAR).save(out / f"img{fi:02d}_gt.png")
        rgb = np.asarray(im_c.resize((SENSOR_W, SENSOR_H), Image.BILINEAR), np.float32) / 255.0
        (out / f"img{fi:02d}.raw").write_bytes(rgb_to_sensor_raw(rgb))
        # ParamNet warmstart u
        xn = torch.from_numpy(
            np.asarray(im_c.resize((NET_W, NET_H), Image.BILINEAR), np.float32) / 255.0
        ).permute(2, 0, 1).unsqueeze(0).to(device)
        with torch.no_grad():
            warm_u.append(net(xn)[0].cpu())
        meta.append({"idx": fi, "src": n})

    # 候选池 = M 个 LHS(对称+γ) + N 个 warmstart
    u_lhs = torch.from_numpy(lhs(args.cands, U_DIM, rng)).float()
    pool_u = torch.cat([u_lhs, torch.stack(warm_u)], dim=0)  # (M+N, 30)
    for k in range(pool_u.shape[0]):
        theta = u_to_theta(pool_u[k:k + 1])
        (out / "pool" / f"z{k:03d}.bin").write_bytes(
            sim_params_to_blob(theta, gamma_on=True, gamma_strength=1.0))

    torch.save({"pool_u": pool_u, "n_lhs": args.cands, "names": names}, out / "pool.pt")
    (out / "meta.json").write_text(json.dumps(meta, indent=2, ensure_ascii=False))
    print(f"候选池 {pool_u.shape[0]}（{args.cands} LHS + {len(names)} warmstart）-> {out}/pool")
    print(f"板端: stop stream; ./test_raw_replay --settle 8 --out 512x288 --exptime 8000 "
          f"--again 1024 --outdir out " +
          " ".join(f"--raw-file img{i:02d}.raw" for i in range(len(names))) +
          " --blob-dir <板端>/pool")
    return 0


# ── θ* 标签 ───────────────────────────────────────────────────


def cmd_labels(args) -> int:
    from PIL import Image
    d = Path(args.distill_dir)
    board = Path(args.board_dir)
    pool = torch.load(d / "pool.pt")
    pool_u = pool["pool_u"]
    n_pool = pool_u.shape[0]
    n_img = len(pool["names"])

    labels_u, rows = [], []
    for fi in range(n_img):
        gt = np.asarray(Image.open(d / f"img{fi:02d}_gt.png"), np.float32) / 255.0
        inp = np.asarray(Image.open(d / f"img{fi:02d}_input.png"), np.float32) / 255.0
        scores = np.full(n_pool, -1.0)
        for k in range(n_pool):
            p = board / f"out_f{fi:02d}_{k + 1:02d}_blob_z{k:03d}.bin.nv21"
            if p.exists():
                scores[k] = psnr(load_nv21_rgb(p, EVAL_W, EVAL_H), gt)
        best = int(np.argmax(scores))
        # warmstart 该图的候选序号 = n_lhs + fi（即当前 ParamNet 的 θ）
        pn_k = pool["n_lhs"] + fi
        labels_u.append(pool_u[best])
        rows.append({"idx": fi, "best_k": best, "theta_star": float(scores[best]),
                     "paramnet": float(scores[pn_k]), "input": psnr(inp, gt)})
        print(f"img{fi:02d}: θ*={scores[best]:.2f}(k{best}) "
              f"ParamNet={scores[pn_k]:.2f} input={psnr(inp, gt):.2f} "
              f"gain=+{scores[best] - scores[pn_k]:.2f}")

    torch.save({"labels_u": torch.stack(labels_u), "names": pool["names"]}, d / "labels.pt")
    ts = np.array([r["theta_star"] for r in rows])
    pn = np.array([r["paramnet"] for r in rows])
    print(f"\nθ* 中位 {np.median(ts):.2f} vs ParamNet 中位 {np.median(pn):.2f} "
          f"(平均增益 +{(ts - pn).mean():.2f} dB)")
    (d / "labels_report.json").write_text(json.dumps(rows, indent=2))
    return 0


# ── 回归微调 ──────────────────────────────────────────────────


def cmd_finetune(args) -> int:
    from PIL import Image
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    d = Path(args.distill_dir)
    lab = torch.load(d / "labels.pt")
    labels_u = lab["labels_u"].to(device)
    n_img = labels_u.shape[0]

    # 蒸馏图的 ParamNet 输入（sRGB @ NET）
    xs = []
    for fi in range(n_img):
        im = Image.open(d / f"img{fi:02d}_input.png").convert("RGB").resize((NET_W, NET_H))
        xs.append(torch.from_numpy(np.asarray(im, np.float32) / 255.0).permute(2, 0, 1))
    xs = torch.stack(xs).to(device)

    net = ParamNet().to(device)
    ck = torch.load(args.paramnet, map_location=device)
    net.load_state_dict(ck["state_dict"])
    opt = torch.optim.AdamW(net.parameters(), lr=args.lr)

    # 排练：LCDP train 代理目标保泛化（防 15 标签过拟合），+ 硬件 θ* 回归拉修正
    from models.isp_simulator.paramnet import CalibratedProxy
    tr = torch.load(Path(args.paramnet_cache) / "lcdp_train.pt")
    proxy = CalibratedProxy(Path(args.resnet), device)
    n_tr = tr["x_net"].shape[0]

    for ep in range(args.epochs):
        net.train()
        order = np.random.default_rng(ep).permutation(n_tr)
        tot_p, tot_d, nb = 0.0, 0.0, 0
        for s in range(0, n_tr, args.batch):
            idx = order[s:s + args.batch]
            xn = tr["x_net"][idx].to(device).float() / 255.0
            xp = tr["x_pxy"][idx].to(device).float() / 255.0
            gt = tr["gt"][idx].to(device).float() / 255.0
            loss_p = F.l1_loss(proxy(xp, u_to_theta(net(xn))), gt)   # 排练（保泛化）
            loss_d = F.mse_loss(net(xs), labels_u)                   # 硬件 θ* 蒸馏
            loss = loss_p + args.lam_d * loss_d
            opt.zero_grad()
            loss.backward()
            opt.step()
            tot_p += loss_p.item(); tot_d += loss_d.item(); nb += 1
        if (ep + 1) % 2 == 0 or ep == args.epochs - 1:
            print(f"ep {ep + 1:3d}: proxy_l1={tot_p / nb:.4f} distill_mse={tot_d / nb:.5f}")

    torch.save({"state_dict": net.state_dict(), "src": "distill_rehearse",
                "base": str(args.paramnet)}, d / "paramnet_distill.pt")
    print(f"蒸馏微调（排练）-> {d}/paramnet_distill.pt")

    # LCDP valid 不回归检查（代理口径；代理仅作参考，真验收在板端）
    if args.check_valid:
        from models.isp_simulator.paramnet import CalibratedProxy
        va = torch.load(Path(args.paramnet_cache) / "lcdp_valid.pt")
        proxy = CalibratedProxy(Path(args.resnet), device)

        @torch.no_grad()
        def valpsnr(model):
            vals = []
            for s in range(0, va["x_net"].shape[0], 8):
                xn = va["x_net"][s:s + 8].to(device).float() / 255.0
                xp = va["x_pxy"][s:s + 8].to(device).float() / 255.0
                gt = va["gt"][s:s + 8].to(device).float() / 255.0
                out = proxy(xp, u_to_theta(model(xn)))
                mse = ((out - gt) ** 2).flatten(1).mean(dim=1)
                vals += (10 * torch.log10(1 / mse.clamp(min=1e-10))).cpu().tolist()
            return float(np.median(vals))

        base = ParamNet().to(device)
        base.load_state_dict(ck["state_dict"])
        base.eval(); net.eval()
        print(f"LCDP valid 代理口径: 微调前 {valpsnr(base):.2f} → 微调后 {valpsnr(net):.2f} dB")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("gen")
    g.add_argument("--outdir", default="models/weights/distill")
    g.add_argument("--num", type=int, default=16)
    g.add_argument("--cands", type=int, default=128)
    g.add_argument("--paramnet", default="models/weights/paramnet/paramnet.pt")

    lb = sub.add_parser("labels")
    lb.add_argument("--distill-dir", default="models/weights/distill")
    lb.add_argument("--board-dir", required=True)

    ft = sub.add_parser("finetune")
    ft.add_argument("--distill-dir", default="models/weights/distill")
    ft.add_argument("--paramnet", default="models/weights/paramnet/paramnet.pt")
    ft.add_argument("--epochs", type=int, default=16)
    ft.add_argument("--lr", type=float, default=2e-4)
    ft.add_argument("--lam-d", type=float, default=1.0, help="硬件 θ* 蒸馏项权重")
    ft.add_argument("--batch", type=int, default=8)
    ft.add_argument("--check-valid", action="store_true")
    ft.add_argument("--paramnet-cache", default="models/weights/paramnet")
    ft.add_argument("--resnet", default="models/weights/calib_v2/residual_net.pt")

    args = ap.parse_args()
    return {"gen": cmd_gen, "labels": cmd_labels, "finetune": cmd_finetune}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
