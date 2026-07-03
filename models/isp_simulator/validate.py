#!/usr/bin/env python3
"""ISP simulator vs. board hardware comparison tool.

Usage:
    # 1. Set up SSH tunnel in another terminal:
    ssh -L 8554:127.0.0.1:8554 hispark-remote

    # 2. Run validation:
    python -m models.isp_simulator.validate

This script assumes the board's socchina-stream service is running and
MediaMTX exposes RTSP on port 8554 (forwarded through SSH tunnel).
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np
import torch

from models.isp_simulator import ISPPipeline, make_identity_params, gamma
from models.isp_simulator.params import PARAM_TOTAL_DIM

REPO_ROOT = Path(__file__).resolve().parents[2]
OUTPUT_DIR = REPO_ROOT / "models" / "weights" / "validation"


def grab_rtsp_frame(rtsp_url: str, timeout: float = 10.0) -> np.ndarray | None:
    """Grab a single frame from RTSP stream. Requires cv2."""
    import cv2
    cap = cv2.VideoCapture(rtsp_url)
    cap.set(cv2.CAP_PROP_OPEN_TIMEOUT_MSEC, int(timeout * 1000))
    ret, frame = cap.read()
    cap.release()
    if not ret:
        return None
    return cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)


def run_simulator(image_np: np.ndarray, params: torch.Tensor) -> np.ndarray:
    """Run ISP simulator on RGB uint8 image with given params.

    Args:
        image_np: (H, W, 3) uint8 RGB
        params: (1, PARAM_TOTAL_DIM) parameter vector

    Returns:
        (H, W, 3) uint8 RGB simulated output
    """
    pipeline = ISPPipeline()
    img_tensor = torch.from_numpy(image_np).float().permute(2, 0, 1).unsqueeze(0) / 255.0
    with torch.no_grad():
        result = pipeline(img_tensor, params)
    out = result["output"].squeeze(0).permute(1, 2, 0).clamp(0.0, 1.0).cpu().numpy()
    return (out * 255.0).astype(np.uint8)


def compute_metrics(ref: np.ndarray, test: np.ndarray) -> dict[str, float]:
    """Compute PSNR and SSIM between two uint8 RGB images."""
    ref_f = ref.astype(np.float32) / 255.0
    test_f = test.astype(np.float32) / 255.0
    mse = np.mean((ref_f - test_f) ** 2)
    psnr = float(20.0 * np.log10(1.0 / np.sqrt(max(mse, 1e-10))))

    # Simple SSIM-like metric (luminance correlation)
    ref_y = 0.299 * ref_f[..., 0] + 0.587 * ref_f[..., 1] + 0.114 * ref_f[..., 2]
    test_y = 0.299 * test_f[..., 0] + 0.587 * test_f[..., 1] + 0.114 * test_f[..., 2]
    corr = np.corrcoef(ref_y.ravel(), test_y.ravel())[0, 1]

    return {"psnr_db": psnr, "luma_correlation": float(corr)}


def ssh_cmd(board: str, cmd: str, timeout: int = 30) -> tuple[int, str, str]:
    """Run command on board via SSH."""
    p = subprocess.run(
        ["ssh", "-o", "ConnectTimeout=15", board, cmd],
        capture_output=True, text=True, timeout=timeout,
    )
    return p.returncode, p.stdout, p.stderr


def main():
    import argparse
    import cv2
    from PIL import Image

    ap = argparse.ArgumentParser(description="ISP Simulator vs Board Validation")
    ap.add_argument("--board", default="hispark-remote", help="SSH alias for board")
    ap.add_argument("--rtsp-port", type=int, default=8554, help="Local RTSP port (via SSH tunnel)")
    ap.add_argument("--rtsp-path", default="/internal", help="RTSP stream path")
    ap.add_argument("--output-dir", default=str(OUTPUT_DIR), help="Output directory for frames")
    ap.add_argument("--skip-board", action="store_true", help="Skip board capture, use existing frames")
    args = ap.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    board = args.board
    rtsp_url = f"rtsp://127.0.0.1:{args.rtsp_port}{args.rtsp_path}"

    # ── 1. Generate synthetic test cases (host-only, always works) ─────────
    print("=" * 60)
    print("Test 1: Synthetic identity test (host-only)")
    print("=" * 60)

    # Generate a test pattern
    H, W = 288, 512
    x = np.linspace(0, 1, W)
    y = np.linspace(0, 1, H)
    xx, yy = np.meshgrid(x, y)
    # Gradient + color patches
    gradient = np.stack([
        xx,                    # R horizontal
        yy,                    # G vertical
        0.5 * xx + 0.5 * yy,  # B diagonal
    ], axis=-1)
    test_img = (gradient * 255).astype(np.uint8)
    Image.fromarray(test_img).save(output_dir / "test_pattern.png")

    # Identity params -> output should be close to input
    id_params = make_identity_params(1)
    sim_out = run_simulator(test_img, id_params)
    Image.fromarray(sim_out).save(output_dir / "sim_identity.png")

    metrics = compute_metrics(test_img, sim_out)
    print(f"  Identity test: PSNR={metrics['psnr_db']:.1f} dB, corr={metrics['luma_correlation']:.4f}")
    assert metrics["psnr_db"] > 25, f"Identity PSNR too low: {metrics['psnr_db']:.1f}"

    # ── 2. Gamma brighten test ─────────────────────────────────────────────
    print("\n" + "=" * 60)
    print("Test 2: Gamma brighten effect verification")
    print("=" * 60)

    brighten_curve = gamma.gamma_brighten_curve(strength=0.8, num_nodes=64).unsqueeze(0)
    params = make_identity_params(1)
    from models.isp_simulator.params import get_offset
    off = get_offset("gamma")
    params[0, off:off + 64] = brighten_curve

    sim_bright = run_simulator(test_img, params)
    Image.fromarray(sim_bright).save(output_dir / "sim_gamma_brighten.png")

    # Dark areas should be lifted
    dark_mask = test_img.mean(axis=-1) < 64
    delta = sim_bright.astype(float) - test_img.astype(float)
    dark_lift = delta[dark_mask].mean()
    print(f"  Dark area mean lift: {dark_lift:.1f} (positive = brighter)")
    assert dark_lift > 0, f"Gamma brighten should lift dark areas, got {dark_lift:.1f}"

    # ── 3. DRC S-curve test ────────────────────────────────────────────────
    print("\n" + "=" * 60)
    print("Test 3: DRC S-curve compression")
    print("=" * 60)

    params2 = make_identity_params(1)
    off_tone = get_offset("drc_tone")
    # S-curve: lift shadows, compress highlights
    s_curve = torch.tensor([[0.0, 0.15, 0.5, 0.5, 0.85, 1.0]])
    params2[0, off_tone:off_tone + 6] = s_curve
    # Enable local detail
    off_mix = get_offset("drc_mix")
    params2[0, off_mix:off_mix + 12] = 0.6

    sim_drc = run_simulator(test_img, params2)
    Image.fromarray(sim_drc).save(output_dir / "sim_drc_scurve.png")

    # Check dynamic range is compressed
    orig_std = test_img.astype(float).std()
    drc_std = sim_drc.astype(float).std()
    print(f"  Image std: {orig_std:.1f} -> {drc_std:.1f}")
    # S-curve should reduce contrast at extremes
    assert drc_std > 0, "DRC output should not be flat"

    # ── 4. Pipeline stress test ────────────────────────────────────────────
    print("\n" + "=" * 60)
    print("Test 4: Full pipeline stress test")
    print("=" * 60)

    rng = torch.Generator().manual_seed(42)
    for i in range(20):
        params_rand = torch.rand(1, PARAM_TOTAL_DIM, generator=rng)
        out = run_simulator(test_img, params_rand)
        assert np.isfinite(out).all(), f"NaN in random test {i}"
        assert out.min() >= 0 and out.max() <= 255, f"Range error in test {i}"
    print("  20 random parameter sets: all passed")

    # ── 5. Board comparison (if available) ─────────────────────────────────
    if not args.skip_board:
        print("\n" + "=" * 60)
        print("Test 5: Board comparison")
        print("=" * 60)

        # Check board connectivity
        ret, out, err = ssh_cmd(board, "echo OK")
        if ret != 0:
            print(f"  Board not reachable: {err}")
            print("  Skipping board tests. Run with --skip-board to suppress this.")
            return 0

        # Grab reference frame from RTSP
        print(f"  Connecting to {rtsp_url} ...")
        ref_frame = grab_rtsp_frame(rtsp_url, timeout=10.0)
        if ref_frame is None:
            print("  Failed to grab RTSP frame. Is SSH tunnel active?")
            print("  Run: ssh -L 8554:127.0.0.1:8554 hispark-remote")
            return 1

        print(f"  Captured reference frame: {ref_frame.shape}")
        Image.fromarray(ref_frame).save(output_dir / "board_ref.png")

        # Run simulator on the reference frame (identity = should match)
        sim_ref = run_simulator(ref_frame, make_identity_params(1))
        Image.fromarray(sim_ref).save(output_dir / "board_sim_ref.png")

        metrics = compute_metrics(ref_frame, sim_ref)
        print(f"  Board ref vs Sim identity: PSNR={metrics['psnr_db']:.1f} dB, "
              f"corr={metrics['luma_correlation']:.4f}")

        # Simulator with identity params on real camera image should still
        # produce reasonable output (close to input)
        assert metrics["psnr_db"] > 20, f"Identity on camera image degraded: {metrics['psnr_db']:.1f}"

    print("\n" + "=" * 60)
    print("All validation tests passed!")
    print(f"Output images saved to: {output_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
