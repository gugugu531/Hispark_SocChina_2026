"""CTBG v8 拆分导出:estimator(低分辨率出 18ch raw 系数) + apply(全分辨率施加)。

拆分动机(规避红名单 Resize/Pow):
- 整模型 forward 含 Resize×3(输入降采样 + dark/bright 系数上采样)与 Pow×1(gamma 施加)。
- **estimator**:喂*已降采样*的 256×144 输入(板端由 VPSS chn2 给,主机用 interpolate),
  跑 backbone+FiLM+tail,出 18ch raw 系数(256×144)。无 Resize、无 Pow。
- **apply**:全分辨率 in(1024×576) + 18ch raw 系数(256×144)→ ConvTranspose ×4 上采样(替 Resize,
  红名单安全)→ decode(tanh/exp)→ 按像素 luma 插值 dark/bright → clamp(a·in^g+b)。Pow 隔离在此。

对拍:整模型 forward(用 F.interpolate 上采样)vs estimator→apply(用 ConvTranspose 上采样),
差异主要来自上采样核近似(与 P1 routeB 同源,已接受)。
"""
import sys, argparse
from pathlib import Path
import numpy as np, torch, torch.nn as nn, torch.nn.functional as F

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import litmodule_coeff as M
REPO = Path(__file__).resolve().parents[1] / "repos/MobileIE"
sys.path.insert(0, str(REPO))
from model.utils import MBRConv1, MBRConv3, MBRConv5, FST, FSTS  # noqa: E402

RED = {'Resize', 'Upsample', 'GridSample', 'Cast', 'LayerNormalization',
       'Erf', 'Gelu', 'InstanceNormalization', 'Pow'}


class CTBGSlimEstimator(nn.Module):
    """CTBG backbone 的 reparam slim 版:多分支 MBRConv→单 conv,tail 18ch,残差式 + FiLM。
    forward 严格镜像 CoeffNetCTBG._backbone(residual_spatial=True)。"""
    def __init__(self, channels=12):
        super().__init__()
        self.head = FSTS(nn.Sequential(
            nn.Conv2d(3, channels, 5, 1, 2), nn.PReLU(channels),
            nn.Conv2d(channels, channels, 3, 1, 1)), channels)
        self.body = FSTS(nn.Conv2d(channels, channels, 3, 1, 1), channels)
        self.att = nn.Sequential(nn.AdaptiveAvgPool2d(1), nn.Conv2d(channels, channels, 1), nn.Sigmoid())
        self.att1 = nn.Sequential(nn.Conv2d(1, channels, 1, 1), nn.Sigmoid())
        self.tail = nn.Conv2d(channels, 18, 3, 1, 1)
        self.film_s = nn.Linear(1, channels, bias=False)
        self.film_b = nn.Linear(1, channels, bias=False)

    def forward(self, xl):
        x0 = self.head(xl); x1 = self.body(x0); x2 = self.att(x1)
        mx, _ = torch.max(x2 * x1, dim=1, keepdim=True); x3 = self.att1(mx)
        x4 = x1 * (1.0 + torch.mul(x2, x3))                       # 残差式(v8)
        mu = (0.299 * xl[:, 0] + 0.587 * xl[:, 1] + 0.114 * xl[:, 2]).mean(dim=[1, 2]).unsqueeze(1)
        s = self.film_s(mu).unsqueeze(-1).unsqueeze(-1)
        sh = self.film_b(mu).unsqueeze(-1).unsqueeze(-1)
        x4 = x4 * (1.0 + s) + sh
        return self.tail(x4)                                     # (B,18,h,w) raw


def reparam_estimator(net):
    """把训练版 CoeffNetCTBG(net) 的多分支 backbone+FiLM reparam 进 CTBGSlimEstimator。"""
    ch = net.bb.channels
    slim = CTBGSlimEstimator(ch).eval()
    ws = slim.state_dict()
    for name, mod in net.bb.named_modules():               # 镜像 MobileIELLENet.slim 的拷贝
        if isinstance(mod, (MBRConv1, MBRConv3, MBRConv5)):
            if f"{name}.weight" in ws:
                w, b = mod.slim()
                ws[f"{name}.weight"] = w
                ws[f"{name}.bias"] = b
        elif isinstance(mod, FST):
            ws[f"{name}.bias"] = mod.bias
            ws[f"{name}.weight1"] = mod.weight1
            ws[f"{name}.weight2"] = mod.weight2
        elif isinstance(mod, nn.PReLU):
            ws[f"{name}.weight"] = mod.weight
    ws["film_s.weight"] = net.film_s.weight                 # FiLM 直接拷
    ws["film_b.weight"] = net.film_b.weight
    slim.load_state_dict(ws)
    return slim.eval()



