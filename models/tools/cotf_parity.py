"""生成固定 NV21/NCHW 输入并比较 checkpoint、ONNX、裸 OM、AIPP OM 输出。"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import onnx
import torch
from onnx.reference import ReferenceEvaluator

from models.networks.cotf import build_param_net

WIDTH = 256
HEIGHT = 144
LUT_COUNT = 3 * 17**3


def make_rgb_pattern() -> np.ndarray:
    """覆盖暗部、渐变、饱和色和高光的确定性 RGB uint8 图。"""
    yy, xx = np.mgrid[0:HEIGHT, 0:WIDTH]
    r = np.clip(xx / (WIDTH - 1) * 255.0, 0, 255)
    g = np.clip(yy / (HEIGHT - 1) * 255.0, 0, 255)
    b = np.clip((0.55 * xx / (WIDTH - 1) + 0.45 * yy / (HEIGHT - 1)) * 255.0, 0, 255)
    rgb = np.stack([r, g, b], axis=-1)
    rgb[: HEIGHT // 3, : WIDTH // 4] *= 0.2
    rgb[: HEIGHT // 3, WIDTH // 4 : WIDTH // 2] = (255, 32, 32)
    rgb[: HEIGHT // 3, WIDTH // 2 : 3 * WIDTH // 4] = (32, 255, 32)
    rgb[: HEIGHT // 3, 3 * WIDTH // 4 :] = (32, 32, 255)
    rgb[-HEIGHT // 4 :, -WIDTH // 4 :] = 255
    return np.rint(rgb).astype(np.uint8)


def rgb_to_nv21(rgb: np.ndarray) -> bytes:
    """BT.601 limited-range RGB→NV21，色度按 2x2 平均。"""
    x = rgb.astype(np.float64)
    r, g, b = x[..., 0], x[..., 1], x[..., 2]
    y = 16.0 + (65.738 * r + 129.057 * g + 25.064 * b) / 256.0
    u = 128.0 + (-37.945 * r - 74.494 * g + 112.439 * b) / 256.0
    v = 128.0 + (112.439 * r - 94.154 * g - 18.285 * b) / 256.0
    y8 = np.clip(np.rint(y), 0, 255).astype(np.uint8)
    u420 = np.rint(u.reshape(HEIGHT // 2, 2, WIDTH // 2, 2).mean(axis=(1, 3)))
    v420 = np.rint(v.reshape(HEIGHT // 2, 2, WIDTH // 2, 2).mean(axis=(1, 3)))
    vu = np.empty((HEIGHT // 2, WIDTH), dtype=np.uint8)
    vu[:, 0::2] = np.clip(v420, 0, 255).astype(np.uint8)
    vu[:, 1::2] = np.clip(u420, 0, 255).astype(np.uint8)
    return y8.tobytes() + vu.tobytes()


def aipp_nv21_to_rgb(nv21: bytes) -> np.ndarray:
    """精确复现配置中的整数 BT.601 limited CSC 与 NV21 VU 顺序。"""
    raw = np.frombuffer(nv21, dtype=np.uint8)
    y = raw[: WIDTH * HEIGHT].reshape(HEIGHT, WIDTH).astype(np.int32)
    vu = raw[WIDTH * HEIGHT :].reshape(HEIGHT // 2, WIDTH)
    v = np.repeat(np.repeat(vu[:, 0::2], 2, axis=0), 2, axis=1).astype(np.int32)
    u = np.repeat(np.repeat(vu[:, 1::2], 2, axis=0), 2, axis=1).astype(np.int32)
    c, d, e = y - 16, u - 128, v - 128
    r = (298 * c + 409 * e + 128) >> 8
    g = (298 * c - 100 * d - 208 * e + 128) >> 8
    b = (298 * c + 516 * d + 128) >> 8
    return np.clip(np.stack([r, g, b], axis=-1), 0, 255).astype(np.uint8)


def metrics(reference: np.ndarray, actual: np.ndarray) -> dict[str, float]:
    diff = actual.astype(np.float64) - reference.astype(np.float64)
    denom = np.linalg.norm(reference.astype(np.float64)) * np.linalg.norm(actual.astype(np.float64))
    return {
        "mae": float(np.mean(np.abs(diff))),
        "rmse": float(np.sqrt(np.mean(diff**2))),
        "max_abs": float(np.max(np.abs(diff))),
        "cosine": float(np.dot(reference.ravel(), actual.ravel()) / denom) if denom else 1.0,
        "over_1e-2_pct": float(np.mean(np.abs(diff) > 1e-2) * 100.0),
    }


def prepare(args: argparse.Namespace) -> None:
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    rgb = make_rgb_pattern()
    nv21 = rgb_to_nv21(rgb)
    aipp_rgb = aipp_nv21_to_rgb(nv21)
    nchw = np.transpose(aipp_rgb.astype(np.float32) / 255.0, (2, 0, 1))[None]

    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    model = build_param_net(down=8, ch=32, lut_dim=17)
    model.load_state_dict(checkpoint["model"])
    model.eval()
    with torch.no_grad():
        pytorch_out = model(torch.from_numpy(nchw)).numpy().reshape(-1).astype(np.float32)

    onnx_model = onnx.load(args.onnx)
    evaluator = ReferenceEvaluator(onnx_model)
    input_type = np.float16 if "fp16" in Path(args.onnx).stem else np.float32
    onnx_out = evaluator.run(None, {"input": nchw.astype(input_type)})[0].reshape(-1).astype(np.float32)

    (out / "input_nv21.bin").write_bytes(nv21)
    nchw.astype("<f2").tofile(out / "input_nchw_fp16.bin")
    rgb.tofile(out / "source_rgb_u8.bin")
    aipp_rgb.tofile(out / "aipp_rgb_u8.bin")
    pytorch_out.astype("<f4").tofile(out / "pytorch.f32")
    onnx_out.astype("<f4").tofile(out / "onnx.f32")
    report = {
        "shape": [1, 3, HEIGHT, WIDTH],
        "nv21_bytes": len(nv21),
        "nchw_fp16_bytes": nchw.size * 2,
        "pytorch_vs_onnx": metrics(pytorch_out, onnx_out),
    }
    (out / "host_report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


def compare(args: argparse.Namespace) -> None:
    root = Path(args.dir)
    arrays = {
        name: np.fromfile(path, dtype="<f4")
        for name, path in {
            "pytorch": root / "pytorch.f32",
            "onnx": root / "onnx.f32",
            "bare_om": root / "bare_om.f32",
            "aipp_om": root / "aipp_om.f32",
        }.items()
    }
    for name, array in arrays.items():
        if array.size != LUT_COUNT:
            raise ValueError(f"{name}: expected {LUT_COUNT} floats, got {array.size}")
    report = {
        "pytorch_vs_onnx": metrics(arrays["pytorch"], arrays["onnx"]),
        "pytorch_vs_bare_om": metrics(arrays["pytorch"], arrays["bare_om"]),
        "bare_om_vs_aipp_om": metrics(arrays["bare_om"], arrays["aipp_om"]),
        "pytorch_vs_aipp_om": metrics(arrays["pytorch"], arrays["aipp_om"]),
    }
    (root / "parity_report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    prep = sub.add_parser("prepare")
    prep.add_argument("--checkpoint", required=True)
    prep.add_argument("--onnx", required=True)
    prep.add_argument("--out-dir", required=True)
    prep.set_defaults(func=prepare)
    comp = sub.add_parser("compare")
    comp.add_argument("--dir", required=True)
    comp.set_defaults(func=compare)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
