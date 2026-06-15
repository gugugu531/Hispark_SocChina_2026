"""双向曝光校正曲线预测网络 (ExpoCurveNet)。

任务：实时**双向曝光校正**（既拉亮暗部、也压制高光过曝），单输入、单帧。
本文件只负责"结构正确 + 算子干净上板"，权重随机即可（去风险任务，不做认真训练）。

借用来源 / Borrowed from
-----------------------
* Zero-DCE 上游骨架：``Zero-DCE/Zero-DCE_code/model.py`` 的 ``enhance_net_nopool``
  —— 7 层 3x3 卷积 + concat 跳连 + 末层 Tanh 出曲线参数 + 迭代施加 ``x = x + r*(x²-x)``。
* EnhanceNet / 16008_LightEnhancer（同芯片族竞赛实测改进版 Zero-DCE）
  —— "先 AvgPool 降分辨率跑 backbone、再上采样曲线图"的低算力布局；
     以及"训练态用插值 / 部署态用 ConvTranspose 模拟双线性"的双态约定。
* Zero-DCE-Lite（``tools/local/Zero-DCE-Lite/ZeroDCE/zero_dce_lite.py``）
  —— ``filters`` / ``iteration`` 参数化与轻量化参照。

相对参考做了什么 / What we changed and why
-----------------------------------------
1. **单帧、单输入**：本项目降噪交给 ISP 硬件（3DNR/HNR），不照搬 EnhanceNet 的 5 帧时域
   融合（避免运动鬼影 + ACL 多输入未验证 + 模型过重，见 architecture.md §1）。
2. **NCHW + 仅绿名单算子**（见 development-guide.md §6）：Conv / ConvTranspose(DepthwiseConv) /
   AveragePool / Clip / Tanh / Mul,Sub,Add / Concat / Split。隐层激活用 **Clip(0,6)=ReLU6**
   而非 ReLU —— 绿名单含 Clip 不含 Relu，用 Clip 严格落在硬约束内（语义近似、上板更稳）。
3. **双向曝光**：曲线参数 ``r`` 经 Tanh ∈ [-1,1]，逐像素可正可负；``x = x + r*(x²-x)`` 中 ``r``
   的符号决定提亮/压暗 —— 原版 Zero-DCE 偏单向提亮，本网络**结构不变即获双向能力**
   （见 architecture.md §1）。
4. **上采样恒用 ConvTranspose**（固定 bilinear 权重、深度可分组）模拟双线性，导出态绝不出现
   Resize/F.interpolate（NNN 实测 Resize 异常，见 development-guide.md §6）。
5. **niter 与 filters 全参数化**：超预算时可快速砍小重测（去风险任务的核心诉求）。
"""

from __future__ import annotations

import torch
import torch.nn as nn
import torch.nn.functional as F


def bilinear_kernel(channels: int, scale: int) -> torch.Tensor:
    """生成 ConvTranspose 用的双线性上采样权重（深度可分，每通道独立用同一核）。

    搭配 ``kernel_size=2*scale, stride=scale, padding=scale//2``（scale 为偶数时）可严格放大
    ``scale`` 倍。实现为经典 FCN ``get_upsampling_weight``。返回 shape ``(channels, 1, k, k)``。
    """
    ksize = 2 * scale
    factor = (ksize + 1) // 2
    center = factor - 1.0 if ksize % 2 == 1 else factor - 0.5
    og = torch.arange(ksize, dtype=torch.float32)
    filt_1d = 1.0 - torch.abs(og - center) / factor
    filt_2d = filt_1d[:, None] * filt_1d[None, :]  # (k, k) 双线性核
    weight = torch.zeros(channels, 1, ksize, ksize, dtype=torch.float32)
    weight[:, 0] = filt_2d
    return weight


