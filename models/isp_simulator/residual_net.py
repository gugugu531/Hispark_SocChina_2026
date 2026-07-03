#!/usr/bin/env python3
"""残差校准网络 R(sim输出, θ) —— 把解析模拟器校准到 SS928 硬件 ISP。

背景（docs/isp-param-tuning-research.md §5.3）：模拟器排序保真已坐实，但绝对残差
PSNR 中位仅 16 dB 且非低维全局项；残差主要由 θ 决定 → 参数条件化为主轴。

设计:
    - FiLM 条件化小 CNN：θ(31 维核心参数) → MLP → 逐块 scale/shift 调制;
      骨干 4 × [conv3x3 + GN + FiLM + ReLU]，通道 32，~60K 参数。
    - 输出头 zero-init → 初始即恒等（输出=sim），训练稳定。
    - 校正输出 = clamp(sim + Δ)。

用法:
    # 训练（数据 = calib_dataset 采集的板端帧；sim 输出即时预计算缓存）
    python -m models.isp_simulator.residual_net train \
        --calib-dir models/weights/calib --board-dir models/weights/calib/board

    # 验收：留出参数组校正前后 PSNR（判据中位 >25 dB）
    python -m models.isp_simulator.residual_net eval \
        --calib-dir models/weights/calib --board-dir models/weights/calib/board

θ 取参数向量 [65:96]（tone6+mix12+ctrl3+blend1+strength1+ldci8 = 31 连续维）。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

from models.isp_simulator import ISPPipeline

THETA_SLICE = slice(65, 96)  # 31 维核心参数（board blob 覆盖范围）
SPLIT_SEED = 20260705
VAL_FRAC = 0.2


class FiLMBlock(nn.Module):
    def __init__(self, ch: int, emb: int):
        super().__init__()
        self.conv = nn.Conv2d(ch, ch, 3, padding=1)
        self.norm = nn.GroupNorm(4, ch)
        self.film = nn.Linear(emb, ch * 2)

    def forward(self, x: torch.Tensor, e: torch.Tensor) -> torch.Tensor:
        h = self.norm(self.conv(x))
        scale, shift = self.film(e).chunk(2, dim=-1)
        h = h * (1.0 + scale[:, :, None, None]) + shift[:, :, None, None]
        return F.relu(h)


class ResidualNet(nn.Module):
    """R(sim, θ) → 校正输出。zero-init 头保证初始恒等。"""

    def __init__(self, theta_dim: int = 31, ch: int = 32, emb: int = 128, blocks: int = 4):
        super().__init__()
        self.theta_mlp = nn.Sequential(
            nn.Linear(theta_dim, emb), nn.ReLU(), nn.Linear(emb, emb), nn.ReLU())
        self.stem = nn.Conv2d(3, ch, 3, padding=1)
        self.blocks = nn.ModuleList([FiLMBlock(ch, emb) for _ in range(blocks)])
        self.head = nn.Conv2d(ch, 3, 3, padding=1)
        nn.init.zeros_(self.head.weight)
        nn.init.zeros_(self.head.bias)

    def forward(self, sim: torch.Tensor, theta: torch.Tensor) -> torch.Tensor:
        e = self.theta_mlp(theta)
        h = F.relu(self.stem(sim))
        for blk in self.blocks:
            h = blk(h, e)
        return (sim + self.head(h)).clamp(0.0, 1.0)


# ── 数据 ──────────────────────────────────────────────────────


def load_dataset(calib_dir: Path, board_dir: Path, scenes: int, w: int, h: int,
                 device: torch.device):
    """载入 (sim, hw, θ) 三元组。sim 输出用管线在 device 上预计算，uint8 缓存省内存。"""
    from models.isp_simulator.fidelity_gate import load_nv21_rgb

    params = torch.load(calib_dir / "params.pt")
    num = params.shape[0]
    pipeline = ISPPipeline()

    sim_u8 = torch.empty(scenes, num, 3, h, w, dtype=torch.uint8)
    hw_u8 = torch.empty(scenes, num, 3, h, w, dtype=torch.uint8)
    for fi in range(scenes):
        neutral = load_nv21_rgb(board_dir / f"out_f{fi:02d}_01_blob_c000_neutral.bin.nv21", w, h)
        x = torch.from_numpy(neutral).float().permute(2, 0, 1).unsqueeze(0).to(device)
        for i in range(num):
            hw = load_nv21_rgb(board_dir / f"out_f{fi:02d}_{i + 2:02d}_blob_c{i + 1:03d}.bin.nv21",
                               w, h)
            hw_u8[fi, i] = torch.from_numpy((hw * 255.0).astype(np.uint8)).permute(2, 0, 1)
            with torch.no_grad():
                sim = pipeline(x, params[i:i + 1].to(device), enable_wdr=False,
                               enable_gamma=False, enable_dehaze=False)["output"]
            sim_u8[fi, i] = (sim.squeeze(0).clamp(0, 1) * 255.0).to(torch.uint8).cpu()
        print(f"  场景 f{fi:02d} 预计算完成")

    theta = params[:, THETA_SLICE].clone()
    return sim_u8, hw_u8, theta


def param_split(num: int):
    rng = np.random.default_rng(SPLIT_SEED)
    idx = rng.permutation(num)
    n_val = int(num * VAL_FRAC)
    return np.sort(idx[n_val:]), np.sort(idx[:n_val])  # train, val


def psnr(a: torch.Tensor, b: torch.Tensor) -> torch.Tensor:
    mse = ((a - b) ** 2).flatten(1).mean(dim=1)
    return 10.0 * torch.log10(1.0 / mse.clamp(min=1e-10))


# ── 训练 / 评估 ───────────────────────────────────────────────


def cmd_train(args) -> int:
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    calib_dir, board_dir = Path(args.calib_dir), Path(args.board_dir)
    print(f"device={device}")

    sim_u8, hw_u8, theta = load_dataset(calib_dir, board_dir, args.scenes,
                                        args.width, args.height, device)
    scenes, num = sim_u8.shape[0], sim_u8.shape[1]
    tr_idx, va_idx = param_split(num)
    print(f"数据: {scenes} 场景 × {num} 参数; train={len(tr_idx)} val={len(va_idx)} (参数组划分)")

    model = ResidualNet().to(device)
    n_par = sum(p.numel() for p in model.parameters())
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=args.epochs)
    print(f"ResidualNet 参数量: {n_par / 1e3:.1f}K")

    pairs = [(fi, pi) for fi in range(scenes) for pi in tr_idx]
    best_val = -1.0
    ckpt = calib_dir / "residual_net.pt"
    for ep in range(args.epochs):
        model.train()
        rng = np.random.default_rng(ep)
        order = rng.permutation(len(pairs))
        tot, nb = 0.0, 0
        for s in range(0, len(order), args.batch):
            sel = [pairs[j] for j in order[s:s + args.batch]]
            sim = torch.stack([sim_u8[fi, pi] for fi, pi in sel]).to(device).float() / 255.0
            hw = torch.stack([hw_u8[fi, pi] for fi, pi in sel]).to(device).float() / 255.0
            th = torch.stack([theta[pi] for _, pi in sel]).to(device)
            out = model(sim, th)
            loss = F.l1_loss(out, hw)
            opt.zero_grad()
            loss.backward()
            opt.step()
            tot += loss.item()
            nb += 1
        sched.step()

        if (ep + 1) % args.eval_every == 0 or ep == args.epochs - 1:
            va = evaluate(model, sim_u8, hw_u8, theta, va_idx, device, args.batch)
            print(f"ep {ep + 1:3d}: train_l1={tot / nb:.4f}  val_psnr_med={va:.2f} dB")
            if va > best_val:
                best_val = va
                torch.save({"state_dict": model.state_dict(), "val_psnr_med": va,
                            "theta_slice": [65, 96]}, ckpt)
    print(f"best val PSNR 中位 = {best_val:.2f} dB  -> {ckpt}")
    return 0


@torch.no_grad()
def evaluate(model, sim_u8, hw_u8, theta, va_idx, device, batch) -> float:
    model.eval()
    scenes = sim_u8.shape[0]
    vals = []
    pairs = [(fi, pi) for fi in range(scenes) for pi in va_idx]
    for s in range(0, len(pairs), batch):
        sel = pairs[s:s + batch]
        sim = torch.stack([sim_u8[fi, pi] for fi, pi in sel]).to(device).float() / 255.0
        hw = torch.stack([hw_u8[fi, pi] for fi, pi in sel]).to(device).float() / 255.0
        th = torch.stack([theta[pi] for _, pi in sel]).to(device)
        out = model(sim, th)
        vals.extend(psnr(out, hw).cpu().tolist())
    return float(np.median(vals))


def cmd_eval(args) -> int:
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    calib_dir, board_dir = Path(args.calib_dir), Path(args.board_dir)
    ck = torch.load(calib_dir / "residual_net.pt", map_location=device)
    model = ResidualNet().to(device)
    model.load_state_dict(ck["state_dict"])
    model.eval()

    sim_u8, hw_u8, theta = load_dataset(calib_dir, board_dir, args.scenes,
                                        args.width, args.height, device)
    _, va_idx = param_split(sim_u8.shape[1])

    raw_vals, cor_vals = [], []
    with torch.no_grad():
        for fi in range(sim_u8.shape[0]):
            for pi in va_idx:
                sim = sim_u8[fi, pi].unsqueeze(0).to(device).float() / 255.0
                hw = hw_u8[fi, pi].unsqueeze(0).to(device).float() / 255.0
                th = theta[pi].unsqueeze(0).to(device)
                raw_vals.append(psnr(sim, hw).item())
                cor_vals.append(psnr(model(sim, th), hw).item())
    raw, cor = np.array(raw_vals), np.array(cor_vals)
    print(f"留出参数组 (n={len(raw)}):")
    print(f"  未校正: PSNR 中位 {np.median(raw):.2f}  p5 {np.percentile(raw, 5):.2f}")
    print(f"  校正后: PSNR 中位 {np.median(cor):.2f}  p5 {np.percentile(cor, 5):.2f}")
    gate = np.median(cor) > 25.0
    print(f"验收（校正后中位 >25 dB）: {'PASS' if gate else 'FAIL'}")
    out = Path(args.calib_dir) / "residual_eval.json"
    out.write_text(json.dumps({"raw_med": float(np.median(raw)),
                               "cor_med": float(np.median(cor)),
                               "cor_p5": float(np.percentile(cor, 5)),
                               "n": len(raw), "pass": bool(gate)}, indent=2))
    return 0 if gate else 2


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("train", "eval"):
        p = sub.add_parser(name)
        p.add_argument("--calib-dir", default="models/weights/calib")
        p.add_argument("--board-dir", default="models/weights/calib/board")
        p.add_argument("--scenes", type=int, default=8)
        p.add_argument("--width", type=int, default=512)
        p.add_argument("--height", type=int, default=288)
        p.add_argument("--batch", type=int, default=16)
        if name == "train":
            p.add_argument("--epochs", type=int, default=120)
            p.add_argument("--lr", type=float, default=1e-3)
            p.add_argument("--eval-every", type=int, default=10)
    args = ap.parse_args()
    return cmd_train(args) if args.cmd == "train" else cmd_eval(args)


if __name__ == "__main__":
    sys.exit(main())
