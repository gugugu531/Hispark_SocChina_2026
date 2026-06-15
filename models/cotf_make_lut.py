"""端到端 host 演示：CoTF param-net → 立方 LUT → 硬件 5508 节点 u32 → .bin（板端可加载）。

跑通整条 NN→硬件 LUT 桥（随机权重，结构演示）。板端用法见 cotf-route-verification.md：
    板端 fread(.bin) → ot_isp_clut_lut.lut[] → isp_load_clut_lut(lut, 5508)（ISP 硬件全分辨率施加）。
"""

from __future__ import annotations

import argparse
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cotf_net import build_param_net  # noqa: E402
from cotf_lut_pack import HW_LUT_LENGTH, decode_paramnet_output, pack_cubic_to_hw, write_lut_bin  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lut-dim", type=int, default=17)
    ap.add_argument("--thumb-h", type=int, default=144, help="param-net 输入缩略图高（NN 只需缩略图）")
    ap.add_argument("--thumb-w", type=int, default=256)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "weights", "cotf_clut.bin"))
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    net = build_param_net(lut_dim=args.lut_dim)
    net.eval()
    thumb = torch.rand(1, 3, args.thumb_h, args.thumb_w)  # 代表 VPSS chn2 缩略图
    with torch.no_grad():
        flat = net(thumb).reshape(-1).cpu().numpy()  # (3*D^3,)

    cubic = decode_paramnet_output(flat, args.lut_dim)   # (D,D,D,3) float[0,1]
    packed = pack_cubic_to_hw(cubic)                     # (5508,) uint32
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    write_lut_bin(args.out, packed)

    print(f"param-net(thumb {args.thumb_w}x{args.thumb_h}) → cubic {args.lut_dim}³ "
          f"→ hw {HW_LUT_LENGTH} nodes")
    print(f"  u32 range [{packed.min():#010x}, {packed.max():#010x}] (≤0x3fffffff)")
    print(f"  wrote {args.out} ({os.path.getsize(args.out)} bytes = {HW_LUT_LENGTH}×u32)")
    print("  板端: fread → isp_load_clut_lut(lut, 5508) → ISP 硬件全分辨率施加（零 NPU）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
