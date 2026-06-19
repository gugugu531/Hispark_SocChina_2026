"""param-net 训练闭环的 SDK-free 单元测试。"""

from __future__ import annotations

import math
import random

import cv2
import numpy as np
import torch

from models.networks.cotf import LUTApply, build_param_net
from models.trainers.cotf_paramnet import (
    PairedExposureDataset,
    TrainingProgress,
    capture_rng_state,
    compute_loss,
    discover_pairs,
    format_duration,
    restore_rng_state,
)


def test_untrained_paramnet_is_identity():
    model = build_param_net(down=2, ch=4, lut_dim=5)
    image = torch.rand(2, 3, 16, 24)
    flat = model(image)
    output = LUTApply(lut_dim=5)(image, model.as_lut(flat))
    torch.testing.assert_close(output, image, atol=2e-6, rtol=1e-5)


def test_training_loss_backpropagates():
    model = build_param_net(down=2, ch=4, lut_dim=5)
    source = torch.rand(2, 3, 16, 24)
    target = source.sqrt()
    loss, stats, output = compute_loss(
        model,
        LUTApply(lut_dim=5),
        source,
        target,
        (8, 12),
        {"pixel": 1.0, "gradient": 0.2, "smooth": 1e-4, "monotonic": 10.0},
    )
    loss.backward()
    assert loss.item() > 0
    assert stats["pixel"] > 0
    assert output.shape == source.shape
    assert model.head.weight.grad is not None
    assert torch.isfinite(model.head.weight.grad).all()


def test_paired_dataset_matches_relative_paths(tmp_path):
    input_dir = tmp_path / "input"
    target_dir = tmp_path / "target"
    (input_dir / "nested").mkdir(parents=True)
    (target_dir / "nested").mkdir(parents=True)
    image = np.full((12, 16, 3), 127, dtype=np.uint8)
    cv2.imwrite(str(input_dir / "nested" / "frame.png"), image)
    cv2.imwrite(str(target_dir / "nested" / "frame.png"), image)

    assert len(discover_pairs(input_dir, target_dir)) == 1
    sample = PairedExposureDataset(str(input_dir), str(target_dir), crop_size=8, training=False)[0]
    assert sample["input"].shape == (3, 8, 8)
    torch.testing.assert_close(sample["input"], sample["target"])


def test_rng_state_round_trip():
    state = capture_rng_state()
    expected = (random.random(), np.random.rand(), torch.rand(1))
    restore_rng_state(state)
    actual = (random.random(), np.random.rand(), torch.rand(1))
    assert actual[0] == expected[0]
    assert actual[1] == expected[1]
    torch.testing.assert_close(actual[2], expected[2])


def test_progress_estimate_and_duration_format():
    progress = TrainingProgress(total_steps=100, completed_steps=25, previous_seconds=10.0)
    elapsed, remaining = progress.estimate(25)
    assert elapsed >= 10.0
    assert remaining >= 30.0
    assert format_duration(3661) == "1h01m01s"
    assert format_duration(math.inf) == "--"