class ExpoCurveNet(nn.Module):
    """低分辨率预测曲线 + 全分辨率施加曲线的双向曝光校正网络。

    数据通路（architecture.md §2 第 7 级）::

        x(1x3xHxW) ─AvgPool/down─► backbone @ (H/down, W/down)
                  ─► 曲线参数 a(3*niter ch) ─Tanh─► ConvTranspose 上采样回全分辨率
                  ─► split 成 niter 组(每组 3 ch)，全分辨率逐像素迭代 x=x+r(x²-x)
                  ─► (可选) Clip[0,1] ─► 增强帧(1x3xHxW)

    参数
    ----
    niter : 曲线迭代次数（决定曲线参数通道数 = in_channels*niter）。超时优先砍它。
    filters : backbone 卷积通道数（算力主体）。超时其次砍它。
    down : 降采样倍率（AvgPool 核=步长=down；上采样同倍）。默认 4 → 576x1024 上在 144x256 跑。
    act_max : 隐层 Clip 上界。默认 6.0（ReLU6，保证导出为 ONNX ``Clip`` 而非 ``Relu``）。
              置 None 则用 clamp(min=0)（视 torch 版本可能导出为 ``Relu``，不推荐）。
    clip_output : 末尾是否 Clip 到 [0,1] 归一化域（绿名单 Clip，保证输出落在合法域）。
    train_upsample : 仅训练态可选用 F.interpolate 双线性上采样（更快）。**绝不用于导出**
                     —— 导出/部署一律走 ConvTranspose（见类文档第 4 点）。
    shared_curve : False（默认，architecture.md §2 原义）每次迭代用独立 3 通道曲线，
                   曲线参数共 3*niter 通道；True（Zero-DCE-Lite 变体）只预测 1 套 3 通道
                   曲线、niter 次复用 —— 全分辨率只需上采样/驻留 3 通道而非 3*niter 通道，
                   大幅削减全分辨率显存流量（板端实测见 expo-curve-network.md）。
    """

    def __init__(
        self,
        niter: int = 8,
        filters: int = 16,
        down: int = 4,
        in_channels: int = 3,
        act_max: float | None = 6.0,
        clip_output: bool = True,
        train_upsample: bool = False,
        shared_curve: bool = False,
    ) -> None:
        super().__init__()
        if down % 2 != 0:
            raise ValueError(f"down 需为偶数以保证 ConvTranspose 严格放大，收到 down={down}")
        self.niter = niter
        self.filters = filters
        self.down = down
        self.in_channels = in_channels
        self.act_max = act_max
        self.clip_output = clip_output
        self.train_upsample = train_upsample
        self.shared_curve = shared_curve
        # 共享曲线只出 3 通道并复用 niter 次；否则每次迭代独立 3 通道，共 3*niter 通道
        self.curve_channels = in_channels if shared_curve else in_channels * niter

        f, c = filters, in_channels

        # /down 降采样：算力主体落在低分辨率（绿名单 AveragePool）
        self.downsample = nn.AvgPool2d(kernel_size=down, stride=down)

        # Zero-DCE 式 7 层卷积 backbone（concat 跳连），全程跑在 (H/down, W/down)
        self.e_conv1 = nn.Conv2d(c, f, 3, 1, 1)
        self.e_conv2 = nn.Conv2d(f, f, 3, 1, 1)
        self.e_conv3 = nn.Conv2d(f, f, 3, 1, 1)
        self.e_conv4 = nn.Conv2d(f, f, 3, 1, 1)
        self.e_conv5 = nn.Conv2d(2 * f, f, 3, 1, 1)
        self.e_conv6 = nn.Conv2d(2 * f, f, 3, 1, 1)
        self.e_conv7 = nn.Conv2d(2 * f, self.curve_channels, 3, 1, 1)

        # ConvTranspose 上采样曲线参数回全分辨率（固定 bilinear、深度可分组 groups=C）
        ksize = 2 * down
        self.upsample = nn.ConvTranspose2d(
            self.curve_channels,
            self.curve_channels,
            kernel_size=ksize,
            stride=down,
            padding=down // 2,
            groups=self.curve_channels,
            bias=False,
        )
        self.upsample.weight.data.copy_(bilinear_kernel(self.curve_channels, down))
        self.upsample.weight.requires_grad_(False)  # 固定双线性核，不参与训练

    def _act(self, x: torch.Tensor) -> torch.Tensor:
        # 绿名单激活：Clip。act_max=6 → ReLU6（保证导出为 Clip）；act_max=None → clamp(min=0)
        if self.act_max is None:
            return torch.clamp(x, min=0.0)
        return torch.clamp(x, 0.0, self.act_max)

    def _upsample_curve(self, a: torch.Tensor, out_hw: tuple[int, int]) -> torch.Tensor:
        if self.train_upsample:
            # 仅训练态：F.interpolate 双线性（更快）。导出态绝不走此分支 —— NNN 不支持 Resize。
            return F.interpolate(a, size=out_hw, mode="bilinear", align_corners=False)
        return self.upsample(a)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        h, w = x.shape[-2], x.shape[-1]

        xd = self.downsample(x)  # → (H/down, W/down)

        x1 = self._act(self.e_conv1(xd))
        x2 = self._act(self.e_conv2(x1))
        x3 = self._act(self.e_conv3(x2))
        x4 = self._act(self.e_conv4(x3))
        x5 = self._act(self.e_conv5(torch.cat([x3, x4], dim=1)))
        x6 = self._act(self.e_conv6(torch.cat([x2, x5], dim=1)))
        a = torch.tanh(self.e_conv7(torch.cat([x1, x6], dim=1)))  # 曲线参数 ∈ [-1,1]

        a = self._upsample_curve(a, (h, w))  # 上采样到全分辨率
        if self.shared_curve:
            curves = [a] * self.niter  # 单套 3 通道曲线，复用 niter 次
        else:
            curves = torch.split(a, self.in_channels, dim=1)  # niter 组，每组 3 通道

        # 全分辨率逐像素施加：x = x + r*(x²-x)，迭代 niter 次（r 可正可负 → 双向曝光）
        out = x
        for r in curves:
            out = out + r * (out * out - out)  # 用 Mul 实现 x²，避开 Pow，逐像素廉价

        if self.clip_output:
            out = torch.clamp(out, 0.0, 1.0)  # 钳到 [0,1] 归一化域（绿名单 Clip）
        return out


def build_model(niter: int = 8, filters: int = 16, **kwargs) -> ExpoCurveNet:
    """工厂函数：构建 eval 态网络（去风险任务用随机权重即可）。"""
    model = ExpoCurveNet(niter=niter, filters=filters, **kwargs)
    model.eval()
    return model


if __name__ == "__main__":
    # 自检：打印 IO shape、参数量、输出范围
    torch.manual_seed(0)
    net = build_model(niter=8, filters=16)
    dummy = torch.rand(1, 3, 576, 1024)
    with torch.no_grad():
        y = net(dummy)
    n_params = sum(p.numel() for p in net.parameters())
    print(f"niter={net.niter} filters={net.filters} curve_channels={net.curve_channels}")
    print(f"params={n_params}")
    print(f"in  shape={tuple(dummy.shape)}")
    print(f"out shape={tuple(y.shape)}  range=[{y.min():.4f}, {y.max():.4f}]")
