"""端到端 host 工具：param-net → 立方 LUT → 硬件 5508 节点 u32 → .bin。

支持训练 checkpoint + 真实场景图；不提供 checkpoint 时，恒等初始化用于结构自检。板端用法见
cotf-route-verification.md：
    板端 fread(.bin) → ot_isp_clut_lut.lut[] → isp_load_clut_lut(lut, 5508)（ISP 硬件全分辨率施加）。
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path

import cv2
import numpy as np
import torch
import torch.nn.functional as F

from models.networks.cotf import build_param_net
from models.tools.cotf_lut_pack import (
    HW_LUT_LENGTH,
    decode_paramnet_output,
    pack_cubic_to_hw,
    write_lut_bin,
)

MODELS_DIR = Path(__file__).resolve().parents[1]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lut-dim", type=int, default=17)
    ap.add_argument("--thumb-h", type=int, default=144, help="param-net 输入缩略图高（NN 只需缩略图）")
    ap.add_argument("--thumb-w", type=int, default=256)
    ap.add_argument("--out", default=str(MODELS_DIR / "weights" / "cotf_clut.bin"))
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--checkpoint", help="训练 checkpoint（不提供时使用恒等初始化）")
    ap.add_argument("--input", help="用于预测 LUT 的 RGB 图像；不提供时使用固定随机输入作结构自检")
    ap.add_argument("--down", type=int, default=8)
    ap.add_argument("--ch", type=int, default=32)
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    net = build_param_net(lut_dim=args.lut_dim, down=args.down, ch=args.ch)
    if args.checkpoint:
        checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
        net.load_state_dict(checkpoint["model"] if "model" in checkpoint else checkpoint)
    net.eval()
    if args.input:
        image = cv2.imread(args.input, cv2.IMREAD_COLOR)
        if image is None:
            raise ValueError(f"failed to read image: {args.input}")
        rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
        thumb = torch.from_numpy(rgb.transpose(2, 0, 1)).unsqueeze(0)
        thumb = F.interpolate(thumb, size=(args.thumb_h, args.thumb_w), mode="bilinear",
                              align_corners=False)
    else:
        thumb = torch.rand(1, 3, args.thumb_h, args.thumb_w)
    with torch.no_grad():
        flat = net(thumb).reshape(-1).cpu().numpy()  # (3*D^3,)

    cubic = decode_paramnet_output(flat, args.lut_dim)   # (D,D,D,3) float[0,1]
    packed = pack_cubic_to_hw(cubic)                     # (5508,) uint32
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    write_lut_bin(args.out, packed)

    source = args.input or "synthetic"
    print(f"param-net(input={source}, thumb {args.thumb_w}x{args.thumb_h}) → cubic {args.lut_dim}³ "
          f"→ hw {HW_LUT_LENGTH} nodes")
    print(f"  u32 range [{packed.min():#010x}, {packed.max():#010x}] (≤0x3fffffff)")
    print(f"  wrote {args.out} ({os.path.getsize(args.out)} bytes = {HW_LUT_LENGTH}×u32)")
    print("  板端: fread → isp_load_clut_lut(lut, 5508) → ISP 硬件全分辨率施加（零 NPU）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
