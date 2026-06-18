"""可视化演示：CoTF 3D-LUT 经 *同一条 host→硬件桥* 施加到真实图像（input→gt）。

param-net 当前是随机权重的速率/可行性探针，自己产不出有意义的曝光校正。本演示因此
**拟合**一个 input→gt 的立方 3D-LUT（即训练好的 param-net 应当输出的东西），再把它喂进
与板端完全相同的桥：

    cubic LUT ──pack_cubic_to_hw──► 5508 节点 u32 ──write_lut_bin──► cotf_clut.bin
        ──(读回)──► 17×18×18 硬件 mesh ──三线性施加(模拟 ISP CLUT 硬件)──► 全分辨率输出

输出 input | 经 .bin 施加 | gt 的对比图，并报告施加前/后 PSNR。
证明：cotf_clut.bin 里的字节，按 ISP CLUT 的方式解释，能在全分辨率重建该校正。
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image

from models.tools.cotf_lut_pack import (
    HW_MESH_DIMS,
    HW_MAX,
    pack_cubic_to_hw,
    write_lut_bin,
)

MODELS_DIR = Path(__file__).resolve().parents[1]


def _trilinear_weights(pos, n):
    """pos(N,)∈[0,n-1] → (lo idx, hi idx, hi weight)。"""
    lo = np.clip(np.floor(pos).astype(int), 0, n - 2)
    frac = pos - lo
    return lo, lo + 1, frac


def fit_cubic_lut(inp: np.ndarray, gt: np.ndarray, dim: int) -> np.ndarray:
    """拟合立方 LUT L(dim^3,3)，使 trilinear_sample(L, inp) ≈ gt。

    用三线性的伴随（scatter）把 gt 累加进以 inp 颜色为索引的 LUT 桶，再归一化；
    空桶回退到恒等。这正是"训练好的 param-net 会给出的全局 3D-LUT"。
    """
    inp = inp.reshape(-1, 3)
    gt = gt.reshape(-1, 3)
    pos = inp * (dim - 1)
    los, his, fr = [], [], []
    for c in range(3):
        lo, hi, f = _trilinear_weights(pos[:, c], dim)
        los.append(lo); his.append(hi); fr.append(f)

    acc = np.zeros((dim, dim, dim, 3), dtype=np.float64)
    wsum = np.zeros((dim, dim, dim), dtype=np.float64)
    for dr in (0, 1):
        for dg in (0, 1):
            for db in (0, 1):
                ir = his[0] if dr else los[0]
                ig = his[1] if dg else los[1]
                ib = his[2] if db else los[2]
                w = ((fr[0] if dr else 1 - fr[0]) *
                     (fr[1] if dg else 1 - fr[1]) *
                     (fr[2] if db else 1 - fr[2]))
                np.add.at(acc, (ir, ig, ib), w[:, None] * gt)
                np.add.at(wsum, (ir, ig, ib), w)

    lut = np.zeros_like(acc)
    nz = wsum > 1e-6
    lut[nz] = acc[nz] / wsum[nz][:, None]
    # 空桶回退到恒等映射
    axis = np.linspace(0.0, 1.0, dim)
    rr, gg, bb = np.meshgrid(axis, axis, axis, indexing="ij")
    ident = np.stack([rr, gg, bb], axis=-1)
    lut[~nz] = ident[~nz]
    return lut


def apply_hw_mesh(packed_u32: np.ndarray, img: np.ndarray,
                  mesh_dims=HW_MESH_DIMS) -> np.ndarray:
    """按 ISP CLUT 硬件方式施加：解包 5508 节点 u32 → 17×18×18 mesh → 三线性查表。

    与 cotf_lut_pack.resample_to_hw_mesh 的几何一致（R 外层 row-major, 各轴 linspace）。
    """
    nr, ng, nb = mesh_dims
    r = ((packed_u32 >> 20) & 0x3FF).astype(np.float64) / HW_MAX
    g = ((packed_u32 >> 10) & 0x3FF).astype(np.float64) / HW_MAX
    b = (packed_u32 & 0x3FF).astype(np.float64) / HW_MAX
    mesh = np.stack([r, g, b], axis=1).reshape(nr, ng, nb, 3)

    flat = img.reshape(-1, 3)
    out = np.zeros_like(flat)
    los, his, fr = [], [], []
    for c, n in enumerate((nr, ng, nb)):
        lo, hi, f = _trilinear_weights(flat[:, c] * (n - 1), n)
        los.append(lo); his.append(hi); fr.append(f)
    for dr in (0, 1):
        for dg in (0, 1):
            for db in (0, 1):
                ir = his[0] if dr else los[0]
                ig = his[1] if dg else los[1]
                ib = his[2] if db else los[2]
                w = ((fr[0] if dr else 1 - fr[0]) *
                     (fr[1] if dg else 1 - fr[1]) *
                     (fr[2] if db else 1 - fr[2]))
                out += w[:, None] * mesh[ir, ig, ib]
    return out.reshape(img.shape)


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = np.mean((a - b) ** 2)
    return float("inf") if mse == 0 else 10 * np.log10(1.0 / mse)


def _load(path, size=None):
    im = Image.open(path).convert("RGB")
    if size is not None:
        im = im.resize(size, Image.BILINEAR)
    return np.asarray(im, dtype=np.float64) / 255.0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True, help="输入图像路径")
    ap.add_argument("--gt", required=True, help="参考图像路径")
    ap.add_argument("--lut-dim", type=int, default=17)
    ap.add_argument("--bin", default=str(MODELS_DIR / "weights" / "cotf_clut_demo.bin"))
    ap.add_argument("--out", default=str(MODELS_DIR / "weights" / "cotf_demo_compare.png"))
    args = ap.parse_args()

    inp = _load(args.input)
    h, w = inp.shape[:2]
    gt = _load(args.gt, size=(w, h))

    # 1) 拟合 input→gt 的立方 LUT（训练好的 param-net 的等价输出）
    cubic = fit_cubic_lut(inp, gt, args.lut_dim)

    # 2) 经与板端完全相同的桥：cube → 5508 u32 → cotf_clut_demo.bin
    packed = pack_cubic_to_hw(cubic)
    write_lut_bin(args.bin, packed)

    # 3) 读回 .bin，按 ISP CLUT 硬件方式全分辨率施加
    back = np.fromfile(args.bin, dtype="<u4")
    corrected = np.clip(apply_hw_mesh(back, inp), 0.0, 1.0)

    p_before = psnr(inp, gt)
    p_after = psnr(corrected, gt)

    # 4) 拼 input | corrected(.bin) | gt
    def u8(x):
        return (np.clip(x, 0, 1) * 255).round().astype(np.uint8)
    gap = np.full((h, 8, 3), 255, np.uint8)
    panel = np.concatenate([u8(inp), gap, u8(corrected), gap, u8(gt)], axis=1)
    Image.fromarray(panel).save(args.out)

    print(f"input {w}x{h}  lut_dim={args.lut_dim}  mesh={HW_MESH_DIMS}={back.size} nodes")
    print(f"  wrote {args.bin} ({packed.size*4} bytes)  u32 range "
          f"[{packed.min():#010x},{packed.max():#010x}]")
    print(f"  PSNR(input vs gt)            = {p_before:5.2f} dB")
    print(f"  PSNR(LUT-from-.bin vs gt)    = {p_after:5.2f} dB   "
          f"(+{p_after - p_before:.2f} dB)")
    print(f"  对比图: {args.out}  [input | 经 cotf_clut_demo.bin 施加 | gt]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
