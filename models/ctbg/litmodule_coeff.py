"""路线 B 系数头模型:MobileIE backbone → 逐像素空间系数(空间变化→光照统一)。
前向镜像部署:下采样输入→backbone 出低分辨率系数→上采样→施加。端到端可微。

v2(防 v1 仿射坍缩):
- **gamma 形式** `out=clamp(a·in^γ+b)`:γ<1 拉暗部,补仿射的提亮死角(in≈0 时 a·in+b 拉不亮)。
- **P1 蒸馏**:用训好的 P1 image-to-image(双向 20.28/22.88)作 teacher,distill 项强监督,抗坍缩 + 传双向能力。
- **降 TV**:v1 的 TV 正则把系数推成常数场=恒等;v2 减小。
tail 输出:gamma 版 9ch(a:3, b:3, γ:3),仿射版 6ch。部署上采样用 ConvTranspose(红名单安全)。
"""
import sys
from pathlib import Path
import torch, torch.nn as nn, torch.nn.functional as F
import lightning as L

REPO_MOBILEIE = Path(__file__).resolve().parents[1] / "repos/MobileIE"
sys.path.insert(0, str(REPO_MOBILEIE))
from model.lle import MobileIELLENet, MobileIELLENetS  # noqa: E402
from model.utils import MBRConv3  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parent))
from losses import LossLLE  # noqa: E402


def psnr01(a, b):
    mse = ((a.clamp(0, 1) - b) ** 2).mean(dim=(1, 2, 3))
    return (10 * torch.log10(1.0 / (mse + 1e-10))).mean()


class CoeffNet(nn.Module):
    """MobileIE backbone,tail 改出空间系数;低分辨率估计 + 全分辨率施加。"""
    def __init__(self, channels=12, rep_scale=4, down=4, gamma=True,
                 a_range=0.8, b_range=0.2, g_range=0.9):
        super().__init__()
        self.bb = MobileIELLENet(channels=channels, rep_scale=rep_scale)
        self.bb.tail = MBRConv3(channels, 9 if gamma else 6, rep_scale=rep_scale)
        self.gamma = gamma
        self.down, self.a_range, self.b_range, self.g_range = down, a_range, b_range, g_range

    def coeffs(self, xl):
        b = self.bb
        x0 = b.head(xl); x1 = b.body(x0); x2 = b.att(x1)
        mx, _ = torch.max(x2 * x1, dim=1, keepdim=True); x3 = b.att1(mx)
        x4 = torch.mul(x2, x3) * x1
        c = b.tail(x4)
        a = 1.0 + self.a_range * torch.tanh(c[:, :3])      # 增益(围绕1)
        bb_ = self.b_range * torch.tanh(c[:, 3:6])         # 偏置(小)
        g = torch.exp(self.g_range * torch.tanh(c[:, 6:9])) if self.gamma else None  # γ∈[~0.4,2.5]
        return a, bb_, g

    def forward(self, x):
        xl = F.interpolate(x, scale_factor=1.0 / self.down, mode="bilinear", align_corners=False)
        a, b, g = self.coeffs(xl)
        up = lambda t: F.interpolate(t, size=x.shape[-2:], mode="bilinear", align_corners=False)
        a, b = up(a), up(b)
        if self.gamma:
            g = up(g)
            out = torch.clamp(a * torch.pow(x.clamp(min=1e-3), g) + b, 0, 1)
            return out, (a, b, g)
        return torch.clamp(a * x + b, 0, 1), (a, b)


def tv(t):
    return (t[..., 1:, :] - t[..., :-1, :]).abs().mean() + (t[..., :, 1:] - t[..., :, :-1]).abs().mean()


def _y(im):   # 可微亮度(BT.601)
    return 0.299 * im[:, 0] + 0.587 * im[:, 1] + 0.114 * im[:, 2]


def _chroma(im):   # 可微饱和度代理 = max-min
    return im.max(1).values - im.min(1).values


def illum_lf(out, y, blocks=16):
    """低频光照匹配:把 out/y 亮度下采样到 blocks×blocks 块均值,匹配大尺度光照结构。
    对应 noref_metrics 的 illum_blockstd 粒度。监督输出去对齐 GT 的分块亮度图,
    逼系数走空间变化(非均匀输入→均匀 GT 时必须空间拉平,自然场景则保留结构)。"""
    oy, gy = _y(out).unsqueeze(1), _y(y).unsqueeze(1)   # (B,1,H,W)
    op = F.adaptive_avg_pool2d(oy, blocks)              # (B,1,16,16)
    gp = F.adaptive_avg_pool2d(gy, blocks)
    return F.l1_loss(op, gp)


