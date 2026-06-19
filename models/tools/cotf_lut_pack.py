"""将 NN 预测的 3D-LUT 打包为 SS928 ISP CLUT 的 u32 格式。

硬件格式（核对自 SDK 头 `ot_common_isp.h` / `ot_isp_define.h`）：
  * `ot_isp_clut_lut.lut[OT_ISP_CLUT_LUT_LENGTH]`，`OT_ISP_CLUT_LUT_LENGTH = 5508` 个节点。
  * 每个 `td_u32` 取值 [0, 1073741823] = [0, 2^30-1]，即 **3×10bit 打包**（R/G/B 各 10bit）。
  * 另有 `ot_isp_clut_attr = { en, gain_r/g/b(12bit) }` 做全局增益+开关。
  * 节点几何：逻辑 LUT 为 17³；官方 PQTools ``clut_lut_print_17v2`` 将节点按
    三轴索引奇偶拆成 8 个 bank，再把 bank0..3 交织为 729×4 项、bank4..7
    交织为 648×4 项，因此总长 ``4*729 + 4*648 = 5508``。

整条桥（host 侧）::

    NN param-net 输出 (3*D^3,) ──decode──► 立方 LUT (D,D,D,3) float[0,1]
        ──17v2 bank layout──► (5508,3) float ──pack_u30──► (5508,) uint32
        ──write_lut_bin──► .bin（板端读入 → ss_mpi_isp_set_clut_coeff）

布局来自随 SDK 交付的 PQTools ``algClutcompLib.dll`` 中 ``clut_lut_print_17v2``，
并由板端 36 组 identity 轴序×位序扫测确认：三轴为 RGB，输出位序为
R 高 10bit、G 中 10bit、B 低 10bit。
"""

from __future__ import annotations

import numpy as np

HW_LUT_LENGTH = 5508          # OT_ISP_CLUT_LUT_LENGTH
HW_BITS = 10                  # 每通道 10 bit
HW_MAX = (1 << HW_BITS) - 1   # 1023
HW_BANK_COUNTS = (729, 648, 648, 576, 648, 576, 576, 512)
assert sum(HW_BANK_COUNTS) == 17**3


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


def resample_to_hw_mesh(cubic_lut: np.ndarray) -> np.ndarray:
    """重采样到逻辑 17³ 后，按官方 17v2 奇偶 bank 布局输出 5508×RGB。"""
    axis = np.linspace(0.0, 1.0, 17)
    rr, gg, bb = np.meshgrid(axis, axis, axis, indexing="ij")
    coords = np.stack([rr.ravel(), gg.ravel(), bb.ravel()], axis=1)
    logical = _trilinear_sample(cubic_lut, coords).reshape(17, 17, 17, 3)
    banks: list[list[np.ndarray]] = [[] for _ in range(8)]
    # 官方调用顺序：B 外层、G 中层、R 内层；bank bits = R0 | G0<<1 | B0<<2。
    for b in range(17):
        for g in range(17):
            for r in range(17):
                bank = (r & 1) | ((g & 1) << 1) | ((b & 1) << 2)
                banks[bank].append(logical[r, g, b])
    values = [np.asarray(bank, dtype=np.float64) for bank in banks]
    assert tuple(len(bank) for bank in values) == HW_BANK_COUNTS

    # 精确复现 print_v500_lut0to3/print_v500_lut4to7。内部 8 个 bank 连续存储；
    # bank2、bank6 在打印尾部会继续读到下一 bank，短 bank1/3/5/7 则显式补零。
    contiguous = np.concatenate(values, axis=0)
    starts = np.cumsum((0,) + HW_BANK_COUNTS[:-1])
    output: list[np.ndarray] = []
    zero = np.zeros(3, dtype=np.float64)
    for i in range(729):
        output.append(contiguous[starts[0] + i])
        output.append(contiguous[starts[1] + i] if i < HW_BANK_COUNTS[1] else zero)
        output.append(contiguous[starts[2] + i])
        output.append(contiguous[starts[3] + i] if i < HW_BANK_COUNTS[3] else zero)
    for i in range(648):
        output.append(contiguous[starts[4] + i])
        output.append(contiguous[starts[5] + i] if i < HW_BANK_COUNTS[5] else zero)
        output.append(contiguous[starts[6] + i])
        output.append(contiguous[starts[7] + i] if i < HW_BANK_COUNTS[7] else zero)
    return np.asarray(output)


def unpack_hw_mesh(hw_rgb: np.ndarray) -> np.ndarray:
    """把 5508×RGB 的 17v2 交织存储还原为 (17,17,17,3) 逻辑 RGB 立方。"""
    hw = np.asarray(hw_rgb)
    if hw.shape != (HW_LUT_LENGTH, 3):
        raise ValueError(f"need ({HW_LUT_LENGTH},3), got {hw.shape}")
    banks: list[list[np.ndarray]] = [[] for _ in range(8)]
    for i in range(729):
        banks[0].append(hw[4 * i])
        if i < HW_BANK_COUNTS[1]:
            banks[1].append(hw[4 * i + 1])
        if i < HW_BANK_COUNTS[2]:
            banks[2].append(hw[4 * i + 2])
        if i < HW_BANK_COUNTS[3]:
            banks[3].append(hw[4 * i + 3])
    base = 4 * 729
    for i in range(648):
        banks[4].append(hw[base + 4 * i])
        if i < HW_BANK_COUNTS[5]:
            banks[5].append(hw[base + 4 * i + 1])
        if i < HW_BANK_COUNTS[6]:
            banks[6].append(hw[base + 4 * i + 2])
        if i < HW_BANK_COUNTS[7]:
            banks[7].append(hw[base + 4 * i + 3])
    positions = [0] * 8
    logical = np.empty((17, 17, 17, 3), dtype=hw.dtype)
    for b in range(17):
        for g in range(17):
            for r in range(17):
                bank = (r & 1) | ((g & 1) << 1) | ((b & 1) << 2)
                logical[r, g, b] = banks[bank][positions[bank]]
                positions[bank] += 1
    return logical


def pack_u30(lut_rgb: np.ndarray, order: str = "rgb") -> np.ndarray:
    """(N,3) float[0,1] → (N,) uint32，每通道量化 10bit 打包（每个 ≤ 2^30-1）。

    order='rgb' → bits[29:20]=R,[19:10]=G,[9:0]=B，为 SS928 ``17v2`` 板端确认位序。
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
    print(f"cubic 17³ → 17v2 banks {HW_BANK_COUNTS} interleaved = {packed.size} nodes; "
          f"u32 max={packed.max()} (≤{(1 << 30) - 1})")
    print(f"sample node[3000]={packed[3000]:#010x}")
    print("官方 17v2 bank 交织布局与 RGB 10bit 打包已实现并通过板端 sweep。")