def bilinear_kernel(ch, k):
    factor = (k + 1) // 2
    center = factor - 1 if k % 2 == 1 else factor - 0.5
    og = np.ogrid[:k, :k]
    filt = (1 - abs(og[0] - center) / factor) * (1 - abs(og[1] - center) / factor)
    w = np.zeros((ch, 1, k, k), np.float32)
    for i in range(ch):
        w[i, 0] = filt
    return torch.from_numpy(w)


def nearest_kernel(ch, k):
    """k=stride 的最近邻平铺核:每个 k×k 块全 1 → 输出块直接复制低分辨率系数值(权重和=1,无缩放)。
    用于 ksize=up 的提速档(块状上采样,系数本是低频量,块状可接受)。"""
    w = np.ones((ch, 1, k, k), np.float32)
    return torch.from_numpy(w)


class Estimator(nn.Module):
    """256×144 → 18ch *已 decode* 系数(a_d,b_d,g_d,a_b,b_b,g_b 各 3ch)。
    decode 的 tanh×6/exp×2 在低分辨率做,避免全分辨率超越函数(板端 apply 致死根因)。"""
    def __init__(self, slim_est, a_range, b_range, g_range):
        super().__init__()
        self.est = slim_est
        self.a_range, self.b_range, self.g_range = a_range, b_range, g_range

    def forward(self, xl):
        raw = self.est(xl)                                    # (B,18,h,w) raw
        a_d = 1.0 + self.a_range * torch.tanh(raw[:, 0:3])
        b_d = self.b_range * torch.tanh(raw[:, 3:6])
        g_d = torch.exp(self.g_range * torch.tanh(raw[:, 6:9]))
        a_b = 1.0 + self.a_range * torch.tanh(raw[:, 9:12])
        b_b = self.b_range * torch.tanh(raw[:, 12:15])
        g_b = torch.exp(self.g_range * torch.tanh(raw[:, 15:18]))
        return torch.cat([a_d, b_d, g_d, a_b, b_b, g_b], dim=1)   # (B,18,h,w) decoded


class ApplyCTBG18(nn.Module):
    """全分辨率 in + 单个 18ch decoded 系数张量 → 1 个 group=18 ConvTranspose 一次上采样 → 施加。
    用单次 group=18 替代 6×groups=3(各 ~22ms),省 kernel launch;fp16 输入(根因已解)。
    ksize 可配:k=2·up=8 双线性平滑(慢),k=up=4 近最近邻块状(routeB 实测 k4≈k8 的 0.46×,快)。
    gamma=True 双端 gamma 完整;gamma=False 为 affine-only 降级(去 log/exp)。"""
    def __init__(self, up=4, ksize=None, gamma=True):
        super().__init__()
        self.gamma = gamma
        k = ksize if ksize is not None else 2 * up
        pad = (k - up) // 2
        self.up = nn.ConvTranspose2d(18, 18, k, stride=up, padding=pad, groups=18, bias=False)
        # k==up 用最近邻平铺核(块状,权重和=1);k>up 用双线性核(平滑)
        self.up.weight.data = nearest_kernel(18, k) if k == up else bilinear_kernel(18, k)
        self.luma = nn.Conv2d(3, 3, 1, bias=False)
        lw = torch.tensor([0.299, 0.587, 0.114], dtype=torch.float32)
        self.luma.weight.data = lw.view(1, 3, 1, 1).repeat(3, 1, 1, 1)

    def forward(self, x, coeff):
        c = self.up(coeff)                                   # (B,18,H,W)
        a_d, b_d, g_d = c[:, 0:3], c[:, 3:6], c[:, 6:9]
        a_b, b_b, g_b = c[:, 9:12], c[:, 12:15], c[:, 15:18]
        w = self.luma(x).clamp(0, 1)
        a = (1 - w) * a_d + w * a_b
        b = (1 - w) * b_d + w * b_b
        if not self.gamma:
            return torch.clamp(a * x + b, 0, 1)
        g = (1 - w) * g_d + w * g_b
        xc = x.clamp(min=1e-3)
        return torch.clamp(a * torch.exp(g * torch.log(xc)) + b, 0, 1)


