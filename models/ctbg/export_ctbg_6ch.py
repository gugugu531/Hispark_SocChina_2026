"""v9 6ch 模型导出: estimator(3ch→6ch raw) + apply(全分辨率 in + 预 up coeff)。
apply OM 含 tanh/exp/gamma(与 v8 正确版对比适用)。"""
import argparse, sys, onnx
from pathlib import Path
import torch, torch.nn as nn, torch.nn.functional as F

sys.path.insert(0, str(Path(__file__).parent))
from litmodule_coeff import LitCTBG_6ch
from export_ctbg import CTBGSlimEstimator, bilinear_kernel, nearest_kernel, reparam_estimator

# Note: LitCTBG_6ch 使用 CoeffNetCTBG_6ch (tail=6ch, scalar a,b,g per side)
# estimator 需要输出 6ch raw。apply 收全分辨率 in + 预 up 6ch coeff。

class Estimator6ch(nn.Module):
    """3×144×256 → 6ch raw (未 decode)"""
    def __init__(self, slim_est):
        super().__init__()
        self.est = slim_est
    def forward(self, xl):
        return self.est(xl)  # (B,6,144,256) raw

class Apply6ch(nn.Module):
    """全分辨率 in + 6ch coeff_up(预 up 到 576×1024)→ decode + luma blend + gamma"""
    def __init__(self, a_range=0.8, b_range=0.02, g_range=0.9):
        super().__init__()
        self.a_range, self.b_range, self.g_range = a_range, b_range, g_range

    def forward(self, x, coeff_up):
        # coeff_up: (B,6,576,1024) already upsampled from 144×256
        a_d = 1.0 + self.a_range * torch.tanh(coeff_up[:, 0:1])
        b_d = self.b_range * torch.tanh(coeff_up[:, 1:2])
        g_d = torch.exp(self.g_range * torch.tanh(coeff_up[:, 2:3]))
        a_b = 1.0 + self.a_range * torch.tanh(coeff_up[:, 3:4])
        b_b = self.b_range * torch.tanh(coeff_up[:, 4:5])
        g_b = torch.exp(self.g_range * torch.tanh(coeff_up[:, 5:6]))

        w = (0.299*x[:,0:1] + 0.587*x[:,1:2] + 0.114*x[:,2:3]).clamp(0, 1)
        a = (1-w)*a_d + w*a_b
        b = (1-w)*b_d + w*b_b
        g = (1-w)*g_d + w*g_b
        return torch.clamp(a * torch.exp(g * torch.log(x.clamp(min=1e-3))) + b, 0, 1)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--ckpt", default="p1_bidir/runs/coeff_ctbg_v9_6ch/ckpt/best-epoch=0116-val_psnr=19.83.ckpt")
    args = p.parse_args()

    outdir = Path("p1_bidir/runs/om_ctbg_6ch"); outdir.mkdir(exist_ok=True, parents=True)

    lit = LitCTBG_6ch.load_from_checkpoint(args.ckpt, map_location="cpu")
    net = lit.net.eval()
    H, W = 576, 1024; h, w = 144, 256

    # reparam backbone: 构造后替换 tail 为 6ch
    backbone = net.bb
    from export_ctbg import MBRConv1, MBRConv3, MBRConv5, FST

    slim_est = CTBGSlimEstimator(channels=12)
    # 替换 tail: 18ch → 6ch
    slim_est.tail = nn.Conv2d(12, 6, 3, 1, 1)
    ws = slim_est.state_dict()
    for name, mod in backbone.named_modules():
        if isinstance(mod, (MBRConv1, MBRConv3, MBRConv5)):
            k = f"{name}.weight"
            if k in ws:
                wb, bb_ = mod.slim()
                # If shape mismatch (tail: 18ch vs 6ch), take first 6 output ch
                if wb.shape[0] != ws[k].shape[0]:
                    wb = wb[:ws[k].shape[0]]
                    bb_ = bb_[:ws[k].shape[0]]
                ws[k] = wb; ws[f"{name}.bias"] = bb_
        elif isinstance(mod, FST):
            if f"{name}.bias" in ws:
                ws[f"{name}.bias"] = mod.bias
                ws[f"{name}.weight1"] = mod.weight1
                ws[f"{name}.weight2"] = mod.weight2
        elif isinstance(mod, nn.PReLU):
            if f"{name}.weight" in ws:
                ws[f"{name}.weight"] = mod.weight
    ws["film_s.weight"] = net.film_s.weight
    ws["film_b.weight"] = net.film_b.weight
    slim_est.load_state_dict(ws)
    slim_est.eval()

    est = Estimator6ch(slim_est).eval()
    app = Apply6ch(net.a_range, net.b_range, net.g_range).eval()

    # Parity check
    x = torch.rand(1, 3, H, W)
    with torch.no_grad():
        ref, _ = net(x)
        xl = F.interpolate(x, scale_factor=0.25, mode="bilinear", align_corners=False)
        raw = est(xl)  # (1,6,144,256)
        coeff_up = F.interpolate(raw, size=(H, W), mode="nearest")  # pre-upsample
        out = app(x, coeff_up)
        from litmodule_coeff import psnr01
        print(f"[对拍] 整模型 vs 拆分(预up coeff): PSNR={psnr01(out, ref):.2f}dB")
        print(f"  ref luma={ref.mean()*255:.0f}  est luma={out.mean()*255:.0f}")

    # Export ONNX
    est_onnx = outdir / "ctbg6ch_estimator_256x144.onnx"
    app_onnx = outdir / "ctbg6ch_apply_1024x576.onnx"
    torch.onnx.export(est, xl, str(est_onnx), opset_version=11,
                      input_names=["in_low"], output_names=["raw_coeff"])
    torch.onnx.export(app, (x, coeff_up), str(app_onnx), opset_version=11,
                      input_names=["in_full", "coeff_up"], output_names=["out"])

    for tag, p in [("estimator-6ch", est_onnx), ("apply-6ch", app_onnx)]:
        m = onnx.load(str(p))
        ops = sorted(set(n.op_type for n in m.graph.node))
        red = [o for o in ops if o in {"Pow", "Cast", "ReduceMean"}]
        print(f"[{tag}] {p.name} ops={len(ops)} 红名单={red or '无 ✅'}")
    print(f"\n[done] ONNX → {outdir}")


if __name__ == "__main__":
    main()
