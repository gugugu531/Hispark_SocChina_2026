"""将 NN 预测的 3D-LUT 打包为 SS928 ISP CLUT 的 u32 格式。

硬件格式（核对自 SDK 头 `ot_common_isp.h` / `ot_isp_define.h`）：
  * `ot_isp_clut_lut.lut[OT_ISP_CLUT_LUT_LENGTH]`，`OT_ISP_CLUT_LUT_LENGTH = 5508` 个节点。
  * 每个 `td_u32` 取值 [0, 1073741823] = [0, 2^30-1]，即 **3×10bit 打包**（R/G/B 各 10bit）。
  * 另有 `ot_isp_clut_attr = { en, gain_r/g/b(12bit) }` 做全局增益+开关。
  * 节点几何：5508 = 17×18×18（非均匀 3D mesh）。

整条桥（host 侧）::

    NN param-net 输出 (3*D^3,) ──decode──► 立方 LUT (D,D,D,3) float[0,1]
        ──resample_to_hw_mesh──► (5508,3) float ──pack_u30──► (5508,) uint32
        ──write_lut_bin──► .bin（板端读入 → ss_mpi_isp_set_clut_coeff）

⚠ 几何假设：mesh 维度/轴序/遍历顺序（默认 17×18×18、R 外层 row-major）**需对照 SDK《ISP CLUT 调优说明》
最终标定**；下面实现是结构完整、可跑通的版本，标定只改 `HW_MESH_DIMS` 与遍历顺序常量，不动主流程。
**10bit 位打包格式来自头文件，可信。**
"""

from __future__ import annotations

import numpy as np

HW_LUT_LENGTH = 5508          # OT_ISP_CLUT_LUT_LENGTH
HW_BITS = 10                  # 每通道 10 bit
HW_MAX = (1 << HW_BITS) - 1   # 1023
HW_MESH_DIMS = (17, 18, 18)   # 17*18*18 = 5508（轴序/大小待 CLUT 调优文档确认）
assert HW_MESH_DIMS[0] * HW_MESH_DIMS[1] * HW_MESH_DIMS[2] == HW_LUT_LENGTH


def decode_paramnet_output(flat: np.ndarray, dim: int) -> np.ndarray:
    """param-net 输出 (3*dim^3,) → 立方 LUT (dim,dim,dim,3) float[0,1]。

    约定通道排布为 (3, dim^3) 先 RGB 后空间，再 reshape 成 (3,dim,dim,dim) 转 (dim,dim,dim,3)。
    （随机权重下排布仅影响语义不影响结构；训练时固定此约定即可。）
    """
    flat = np.asarray(flat, dtype=np.float64).reshape(3, dim, dim, dim)
    lut = np.clip(np.transpose(flat, (1, 2, 3, 0)), 0.0, 1.0)  # (D,D,D,3)
    return lut


def _trilinear_sample(cube: np.ndarray, coords: np.ndarray) -> np.ndarray:
    """在立方 LUT cube(D,D,D,3) 上对 coords(N,3)∈[0,1] 做三线性采样 → (N,3)。"""
    d = cube.shape[0]
    pos = coords * (d - 1)
    lo = np.floor(pos).astype(int)
    lo = np.clip(lo, 0, d - 2)
    frac = pos - lo
    out = np.zeros((coords.shape[0], 3), dtype=np.float64)
    for dr in (0, 1):
        for dg in (0, 1):
            for db in (0, 1):
                w = (np.where(dr, frac[:, 0], 1 - frac[:, 0]) *
                     np.where(dg, frac[:, 1], 1 - frac[:, 1]) *
                     np.where(db, frac[:, 2], 1 - frac[:, 2]))
                idx = (lo[:, 0] + dr, lo[:, 1] + dg, lo[:, 2] + db)
                out += w[:, None] * cube[idx]
    return out


def resample_to_hw_mesh(cubic_lut: np.ndarray, mesh_dims: tuple = HW_MESH_DIMS) -> np.ndarray:
    """把立方 LUT (D,D,D,3) 三线性重采样到硬件 mesh（默认 17×18×18，共 5508 节点）→ (5508,3)。

    遍历顺序：R 外层、G 中层、B 内层（row-major）。⚠ 实际硬件轴序/顺序待 CLUT 调优文档确认。
    """
    nr, ng, nb = mesh_dims
    r = np.linspace(0.0, 1.0, nr)
    g = np.linspace(0.0, 1.0, ng)
    b = np.linspace(0.0, 1.0, nb)
    rr, gg, bb = np.meshgrid(r, g, b, indexing="ij")
    coords = np.stack([rr.ravel(), gg.ravel(), bb.ravel()], axis=1)  # (5508,3) row-major
    return _trilinear_sample(cubic_lut, coords)


def pack_u30(lut_rgb: np.ndarray, order: str = "rgb") -> np.ndarray:
    """(N,3) float[0,1] → (N,) uint32，每通道量化 10bit 打包（每个 ≤ 2^30-1）。

    order='rgb' → bits[29:20]=R,[19:10]=G,[9:0]=B；'bgr' 反之。（位序以 CLUT 调优说明为准。）
    """
    lut = np.clip(np.asarray(lut_rgb, dtype=np.float64), 0.0, 1.0)
    q = np.round(lut * HW_MAX).astype(np.uint32)
    r, g, b = q[:, 0], q[:, 1], q[:, 2]
    hi, mid, lo = (r, g, b) if order == "rgb" else (b, g, r)
    return ((hi << 20) | (mid << 10) | lo).astype(np.uint32)


def pack_cubic_to_hw(cubic_lut: np.ndarray, order: str = "rgb") -> np.ndarray:
    """立方 LUT (D,D,D,3) → 硬件 (5508,) uint32（resample + pack 一步到位）。"""
    return pack_u30(resample_to_hw_mesh(cubic_lut), order=order)


def write_lut_bin(path: str, packed: np.ndarray) -> None:
    """写出 5508 个小端 uint32（板端 fread 进 ot_isp_clut_lut.lut[]）。"""
    packed = np.asarray(packed, dtype="<u4")
    if packed.size != HW_LUT_LENGTH:
        raise ValueError(f"need {HW_LUT_LENGTH} nodes, got {packed.size}")
    packed.tofile(path)


def identity_cubic_lut(dim: int) -> np.ndarray:
    """dim³ 恒等立方 LUT (dim,dim,dim,3)（不改变颜色，用于自检/初始化）。"""
    axis = np.linspace(0.0, 1.0, dim)
    r, g, b = np.meshgrid(axis, axis, axis, indexing="ij")
    return np.stack([r, g, b], axis=-1)


if __name__ == "__main__":
    lut = identity_cubic_lut(17)
    packed = pack_cubic_to_hw(lut)
    assert packed.shape == (HW_LUT_LENGTH,) and packed.dtype == np.uint32
    assert packed.max() <= (1 << 30) - 1
    print(f"cubic 17³ → hw mesh {HW_MESH_DIMS} = {packed.size} nodes; "
          f"u32 max={packed.max()} (≤{(1 << 30) - 1})")
    print(f"sample node[3000]={packed[3000]:#010x}")
    print("10bit 打包格式已坐实；mesh 轴序/顺序待 CLUT 调优文档标定（只改常量）。")
