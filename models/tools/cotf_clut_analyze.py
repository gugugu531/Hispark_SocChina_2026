"""离线分析 SS928 ISP CLUT 硬件默认表（mesh 几何标定辅助）。

输入：板端 `test_cotf_live --dumplut <bin>` 落盘的 5508×u32 默认 CLUT 表
（每 u32 = 3×10bit RGB，bits[29:20]=R/[19:10]=G/[9:0]=B，格式经回读核对可信）。

用途：在没有厂商《ISP CLUT 调优说明》的情况下，从默认表的结构反推节点 linear-index↔(r,g,b)
几何映射。当前结论（见 cotf-route-verification.md「板端联机点亮 / mesh 标定」）：
  * 外层轴 = 17（stride 324 = 18²），由 lag-324 显著更平滑确认；
  * 内层 18×18 平面存在嵌套 period-2 交织，单纯去交织只能把 TV 1076→814（未干净收敛）；
  * 灰轴节点 (R==G==B) 多达 617 个（远超均匀 RGB 立方的 ~17）⇒ **CLUT 不是均匀立方 RGB 点阵**，
    而是厂商特定的非均匀/感知点阵。故任意 3D 色彩 LUT 的精确落地需厂商文档；
    本仓库当前用「几何无关」法（读默认表→对输出值叠 tone 曲线）覆盖曝光/色调类校正。

用法：python -m models.tools.cotf_clut_analyze <clut_default.bin>
"""
from __future__ import annotations

import sys
import itertools

import numpy as np


def load(path: str) -> np.ndarray:
    v = np.fromfile(path, dtype="<u4")
    if v.size != 5508:
        raise ValueError(f"expected 5508 nodes, got {v.size}")
    r = (v >> 20) & 0x3FF
    g = (v >> 10) & 0x3FF
    b = v & 0x3FF
    return np.stack([r, g, b], axis=1).astype(float)


def tv(a: np.ndarray) -> float:
    """三轴平均绝对一阶差之和（越小越平滑）。"""
    return float(sum(np.abs(np.diff(a, axis=ax)).mean() for ax in range(3)))


def lag_diff(rgb: np.ndarray, lags) -> None:
    print("mean|diff| at lag L (小 ⇒ 该 lag 是平滑/内层步进):")
    for L in lags:
        if L < len(rgb):
            d = np.abs(rgb[L:] - rgb[:-L])
            print(f"  lag {L:4d}: R={d[:,0].mean():6.1f} G={d[:,1].mean():6.1f} B={d[:,2].mean():6.1f}")


def reshape_search(rgb: np.ndarray) -> None:
    def deint(n):
        return np.array(list(range(0, n, 2)) + list(range(1, n, 2)))

    base = rgb.reshape(17, 18, 18, 3)
    o = deint(18)
    cfgs = {
        "plain (17,18,18)": base,
        "deint ax2": base[:, :, o, :],
        "deint ax1": base[:, o, :, :],
        "deint ax1+ax2": base[:, o, :, :][:, :, o, :],
    }
    for dims in itertools.permutations([17, 18, 18]):
        cfgs[f"{dims} plain"] = rgb.reshape(*dims, 3)
    print("\nreshape / 去交织候选 TV（越小越平滑）:")
    for k, a in sorted(cfgs.items(), key=lambda kv: tv(kv[1])):
        print(f"  {k:22s} TV={tv(a):.1f}")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    rgb = load(sys.argv[1])
    r, g, b = rgb[:, 0], rgb[:, 1], rgb[:, 2]
    gray = int(((r == g) & (g == b)).sum())
    print(f"nodes=5508 nonzero={int((rgb.sum(1) != 0).sum())} gray(R==G==B)={gray}"
          f"  (均匀立方应 ~17 ⇒ {gray} 远超 ⇒ 非均匀点阵)")
    lag_diff(rgb, [1, 2, 3, 17, 18, 19, 289, 306, 323, 324, 342])
    reshape_search(rgb)
    print("\n结论：外层 17(stride 324) 已确认；内层非简单 row-major 且点阵非均匀立方，"
          "精确 index↔(r,g,b) 需厂商《ISP CLUT 调优说明》。曝光/色调类校正可走几何无关法。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