class ApplyCTBG(nn.Module):
    """全分辨率 in + 6 个独立 3ch decoded 系数(a_d,b_d,g_d,a_b,b_b,g_b)→ 上采样 → luma 插值 → 施加。

    输入契约严格对齐 routeB_apply(分离 3ch 输入,已板端验证 44.8ms 可跑):
    不再从 18ch 单张量切片(coeff[:,15:18] 跨 NC1HWC0 的 C0=16 边界 → 板端 warmup 挂死根因)。
    18ch→6×3ch 的切分在两 OM 之间的 host 侧做(NCHW 连续,廉价)。
    """
    def __init__(self, up=4, gamma=True):
        super().__init__()
        self.gamma = gamma
        k = 2 * up
        wk = bilinear_kernel(3, k)
        n_up = 6 if gamma else 4
        self.ups = nn.ModuleList([
            nn.ConvTranspose2d(3, 3, k, stride=up, padding=up // 2, groups=3, bias=False)
            for _ in range(n_up)])
        for m in self.ups:
            m.weight.data = wk.clone()
        self.luma = nn.Conv2d(3, 3, 1, bias=False)
        lw = torch.tensor([0.299, 0.587, 0.114], dtype=torch.float32)
        self.luma.weight.data = lw.view(1, 3, 1, 1).repeat(3, 1, 1, 1)

    def forward(self, x, a_d, b_d, a_b, b_b, g_d=None, g_b=None):
        w = self.luma(x).clamp(0, 1)
        a = (1 - w) * self.ups[0](a_d) + w * self.ups[2 if not self.gamma else 3](a_b)
        b = (1 - w) * self.ups[1](b_d) + w * self.ups[3 if not self.gamma else 4](b_b)
        if not self.gamma:
            return torch.clamp(a * x + b, 0, 1)
        g = (1 - w) * self.ups[2](g_d) + w * self.ups[5](g_b)
        xc = x.clamp(min=1e-3)
        return torch.clamp(a * torch.exp(g * torch.log(xc)) + b, 0, 1)


def scan_onnx(path):
    import onnx
    ops = {}
    for n in onnx.load(str(path)).graph.node:
        ops[n.op_type] = ops.get(n.op_type, 0) + 1
    return dict(sorted(ops.items())), sorted(set(ops) & RED)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", default=str(HERE / "runs/coeff_ctbg_v8/ckpt/best-epoch=0041-val_psnr=19.82.ckpt"))
    ap.add_argument("--outdir", default=str(HERE / "runs/om_prep_ctbg"))
    ap.add_argument("--full", type=int, nargs=2, default=[576, 1024])   # H W
    ap.add_argument("--down", type=int, default=4)
    args = ap.parse_args()
    outdir = Path(args.outdir); outdir.mkdir(parents=True, exist_ok=True)
    H, W = args.full
    h, w = H // args.down, W // args.down

    net = M.LitCTBG.load_from_checkpoint(args.ckpt, map_location="cpu").net.eval()
    slim_est = reparam_estimator(net)
    n_train = sum(p.numel() for p in net.parameters())
    n_slim = sum(p.numel() for p in slim_est.parameters())
    # reparam 一致性:slim backbone vs 训练版 _backbone
    with torch.no_grad():
        xl_chk = torch.rand(1, 3, h, w)
        d_bb = (slim_est(xl_chk) - net._backbone(xl_chk)).abs().max().item()
    print(f"[reparam] 训练版 {n_train/1000:.1f}K → slim est {n_slim/1000:.1f}K  "
          f"backbone max|train-slim|={d_bb:.5f} {'✅' if d_bb < 1e-2 else '⚠️'}")

    est = Estimator(slim_est, net.a_range, net.b_range, net.g_range).eval()
    app = ApplyCTBG(up=args.down, gamma=True).eval()
    app_aff = ApplyCTBG(up=args.down, gamma=False).eval()

    # ---- 数值对拍:整模型 vs 拆分串联(gamma 版)----
    x = torch.rand(1, 3, H, W)
    with torch.no_grad():
        ref = net(x)[0]                                          # 整模型(F.interpolate 上采样)
        xl = F.interpolate(x, scale_factor=1.0 / args.down, mode="bilinear", align_corners=False)
        c = est(xl)                                              # (B,18,h,w) decoded
        a_d, b_d, g_d = c[:, 0:3], c[:, 3:6], c[:, 6:9]
        a_b, b_b, g_b = c[:, 9:12], c[:, 12:15], c[:, 15:18]
        out = app(x, a_d, b_d, a_b, b_b, g_d, g_b)
        diff = (ref - out).abs()
    print(f"[对拍] 整模型 vs 拆分(gamma): "
          f"max|diff|={diff.max():.4f} mean|diff|={diff.mean():.5f} "
          f"PSNR(out,ref)={M.psnr01(out.clamp(0,1), ref.clamp(0,1)):.2f}dB")

    # ---- 导出 ONNX(estimator + apply gamma + apply affine-only)----
    est_onnx = outdir / "ctbg_estimator_256x144.onnx"
    app_onnx = outdir / "ctbg_apply_1024x576.onnx"
    aff_onnx = outdir / "ctbg_apply_affine_1024x576.onnx"
    torch.onnx.export(est, xl, str(est_onnx), opset_version=11,
                      input_names=["in_low"], output_names=["coeff"])
    torch.onnx.export(app, (x, a_d, b_d, a_b, b_b, g_d, g_b), str(app_onnx), opset_version=11,
                      input_names=["in_full", "a_d", "b_d", "a_b", "b_b", "g_d", "g_b"],
                      output_names=["out"])
    torch.onnx.export(app_aff, (x, a_d, b_d, a_b, b_b), str(aff_onnx), opset_version=11,
                      input_names=["in_full", "a_d", "b_d", "a_b", "b_b"], output_names=["out"])

    # 变体:单 group=18 ConvTranspose(省 kernel launch,fp16 输入)
    app18 = ApplyCTBG18(up=args.down).eval()
    g18_onnx = outdir / "ctbg_apply_g18_1024x576.onnx"
    torch.onnx.export(app18, (x, c), str(g18_onnx), opset_version=11,
                      input_names=["in_full", "coeff"], output_names=["out"])

    # 提速变体:k4(=up)近最近邻上采样,routeB 实测 k4≈k8 的 0.46×。gamma + affine 各一。
    app_k4 = ApplyCTBG18(up=args.down, ksize=args.down, gamma=True).eval()
    app_k4a = ApplyCTBG18(up=args.down, ksize=args.down, gamma=False).eval()
    k4_onnx = outdir / "ctbg_apply_g18k4_1024x576.onnx"
    k4a_onnx = outdir / "ctbg_apply_g18k4_affine_1024x576.onnx"
    torch.onnx.export(app_k4, (x, c), str(k4_onnx), opset_version=11,
                      input_names=["in_full", "coeff"], output_names=["out"])
    torch.onnx.export(app_k4a, (x, c), str(k4a_onnx), opset_version=11,
                      input_names=["in_full", "coeff"], output_names=["out"])
    # k4 gamma 对拍(相对整模型):量化块状上采样的画质代价
    with torch.no_grad():
        out_k4 = app_k4(x, c)
    print(f"[对拍-k4] 整模型 vs g18-k4(gamma): "
          f"PSNR(k4,ref)={M.psnr01(out_k4.clamp(0,1), ref.clamp(0,1)):.2f}dB  "
          f"PSNR(k4,k8)={M.psnr01(out_k4.clamp(0,1), out.clamp(0,1)):.2f}dB")

    for tag, p in [("estimator", est_onnx), ("apply-gamma", app_onnx), ("apply-affine", aff_onnx),
                   ("apply-g18", g18_onnx), ("apply-g18k4", k4_onnx), ("apply-g18k4-affine", k4a_onnx)]:
        ops, red = scan_onnx(p)
        print(f"[{tag}] {p.name}\n  ops={ops}\n  红名单命中={red or '无 ✅'}")
    print(f"\n[done] ONNX -> {outdir}")


if __name__ == "__main__":
    main()
