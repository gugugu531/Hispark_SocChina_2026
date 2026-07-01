"""CTBG v8 正确版导出:decode 在 apply 端高分辨率做,恢复训练版顺序。
修复根因:tanh(up(raw)) ≠ up(tanh(raw)),非线性函数不能与线性插值交换。
代价:全分辨率 tanh(6×3ch)/exp(2×3ch) 回来了,需板端实测耗时。"""
import argparse, sys, onnx
from pathlib import Path
import torch, torch.nn as nn, torch.nn.functional as F

sys.path.insert(0, str(Path(__file__).parent))
from litmodule_coeff import LitCTBG, psnr01
from export_ctbg import CTBGSlimEstimator, bilinear_kernel, nearest_kernel, reparam_estimator


class EstimatorRaw(nn.Module):
    """256×144 → 18ch raw logits(未 decode)。decode 留给 apply 端。"""
    def __init__(self, slim_est):
        super().__init__()
        self.est = slim_est
    def forward(self, xl):
        return self.est(xl)


class ApplyCorrect(nn.Module):
    """全分辨率 in + 18ch raw → 单 group=18 ConvTranspose up raw → 高分辨率 decode → gamma。
    正确顺序(先 up 后 decode) + 最优 ConvTranspose 数(单 group=18,省 kernel launch)。"""
    def __init__(self, up=4, ksize=None, a_range=0.5, b_range=0.5, g_range=0.5):
        super().__init__()
        self.a_range, self.b_range, self.g_range = a_range, b_range, g_range
        k = ksize if ksize is not None else 2 * up
        pad = (k - up) // 2
        self.up = nn.ConvTranspose2d(18, 18, k, stride=up, padding=pad, groups=18, bias=False)
        kern = nearest_kernel(18, k) if k == up else bilinear_kernel(18, k)
        self.up.weight.data = kern

    def forward(self, x, raw):
        # 单 group=18 up raw → 高分辨率 decode(正确顺序:先 up 后 decode)
        raw_full = self.up(raw)  # (B,18,H,W)
        a_d = 1.0 + self.a_range * torch.tanh(raw_full[:, 0:3])
        b_d = self.b_range * torch.tanh(raw_full[:, 3:6])
        g_d = torch.exp(self.g_range * torch.tanh(raw_full[:, 6:9]))
        a_b = 1.0 + self.a_range * torch.tanh(raw_full[:, 9:12])
        b_b = self.b_range * torch.tanh(raw_full[:, 12:15])
        g_b = torch.exp(self.g_range * torch.tanh(raw_full[:, 15:18]))
        w = (0.299*x[:,0:1] + 0.587*x[:,1:2] + 0.114*x[:,2:3]).clamp(0, 1)
        a = (1-w)*a_d + w*a_b; b = (1-w)*b_d + w*b_b; g = (1-w)*g_d + w*g_b
        return torch.clamp(a * torch.exp(g * torch.log(x.clamp(min=1e-3))) + b, 0, 1)


class ApplyTwoStage(nn.Module):
    """方案C:两阶段上采样 + 中间分辨率 decode。
    stage1_nearest=False: Stage1 bilinear k4(画质优先,39dB,~3ms)
    stage1_nearest=True:  Stage1 nearest k2(速度优先,~31dB,~1.5ms,省约1.5ms→30fps)
    Stage2 始终 nearest k2(decoded 值不需要插值)。"""
    def __init__(self, a_range=0.5, b_range=0.5, g_range=0.5, stage1_nearest=False):
        super().__init__()
        self.a_range, self.b_range, self.g_range = a_range, b_range, g_range
        if stage1_nearest:
            self.up1 = nn.ConvTranspose2d(18, 18, 2, stride=2, groups=18, bias=False)
            self.up1.weight.data = nearest_kernel(18, 2)
        else:
            self.up1 = nn.ConvTranspose2d(18, 18, 4, stride=2, padding=1, groups=18, bias=False)
            self.up1.weight.data = bilinear_kernel(18, 4)
        self.up2 = nn.ConvTranspose2d(18, 18, 2, stride=2, groups=18, bias=False)
        self.up2.weight.data = nearest_kernel(18, 2)

    def forward(self, x, raw):
        m = self.up1(raw)                                    # (B,18,288,512) raw 中间分辨率
        a_d = 1.0 + self.a_range * torch.tanh(m[:, 0:3])
        b_d = self.b_range * torch.tanh(m[:, 3:6])
        g_d = torch.exp(self.g_range * torch.tanh(m[:, 6:9]))
        a_b = 1.0 + self.a_range * torch.tanh(m[:, 9:12])
        b_b = self.b_range * torch.tanh(m[:, 12:15])
        g_b = torch.exp(self.g_range * torch.tanh(m[:, 15:18]))
        d = torch.cat([a_d, b_d, g_d, a_b, b_b, g_b], dim=1)  # (B,18,288,512) decoded
        c = self.up2(d)                                      # (B,18,576,1024) 全分辨率 decoded
        w = (0.299*x[:,0:1] + 0.587*x[:,1:2] + 0.114*x[:,2:3]).clamp(0, 1)
        a = (1-w)*c[:,0:3] + w*c[:,9:12]
        b = (1-w)*c[:,3:6] + w*c[:,12:15]
        g = (1-w)*c[:,6:9] + w*c[:,15:18]
        return torch.clamp(a * torch.exp(g * torch.log(x.clamp(min=1e-3))) + b, 0, 1)