class LitMobileIECoeff(L.LightningModule):
    def __init__(self, channels=12, rep_scale=4, down=4, lr=1e-3, max_epochs=400,
                 tv_w=0.002, gamma=True, distill_w=0.0, teacher_pkl=None,
                 b_range=0.2, std_w=0.0, sat_w=0.0):
        super().__init__()
        self.save_hyperparameters()
        self.net = CoeffNet(channels, rep_scale, down, gamma=gamma, b_range=b_range)
        self.loss = LossLLE()
        self.teacher = None
        if distill_w > 0 and teacher_pkl:
            self.teacher = MobileIELLENetS(channels=channels)
            self.teacher.load_state_dict(torch.load(teacher_pkl, map_location="cpu"))
            self.teacher.eval()
            for p in self.teacher.parameters():
                p.requires_grad_(False)

    def forward(self, x):
        return self.net(x)[0]

    def _step(self, batch, tag):
        x, y = batch
        out, coeffs = self.net(x)
        loss = self.loss(out, y) + self.hparams.tv_w * sum(tv(c) for c in coeffs)
        # 对比度(Y std)+ 饱和度保持(治低对比/掉饱和),逐图统计匹配 GT
        oy, gy = _y(out), _y(y)
        l_std = (oy.std(dim=(1, 2)) - gy.std(dim=(1, 2))).abs().mean()
        l_sat = (_chroma(out).mean(dim=(1, 2)) - _chroma(y).mean(dim=(1, 2))).abs().mean()
        loss = loss + self.hparams.std_w * l_std + self.hparams.sat_w * l_sat
        if self.teacher is not None and tag == "train":
            with torch.no_grad():
                t = self.teacher(x).clamp(0, 1)
            loss = loss + self.hparams.distill_w * self.loss(out, t)
        self.log(f"{tag}_loss", loss, prog_bar=True, on_epoch=True, sync_dist=(tag == "val"))
        self.log(f"{tag}_psnr", psnr01(out, y), prog_bar=True, on_epoch=True, sync_dist=(tag == "val"))
        return loss

    def training_step(self, b, _): return self._step(b, "train")
    def validation_step(self, b, _): self._step(b, "val")

    def configure_optimizers(self):
        opt = torch.optim.Adam(self.net.parameters(), lr=self.hparams.lr)
        sch = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=self.hparams.max_epochs)
        return {"optimizer": opt, "lr_scheduler": {"scheduler": sch, "interval": "epoch"}}


class CoeffNetCTBG(nn.Module):
    """v6 CTBG — Conditioned Two-point Bilateral Grid

    解决 v1-v5 的结构性 input-independent 问题:
    1. 双端系数: backbone 出 dark/bright 各一组9ch系数(18ch total)
       施加时按像素亮度 w=luma(pixel) 插值:
         coeff = (1-w)·coeff_dark + w·coeff_bright
       → 暗像素(w≈0)走 dark 系数(训练使其放大)
       → 亮像素(w≈1)走 bright 系数(训练使其压制)
       → 同一空间位置、不同强度 → 不同变换 = 近似 bilateral grid,无需 grid_sample
    2. FiLM 场景条件: mean_luma → backbone 瓶颈, 给"整帧方向"信号

    v8 结构修复(诊断:MobileIE 的 att 全局池化把空间变化压成 0,保留率 0.0%):
    - 覆盖式调制 `x4 = att·x1` → 残差式 `x4 = x1·(1+att)`,给空间特征 x1 直通路径
    - residual_spatial=True 时启用;att 退化为"全局精修"而非"覆盖",空间结构得以保留。
    """
    def __init__(self, channels=12, rep_scale=4, down=4,
                 a_range=0.8, b_range=0.02, g_range=0.9, residual_spatial=True):
        super().__init__()
        self.bb = MobileIELLENet(channels=channels, rep_scale=rep_scale)
        self.bb.tail = MBRConv3(channels, 18, rep_scale=rep_scale)  # 2×9ch
        self.down = down
        self.residual_spatial = residual_spatial
        self.a_range, self.b_range, self.g_range = a_range, b_range, g_range
        # FiLM: mean_luma(标量) → scale/shift 瓶颈特征(零初始化=恒等启动)
        self.film_s = nn.Linear(1, channels, bias=False)
        self.film_b = nn.Linear(1, channels, bias=False)
        nn.init.zeros_(self.film_s.weight)
        nn.init.zeros_(self.film_b.weight)

    def _backbone(self, xl):
        b = self.bb
        x0 = b.head(xl); x1 = b.body(x0); x2 = b.att(x1)
        mx, _ = torch.max(x2 * x1, dim=1, keepdim=True); x3 = b.att1(mx)
        if self.residual_spatial:
            # 残差式: x1 直通 + att 作精修增量 → 保留 x1 的空间变化(诊断证明 x1 std=24.9)
            x4 = x1 * (1.0 + torch.mul(x2, x3))
        else:
            x4 = torch.mul(x2, x3) * x1   # 原 MobileIE 覆盖式(空间塌缩)
        # FiLM: 全局均值亮度调制瓶颈
        mu = (0.299*xl[:,0] + 0.587*xl[:,1] + 0.114*xl[:,2]).mean(dim=[1,2]).unsqueeze(1)  # (B,1)
        s  = self.film_s(mu).unsqueeze(-1).unsqueeze(-1)   # (B,C,1,1)
        sh = self.film_b(mu).unsqueeze(-1).unsqueeze(-1)
        x4 = x4 * (1.0 + s) + sh
        return b.tail(x4)  # (B,18,h,w)

    @staticmethod
    def _decode(raw, a_range, b_range, g_range):
        a = 1.0 + a_range * torch.tanh(raw[:, 0:3])
        b = b_range  * torch.tanh(raw[:, 3:6])
        g = torch.exp(g_range * torch.tanh(raw[:, 6:9]))
        return a, b, g

    def forward(self, x):
        xl = F.interpolate(x, scale_factor=1.0/self.down, mode="bilinear", align_corners=False)
        c  = self._backbone(xl)                              # (B,18,h,w)
        up = lambda t: F.interpolate(t, size=x.shape[-2:], mode="bilinear", align_corners=False)
        a_d, b_d, g_d = self._decode(up(c[:, :9]),  self.a_range, self.b_range, self.g_range)
        a_b, b_b, g_b = self._decode(up(c[:, 9:18]), self.a_range, self.b_range, self.g_range)
        # 像素亮度作为插值权重: 暗像素→dark系数, 亮像素→bright系数
        w = (0.299*x[:,0:1] + 0.587*x[:,1:2] + 0.114*x[:,2:3]).clamp(0, 1)  # (B,1,H,W)
        a = (1 - w) * a_d + w * a_b
        b = (1 - w) * b_d + w * b_b
        g = (1 - w) * g_d + w * g_b
        out = torch.clamp(a * x.clamp(min=1e-3).pow(g) + b, 0, 1)
        return out, (a_d, b_d, g_d, a_b, b_b, g_b)


