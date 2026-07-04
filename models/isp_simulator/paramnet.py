#!/usr/bin/env python3
"""ParamNet 阶段 2 预训练——场景图像 → ISP 参数，训练环境 = 校准代理(sim+R)。

设计（docs/isp-param-tuning-research.md §4.4 阶段 2 / §5.5）:
    - 输入 256×144 RGB 下采样图；backbone 全卷积 stride-2 ×5 + AvgPool 固定核
      （NPU 红名单安全：无 Pow/Cast/ReduceMean），~350K 参数。
    - 输出 29 维 u∈[0,1]（sigmoid），经与 calib_dataset LHS **完全相同的可微映射**
      转成 97 维 θ——保证输出天然落在校准代理的有效域内（域一致性）。
    - 训练：LCDP 双向曝光校正数据集（input/gt 1415 对）；冻结校准代理
      sim(512×288)+ResidualNet；loss = L1(代理输出, GT)。
    - 已知妥协：drc_ctrl 3 维在模拟器内以 .item() 标量化（滤波半径本质离散），
      sim 主通路梯度断裂；但 ResidualNet 的 θ 输入含 ctrl（FiLM 可微），硬件对
      ctrl 的效应在校准时已作为残差被 R 吸收 → ParamNet 经 ∂R/∂θ_ctrl 旁路获得
      不完整但非零的学习信号。ctrl 属细调参数（halo/细节/对比微调），最坏情形
      仅细调次优，不影响曝光校正主功能；阶段 3 蒸馏（硬件标签，无梯度依赖）纠偏。

用法:
    python -m models.isp_simulator.paramnet prepare   # LCDP → resize 缓存(一次)
    python -m models.isp_simulator.paramnet train
    python -m models.isp_simulator.paramnet eval
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

from models.isp_simulator import ISPPipeline, make_identity_params
from models.isp_simulator.params import get_offset
from models.isp_simulator.residual_net import ResidualNet

LCDP_DIR = Path("artifacts/datasets/cotf/lcdp/extracted")  # 相对仓库根（python -m 运行）
PROXY_W, PROXY_H = 512, 288   # 校准代理训练分辨率
NET_W, NET_H = 256, 144       # ParamNet 输入分辨率
U_DIM = 30  # v2: +1 维 Gamma γ


def u_to_theta(u: torch.Tensor) -> torch.Tensor:
    """(B, 29) u∈[0,1] → (B, 97) 参数向量。与 calib_dataset.sample_params 逐段同映射
    （可微：clamp/cummax 均有梯度）。"""
    B = u.shape[0]
    params = make_identity_params(B).to(u.device)

    off_t = get_offset("drc_tone")
    base = torch.linspace(0.0, 1.0, 6, device=u.device).expand(B, 6).clone()
    delta = u[:, 0:4] * 0.60 - 0.30  # v2: 对称 ±0.30（与 calib v2 同域，可压暗）
    mid = (base[:, 1:5] + delta).clamp(0.0, 1.0)
    cp = torch.cat([base[:, :1], mid, base[:, 5:]], dim=1)
    cp = torch.cummax(cp, dim=1).values
    params[:, off_t:off_t + 6] = cp

    params[:, get_offset("drc_strength")] = u[:, 4]

    off_l = get_offset("ldci")
    lo = torch.tensor([0.0, 0.2, 0.1, 0.0, 0.2, 0.4, 0.0, 0.2], device=u.device)
    sp = torch.tensor([0.90, 0.6, 0.5, 0.60, 0.6, 0.5, 0.5, 0.6], device=u.device)
    params[:, off_l:off_l + 8] = lo + u[:, 5:13] * sp

    off_m = get_offset("drc_mix")
    params[:, off_m:off_m + 12] = 0.2 + u[:, 13:25] * 0.7
    off_c = get_offset("drc_ctrl")
    params[:, off_c:off_c + 3] = 0.2 + u[:, 25:28] * 0.6
    params[:, get_offset("drc_blend")] = 0.2 + u[:, 28] * 0.6

    # v2: Gamma γ ∈ [0.45, 1.55] 幂曲线（与 calib v2 采样同映射；γ<1 提亮）
    off_g = get_offset("gamma")
    gam = (0.45 + u[:, 29] * 1.10).view(-1, 1)
    nodes = torch.linspace(0.0, 1.0, 64, device=u.device).view(1, -1)
    params[:, off_g:off_g + 64] = nodes.clamp(min=1e-6).pow(gam)
    return params


class ParamNet(nn.Module):
    """256×144 RGB → 29 维 u。全卷积 + 固定核 AvgPool，NPU 红名单安全。"""

    def __init__(self, u_dim: int = U_DIM):
        super().__init__()
        chs = [3, 24, 48, 96, 128, 160]
        layers = []
        for i in range(5):
            layers += [nn.Conv2d(chs[i], chs[i + 1], 3, stride=2, padding=1),
                       nn.ReLU(inplace=True)]
        self.features = nn.Sequential(*layers)          # 144x256 → 5x8
        self.pool = nn.AvgPool2d((5, 8))                # 固定核，非 ReduceMean
        self.head = nn.Conv2d(160, u_dim, 1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        h = self.pool(self.features(x))
        return torch.sigmoid(self.head(h)).flatten(1)


# ── 数据 ──────────────────────────────────────────────────────


def _load_split(split_in: Path, split_gt: Path, limit: int | None = None):
    from PIL import Image
    names = sorted(p.name for p in split_in.glob("*.png"))
    if limit:
        names = names[:limit]
    n = len(names)
    x_net = torch.empty(n, 3, NET_H, NET_W, dtype=torch.uint8)
    x_pxy = torch.empty(n, 3, PROXY_H, PROXY_W, dtype=torch.uint8)
    gt = torch.empty(n, 3, PROXY_H, PROXY_W, dtype=torch.uint8)
    for i, name in enumerate(names):
        im = Image.open(split_in / name).convert("RGB")
        g = Image.open(split_gt / name).convert("RGB")
        x_pxy[i] = torch.from_numpy(
            np.asarray(im.resize((PROXY_W, PROXY_H), Image.BILINEAR))).permute(2, 0, 1)
        x_net[i] = torch.from_numpy(
            np.asarray(im.resize((NET_W, NET_H), Image.BILINEAR))).permute(2, 0, 1)
        gt[i] = torch.from_numpy(
            np.asarray(g.resize((PROXY_W, PROXY_H), Image.BILINEAR))).permute(2, 0, 1)
        if (i + 1) % 300 == 0:
            print(f"  {i + 1}/{n}")
    return {"x_net": x_net, "x_pxy": x_pxy, "gt": gt, "names": names}


def cmd_prepare(args) -> int:
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)
    print("train split:")
    torch.save(_load_split(LCDP_DIR / "input", LCDP_DIR / "gt"), out / "lcdp_train.pt")
    print("valid split:")
    torch.save(_load_split(LCDP_DIR / "valid-input", LCDP_DIR / "valid-gt"),
               out / "lcdp_valid.pt")
    print(f"缓存 -> {out}")
    return 0


# ── 代理前向 ──────────────────────────────────────────────────


class CalibratedProxy(nn.Module):
    """冻结的校准代理: sim(DRC+LDCI) + ResidualNet。"""

    def __init__(self, resnet_ckpt: Path, device):
        super().__init__()
        self.pipeline = ISPPipeline()
        ck = torch.load(resnet_ckpt, map_location=device)
        ts = ck.get("theta_slice", [65, 96])
        self.theta_slice = slice(ts[0], ts[1])
        self.gamma_in_theta = ts[0] <= 1  # v2 条件含 gamma → sim 前向也开 gamma
        self.resnet = ResidualNet(theta_dim=ts[1] - ts[0], ch=ck.get("ch", 32),
                                  blocks=ck.get("blocks", 4)).to(device)
        self.resnet.load_state_dict(ck["state_dict"])
        self.resnet.eval()
        for p in self.resnet.parameters():
            p.requires_grad_(False)

    def forward(self, x: torch.Tensor, theta: torch.Tensor) -> torch.Tensor:
        sim = self.pipeline(x, theta, enable_wdr=False,
                            enable_gamma=self.gamma_in_theta,
                            enable_dehaze=False)["output"]
        return self.resnet(sim, theta[:, self.theta_slice])


def psnr(a: torch.Tensor, b: torch.Tensor) -> torch.Tensor:
    mse = ((a - b) ** 2).flatten(1).mean(dim=1)
    return 10.0 * torch.log10(1.0 / mse.clamp(min=1e-10))


# ── 训练 / 评估 ───────────────────────────────────────────────


def cmd_train(args) -> int:
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    out = Path(args.outdir)
    tr = torch.load(out / "lcdp_train.pt")
    va = torch.load(out / "lcdp_valid.pt")
    n = tr["x_net"].shape[0]
    print(f"device={device}  train={n} valid={va['x_net'].shape[0]}")

    proxy = CalibratedProxy(Path(args.resnet), device)
    model = ParamNet().to(device)
    n_par = sum(p.numel() for p in model.parameters())
    print(f"ParamNet 参数量: {n_par / 1e3:.1f}K")
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=args.epochs)

    best = -1.0
    ckpt = out / "paramnet.pt"
    for ep in range(args.epochs):
        model.train()
        order = np.random.default_rng(ep).permutation(n)
        tot, nb = 0.0, 0
        for s in range(0, n, args.batch):
            idx = order[s:s + args.batch]
            xn = tr["x_net"][idx].to(device).float() / 255.0
            xp = tr["x_pxy"][idx].to(device).float() / 255.0
            gt = tr["gt"][idx].to(device).float() / 255.0
            theta = u_to_theta(model(xn))
            out_img = proxy(xp, theta)
            loss = F.l1_loss(out_img, gt)
            opt.zero_grad()
            loss.backward()
            opt.step()
            tot += loss.item()
            nb += 1
        sched.step()

        if (ep + 1) % args.eval_every == 0 or ep == args.epochs - 1:
            v = eval_split(model, proxy, va, device, args.batch)
            print(f"ep {ep + 1:3d}: train_l1={tot / nb:.4f}  val_psnr={v:.2f} dB")
            if v > best:
                best = v
                torch.save({"state_dict": model.state_dict(), "val_psnr": v}, ckpt)
    print(f"best val PSNR = {best:.2f} dB -> {ckpt}")
    return 0


@torch.no_grad()
def eval_split(model, proxy, data, device, batch) -> float:
    model.eval()
    n = data["x_net"].shape[0]
    vals = []
    for s in range(0, n, batch):
        xn = data["x_net"][s:s + batch].to(device).float() / 255.0
        xp = data["x_pxy"][s:s + batch].to(device).float() / 255.0
        gt = data["gt"][s:s + batch].to(device).float() / 255.0
        out_img = proxy(xp, u_to_theta(model(xn)))
        vals.extend(psnr(out_img, gt).cpu().tolist())
    return float(np.median(vals))


def cmd_eval(args) -> int:
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    out = Path(args.outdir)
    va = torch.load(out / "lcdp_valid.pt")
    proxy = CalibratedProxy(Path(args.resnet), device)
    model = ParamNet().to(device)
    ck = torch.load(out / "paramnet.pt", map_location=device)
    model.load_state_dict(ck["state_dict"])
    model.eval()

    n = va["x_net"].shape[0]
    base_vals, out_vals = [], []
    with torch.no_grad():
        for s in range(0, n, args.batch):
            xp = va["x_pxy"][s:s + args.batch].to(device).float() / 255.0
            xn = va["x_net"][s:s + args.batch].to(device).float() / 255.0
            gt = va["gt"][s:s + args.batch].to(device).float() / 255.0
            base_vals.extend(psnr(xp, gt).cpu().tolist())
            out_img = proxy(xp, u_to_theta(model(xn)))
            out_vals.extend(psnr(out_img, gt).cpu().tolist())
    base, outv = np.array(base_vals), np.array(out_vals)
    print(f"LCDP valid (n={n}):")
    print(f"  不处理 baseline: PSNR 中位 {np.median(base):.2f}  p5 {np.percentile(base, 5):.2f}")
    print(f"  ParamNet+代理:   PSNR 中位 {np.median(outv):.2f}  p5 {np.percentile(outv, 5):.2f}")
    (out / "paramnet_eval.json").write_text(json.dumps(
        {"baseline_med": float(np.median(base)), "paramnet_med": float(np.median(outv)),
         "n": int(n)}, indent=2))
    return 0


def cmd_infer(args) -> int:
    """板端 A/B 用：中性帧 NV21 → ParamNet → θ → blob 文件。"""
    from PIL import Image
    from models.isp_simulator.fidelity_gate import load_nv21_rgb
    from models.isp_simulator.isp_blob import sim_params_to_blob

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model = ParamNet().to(device)
    ck = torch.load(Path(args.outdir) / "paramnet.pt", map_location=device)
    model.load_state_dict(ck["state_dict"])
    model.eval()

    rgb = load_nv21_rgb(Path(args.frame), args.frame_width, args.frame_height)
    im = Image.fromarray((rgb * 255).astype(np.uint8)).resize((NET_W, NET_H), Image.BILINEAR)
    x = torch.from_numpy(np.asarray(im)).float().permute(2, 0, 1).unsqueeze(0).to(device) / 255.0
    with torch.no_grad():
        u = model(x)
        theta = u_to_theta(u)
    # v2 θ 含 Gamma 曲线，blob 必须携带 gamma 段（strength=1.0 完全施加）
    Path(args.out).write_bytes(sim_params_to_blob(theta.cpu(), gamma_on=True,
                                                  gamma_strength=1.0))

    t = theta[0].cpu()
    off_t, off_l = get_offset("drc_tone"), get_offset("ldci")
    print(f"θ 摘要: tone={t[off_t:off_t + 6].numpy().round(3)} "
          f"strength={t[get_offset('drc_strength')].item():.3f} "
          f"ldci_pos_wgt={t[off_l].item():.3f} blob -> {args.out}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("prepare", "train", "eval", "infer"):
        p = sub.add_parser(name)
        p.add_argument("--outdir", default="models/weights/paramnet")
        p.add_argument("--resnet", default="models/weights/calib/residual_net.pt")
        p.add_argument("--batch", type=int, default=8)
        if name == "train":
            p.add_argument("--epochs", type=int, default=24)
            p.add_argument("--lr", type=float, default=3e-4)
            p.add_argument("--eval-every", type=int, default=2)
        if name == "infer":
            p.add_argument("--frame", required=True, help="中性帧 NV21")
            p.add_argument("--frame-width", type=int, default=1024)
            p.add_argument("--frame-height", type=int, default=576)
            p.add_argument("--out", default="models/weights/paramnet/paramnet_theta.bin")
    args = ap.parse_args()
    return {"prepare": cmd_prepare, "train": cmd_train, "eval": cmd_eval,
            "infer": cmd_infer}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