def scan_onnx(p):
    m = onnx.load(str(p)); ops = sorted(set(n.op_type for n in m.graph.node))
    red = [o for o in ops if o in {"Pow", "Cast", "ReduceMean"}]
    return ops, red


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--ckpt", default="p1_bidir/runs/coeff_ctbg_v8/ckpt/best-epoch=0041-val_psnr=19.82.ckpt")
    p.add_argument("--down", type=int, default=4)
    args = p.parse_args()

    outdir = Path("p1_bidir/runs/om_prep_ctbg_correct"); outdir.mkdir(exist_ok=True, parents=True)
    lit = LitCTBG.load_from_checkpoint(args.ckpt, map_location="cpu")
    net = lit.net.eval()
    H, W = 576, 1024

    slim_est = reparam_estimator(net)  # reparam 训练版权重到 slim
    est_raw = EstimatorRaw(slim_est).eval()
    app_k4 = ApplyCorrect(up=args.down, ksize=args.down,
                          a_range=net.a_range, b_range=net.b_range, g_range=net.g_range).eval()
    app_k8 = ApplyCorrect(up=args.down, ksize=2*args.down,
                          a_range=net.a_range, b_range=net.b_range, g_range=net.g_range).eval()
    app_c = ApplyTwoStage(a_range=net.a_range, b_range=net.b_range, g_range=net.g_range).eval()
    app_c_nn = ApplyTwoStage(a_range=net.a_range, b_range=net.b_range, g_range=net.g_range,
                             stage1_nearest=True).eval()

    print(f"[reparam] slim estimator {sum(p.numel() for p in slim_est.parameters())/1000:.1f}K")

    # 对拍
    x = torch.rand(1, 3, H, W)
    with torch.no_grad():
        ref, _ = net(x)
        xl = F.interpolate(x, scale_factor=1.0/args.down, mode="bilinear", align_corners=False)
        raw = est_raw(xl)
        out_k8 = app_k8(x, raw)
        out_k4 = app_k4(x, raw)
        out_c = app_c(x, raw)
        out_c_nn = app_c_nn(x, raw)
    print(f"[对拍-k8]   整模型 vs 正确版(先up后decode): PSNR={psnr01(out_k8, ref):.2f}dB")
    print(f"[对拍-k4]   整模型 vs 正确版(k4 nearest): PSNR={psnr01(out_k4, ref):.2f}dB")
    print(f"[对拍-C]    整模型 vs 方案C(bilinear+nearest):  PSNR={psnr01(out_c, ref):.2f}dB")
    print(f"[对拍-C-NN] 整模型 vs 方案C(nearest+nearest):   PSNR={psnr01(out_c_nn, ref):.2f}dB")

    # 导出 ONNX
    est_onnx = outdir / "ctbg_estimator_raw_256x144.onnx"
    app_k8_onnx = outdir / "ctbg_apply_correct_k8_1024x576.onnx"
    app_k4_onnx = outdir / "ctbg_apply_correct_k4_1024x576.onnx"
    app_c_onnx = outdir / "ctbg_apply_twostage_1024x576.onnx"
    app_c_nn_onnx = outdir / "ctbg_apply_twostage_nn_1024x576.onnx"
    torch.onnx.export(est_raw, xl, str(est_onnx), opset_version=11,
                      input_names=["in_low"], output_names=["raw_coeff"])
    torch.onnx.export(app_k8, (x, raw), str(app_k8_onnx), opset_version=11,
                      input_names=["in_full", "raw_coeff"], output_names=["out"])
    torch.onnx.export(app_k4, (x, raw), str(app_k4_onnx), opset_version=11,
                      input_names=["in_full", "raw_coeff"], output_names=["out"])
    torch.onnx.export(app_c, (x, raw), str(app_c_onnx), opset_version=11,
                      input_names=["in_full", "raw_coeff"], output_names=["out"])
    torch.onnx.export(app_c_nn, (x, raw), str(app_c_nn_onnx), opset_version=11,
                      input_names=["in_full", "raw_coeff"], output_names=["out"])

    for tag, p in [("estimator-raw", est_onnx), ("apply-k8", app_k8_onnx),
                   ("apply-k4", app_k4_onnx), ("apply-C", app_c_onnx),
                   ("apply-C-NN", app_c_nn_onnx)]:
        ops, red = scan_onnx(p)
        print(f"[{tag}] {p.name}\n  ops={ops}\n  红名单={red or '无 ✅'}")
    print(f"\n[done] ONNX → {outdir}")


if __name__ == "__main__":
    main()