class LitCTBG(L.LightningModule):
    def __init__(self, channels=12, rep_scale=4, down=4, lr=1e-3, max_epochs=400,
                 tv_w=0.001, distill_w=0.0, teacher_pkl=None,
                 b_range=0.02, std_w=0.1, sat_w=0.1, illum_w=0.0, residual_spatial=True):
        super().__init__()
        self.save_hyperparameters()
        self.net = CoeffNetCTBG(channels, rep_scale, down, b_range=b_range,
                                residual_spatial=residual_spatial)
        self.loss = LossLLE()
        self.teacher = None
        if distill_w > 0 and teacher_pkl:
            self.teacher = MobileIELLENetS(channels=channels)
            self.teacher.load_state_dict(torch.load(teacher_pkl, map_location="cpu"))
            self.teacher.eval()
            for p in self.teacher.parameters():
                p.requires_grad_(False)

    def forward(self, x): return self.net(x)[0]

    def _step(self, batch, tag):
        x, y = batch
        out, coeffs = self.net(x)
        loss = self.loss(out, y) + self.hparams.tv_w * sum(tv(c) for c in coeffs)
        oy, gy = _y(out), _y(y)
        loss = (loss
                + self.hparams.std_w * (oy.std(dim=(1,2)) - gy.std(dim=(1,2))).abs().mean()
                + self.hparams.sat_w * (_chroma(out).mean(dim=(1,2)) - _chroma(y).mean(dim=(1,2))).abs().mean()
                + self.hparams.illum_w * illum_lf(out, y))
        if self.teacher is not None and tag == "train":
            with torch.no_grad():
                t = self.teacher(x).clamp(0, 1)
            loss = loss + self.hparams.distill_w * self.loss(out, t)
        self.log(f"{tag}_loss", loss, prog_bar=True, on_epoch=True, sync_dist=(tag=="val"))
        self.log(f"{tag}_psnr", psnr01(out, y), prog_bar=True, on_epoch=True, sync_dist=(tag=="val"))
        return loss

    def training_step(self, b, _): return self._step(b, "train")
    def validation_step(self, b, _): self._step(b, "val")

    def configure_optimizers(self):
        opt = torch.optim.Adam(self.net.parameters(), lr=self.hparams.lr)
        sch = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=self.hparams.max_epochs)
        return {"optimizer": opt, "lr_scheduler": {"scheduler": sch, "interval": "epoch"}}


if __name__ == "__main__":
    net = CoeffNet(gamma=True)
    x = torch.rand(2, 3, 256, 256)
    out, coeffs = net(x)
    n = sum(p.numel() for p in net.parameters())
    a, b, g = coeffs
    print(f"CoeffNet(gamma) 参数 {n/1000:.1f}K  out {tuple(out.shape)}")
    print(f"  a[{a.min():.2f},{a.max():.2f}] b[{b.min():.2f},{b.max():.2f}] γ[{g.min():.2f},{g.max():.2f}]")

    net6 = CoeffNetCTBG()
    out6, cf6 = net6(x)
    n6 = sum(p.numel() for p in net6.parameters())
    print(f"CoeffNetCTBG 参数 {n6/1000:.1f}K  out {tuple(out6.shape)}")
    a_d, b_d, g_d, a_b, b_b, g_b = cf6
    print(f"  dark:  a[{a_d.min():.2f},{a_d.max():.2f}] b[{b_d.min():.2f},{b_d.max():.2f}] γ[{g_d.min():.2f},{g_d.max():.2f}]")
    print(f"  bright:a[{a_b.min():.2f},{a_b.max():.2f}] b[{b_b.min():.2f},{b_b.max():.2f}] γ[{g_b.min():.2f},{g_b.max():.2f}]")
