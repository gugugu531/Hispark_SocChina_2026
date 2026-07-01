"""P1 损失:复刻 MobileIE 官方 LossLLE(OutlierAware + 1-cosine + 2*PSNRLoss)。
官方 loss.py 顶部 `from option import get_option` 是死 import(类里没用到),此处重写干净版。"""
import torch
import torch.nn as nn


class OutlierAwareLoss(nn.Module):
    """对离群残差加权的 L1(MobileIE 官方),抑制少数大误差像素主导。"""
    def forward(self, out, lab):
        delta = out - lab
        var = delta.std((2, 3), keepdims=True) / (2 ** 0.5)
        avg = delta.mean((2, 3), True)
        weight = torch.tanh((delta - avg).abs() / (var + 1e-6)).detach()
        return (delta.abs() * weight).mean()


class PSNRLoss(nn.Module):
    """官方 PSNRLoss:(50 - psnr)/100,可微 PSNR 最大化。"""
    def forward(self, pred, target):
        imdff = pred - target
        rmse = ((imdff ** 2).mean(dim=(1, 2, 3)) + 1e-8).sqrt()
        psnr = 20 * torch.log10(1.0 / rmse).mean()
        return (50.0 - psnr) / 100.0


class LossLLE(nn.Module):
    def __init__(self):
        super().__init__()
        self.cs = nn.CosineSimilarity()
        self.oa = OutlierAwareLoss()
        self.psnr = PSNRLoss()

    def forward(self, out, gt):
        return self.oa(out, gt) + (1 - self.cs(out.clip(0, 1), gt)).mean() + 2 * self.psnr(out, gt)
