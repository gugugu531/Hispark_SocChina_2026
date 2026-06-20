"""训练 AICore 友好的 CoTF-inspired param-net。

数据布局：

    <input-dir>/<relative-name>.png
    <target-dir>/<relative-name>.png

输入与目标按相对路径配对。网络只看 ``thumb_width × thumb_height`` 缩略图，预测 17³
3D-LUT；训练时在主机 GPU 上以可微三线性采样把 LUT 施加到图像 crop。部署 ONNX 只导出
param-net，不包含 ``grid_sample``。
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import random
import time
from pathlib import Path
from typing import Any

import cv2
import numpy as np
import torch
import torch.nn.functional as F
import yaml
from torch import nn
from torch.utils.data import DataLoader, Dataset

from models.networks.cotf import LUTApply, build_param_net

IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff"}


def seed_everything(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)


def capture_rng_state() -> dict[str, Any]:
    """捕获断点续训需要的 Python/NumPy/PyTorch RNG 状态。"""
    state: dict[str, Any] = {
        "python": random.getstate(),
        "numpy": np.random.get_state(),
        "torch": torch.get_rng_state(),
    }
    if torch.cuda.is_available():
        state["cuda"] = torch.cuda.get_rng_state_all()
    return state


def restore_rng_state(state: dict[str, Any] | None) -> None:
    """恢复 checkpoint 中的 RNG 状态；兼容没有该字段的旧 checkpoint。"""
    if not state:
        return
    random.setstate(state["python"])
    np.random.set_state(state["numpy"])
    torch_state = state["torch"]
    if not isinstance(torch_state, torch.Tensor):
        torch_state = torch.as_tensor(torch_state, dtype=torch.uint8)
    torch.set_rng_state(torch_state.detach().to(device="cpu", dtype=torch.uint8))
    if torch.cuda.is_available() and "cuda" in state:
        cuda_states = []
        for cuda_state in state["cuda"]:
            if not isinstance(cuda_state, torch.Tensor):
                cuda_state = torch.as_tensor(cuda_state, dtype=torch.uint8)
            cuda_states.append(cuda_state.detach().to(device="cpu", dtype=torch.uint8))
        torch.cuda.set_rng_state_all(cuda_states)


def format_duration(seconds: float) -> str:
    """把秒数格式化为紧凑 ETA。"""
    if not math.isfinite(seconds) or seconds < 0:
        return "--"
    seconds = int(round(seconds))
    hours, remainder = divmod(seconds, 3600)
    minutes, secs = divmod(remainder, 60)
    if hours:
        return f"{hours:d}h{minutes:02d}m{secs:02d}s"
    if minutes:
        return f"{minutes:d}m{secs:02d}s"
    return f"{secs:d}s"


class TrainingProgress:
    """按全局 batch 进度估算剩余时长和完成时间。"""

    def __init__(self, total_steps: int, completed_steps: int = 0,
                 previous_seconds: float = 0.0) -> None:
        self.total_steps = total_steps
        self.completed_steps = completed_steps
        self.previous_seconds = previous_seconds
        self.started = time.perf_counter()

    @property
    def elapsed_seconds(self) -> float:
        return self.previous_seconds + (time.perf_counter() - self.started)

    def estimate(self, completed_steps: int) -> tuple[float, float]:
        elapsed = self.elapsed_seconds
        if completed_steps <= 0:
            return elapsed, math.inf
        rate = elapsed / completed_steps
        remaining = max(self.total_steps - completed_steps, 0) * rate
        return elapsed, remaining

    def finish_time(self, remaining_seconds: float) -> str:
        if not math.isfinite(remaining_seconds):
            return "--"
        finish = dt.datetime.now().astimezone() + dt.timedelta(seconds=remaining_seconds)
        return finish.strftime("%Y-%m-%d %H:%M:%S %Z")


def discover_pairs(input_dir: Path, target_dir: Path) -> list[tuple[Path, Path]]:
    """按相对路径发现严格匹配的成对图像。"""
    inputs = sorted(p for p in input_dir.rglob("*") if p.suffix.lower() in IMAGE_SUFFIXES)
    pairs = [(path, target_dir / path.relative_to(input_dir)) for path in inputs]
    missing = [target for _, target in pairs if not target.is_file()]
    if missing:
        preview = ", ".join(str(path) for path in missing[:3])
        raise FileNotFoundError(f"{len(missing)} targets are missing; first: {preview}")
    if not pairs:
        raise ValueError(f"no images found under {input_dir}")
    return pairs


def _read_rgb(path: Path) -> np.ndarray:
    image = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError(f"failed to read image: {path}")
    return cv2.cvtColor(image, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0


class PairedExposureDataset(Dataset[dict[str, Any]]):
    """轻量成对曝光数据集，不依赖 BasicSR 或机器专用配置。"""

    def __init__(self, input_dir: str, target_dir: str, crop_size: int, training: bool) -> None:
        self.pairs = discover_pairs(Path(input_dir), Path(target_dir))
        self.crop_size = crop_size
        self.training = training

    def __len__(self) -> int:
        return len(self.pairs)

    def __getitem__(self, index: int) -> dict[str, Any]:
        input_path, target_path = self.pairs[index]
        source = _read_rgb(input_path)
        target = _read_rgb(target_path)
        if source.shape != target.shape:
            raise ValueError(f"pair shape mismatch: {input_path} {source.shape} vs "
                             f"{target_path} {target.shape}")

        h, w = source.shape[:2]
        if min(h, w) < self.crop_size:
            scale = self.crop_size / min(h, w)
            size = (max(self.crop_size, round(w * scale)), max(self.crop_size, round(h * scale)))
            source = cv2.resize(source, size, interpolation=cv2.INTER_AREA)
            target = cv2.resize(target, size, interpolation=cv2.INTER_AREA)
            h, w = source.shape[:2]

        if self.training:
            top = random.randint(0, h - self.crop_size)
            left = random.randint(0, w - self.crop_size)
        else:
            top = (h - self.crop_size) // 2
            left = (w - self.crop_size) // 2
        source = source[top:top + self.crop_size, left:left + self.crop_size]
        target = target[top:top + self.crop_size, left:left + self.crop_size]

        if self.training:
            if random.random() < 0.5:
                source, target = source[:, ::-1], target[:, ::-1]
            rotations = random.randrange(4)
            if rotations:
                source = np.rot90(source, rotations)
                target = np.rot90(target, rotations)

        def to_tensor(image: np.ndarray) -> torch.Tensor:
            return torch.from_numpy(np.ascontiguousarray(image.transpose(2, 0, 1)))

        return {
            "input": to_tensor(source),
            "target": to_tensor(target),
            "name": str(input_path.name),
        }


def lut_regularization(lut: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
    """返回 LUT 平滑项和单调性违例项。"""
    smooth = lut.new_zeros(())
    monotonic = lut.new_zeros(())
    for axis in (2, 3, 4):
        diff = torch.diff(lut, dim=axis)
        smooth = smooth + diff.square().mean()
        monotonic = monotonic + F.relu(-diff).mean()
    return smooth, monotonic


def image_gradient_loss(output: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
    dx_out, dx_target = output[..., 1:] - output[..., :-1], target[..., 1:] - target[..., :-1]
    dy_out = output[..., 1:, :] - output[..., :-1, :]
    dy_target = target[..., 1:, :] - target[..., :-1, :]
    return F.l1_loss(dx_out, dx_target) + F.l1_loss(dy_out, dy_target)


def psnr(output: torch.Tensor, target: torch.Tensor) -> float:
    mse = F.mse_loss(output, target).item()
    return -10.0 * math.log10(max(mse, 1e-12))


def compute_loss(model: nn.Module, lut_apply: LUTApply, source: torch.Tensor,
                 target: torch.Tensor, thumb_size: tuple[int, int],
                 weights: dict[str, float]) -> tuple[torch.Tensor, dict[str, float], torch.Tensor]:
    thumb = F.interpolate(source, size=thumb_size, mode="bilinear", align_corners=False)
    flat = model(thumb)
    lut = model.as_lut(flat)  # type: ignore[attr-defined]
    output = lut_apply(source, lut).clamp(0.0, 1.0)
    pixel = F.l1_loss(output, target)
    gradient = image_gradient_loss(output, target)
    smooth, monotonic = lut_regularization(lut)
    total = (weights["pixel"] * pixel + weights["gradient"] * gradient +
             weights["smooth"] * smooth + weights["monotonic"] * monotonic)
    stats = {
        "loss": total.item(),
        "pixel": pixel.item(),
        "gradient": gradient.item(),
        "smooth": smooth.item(),
        "monotonic": monotonic.item(),
    }
    return total, stats, output


@torch.no_grad()
def validate(model: nn.Module, lut_apply: LUTApply, loader: DataLoader,
             device: torch.device, thumb_size: tuple[int, int],
             weights: dict[str, float]) -> dict[str, float]:
    model.eval()
    sums = {"loss": 0.0, "psnr": 0.0}
    count = 0
    for batch in loader:
        source = batch["input"].to(device, non_blocking=True)
        target = batch["target"].to(device, non_blocking=True)
        _, stats, output = compute_loss(model, lut_apply, source, target, thumb_size, weights)
        batch_size = source.shape[0]
        sums["loss"] += stats["loss"] * batch_size
        sums["psnr"] += psnr(output, target) * batch_size
        count += batch_size
    model.train()
    return {key: value / max(count, 1) for key, value in sums.items()}


def save_checkpoint(path: Path, model: nn.Module, optimizer: torch.optim.Optimizer,
                    scheduler: torch.optim.lr_scheduler.LRScheduler, scaler: Any, epoch: int,
                    args: argparse.Namespace, metrics: dict[str, float], best_score: float,
                    training_seconds: float) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    torch.save({
        "model": model.state_dict(),
        "optimizer": optimizer.state_dict(),
        "scheduler": scheduler.state_dict(),
        "scaler": scaler.state_dict(),
        "rng_state": capture_rng_state(),
        "epoch": epoch,
        "args": vars(args),
        "metrics": metrics,
        "best_score": best_score,
        "training_seconds": training_seconds,
    }, path)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", help="YAML 训练配置；显式命令行参数优先于配置文件")
    parser.add_argument("--train-input")
    parser.add_argument("--train-target")
    parser.add_argument("--val-input")
    parser.add_argument("--val-target")
    parser.add_argument("--output-dir", default="models/weights/cotf_paramnet_train")
    parser.add_argument("--epochs", type=int, default=100)
    parser.add_argument("--batch-size", type=int, default=4)
    parser.add_argument("--crop-size", type=int, default=384)
    parser.add_argument("--thumb-width", type=int, default=256)
    parser.add_argument("--thumb-height", type=int, default=144)
    parser.add_argument("--lut-dim", type=int, default=17)
    parser.add_argument("--channels", type=int, default=32)
    parser.add_argument("--down", type=int, default=8)
    parser.add_argument("--lr", type=float, default=4e-4)
    parser.add_argument("--weight-decay", type=float, default=0.0)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--seed", type=int, default=1024)
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    parser.add_argument("--amp", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--resume")
    parser.add_argument("--save-every", type=int, default=10)
    parser.add_argument("--log-every", type=int, default=20,
                        help="每多少个训练 batch 输出一次进度；0 表示仅输出 epoch 汇总")
    parser.add_argument("--pixel-weight", type=float, default=1.0)
    parser.add_argument("--gradient-weight", type=float, default=0.2)
    parser.add_argument("--smooth-weight", type=float, default=1e-4)
    parser.add_argument("--monotonic-weight", type=float, default=10.0)
    return parser


def load_config_defaults(path: str, parser: argparse.ArgumentParser) -> dict[str, Any]:
    """读取与 argparse dest 同名的扁平 YAML 配置，并拒绝静默拼错的键。"""
    with open(path, encoding="utf-8") as file:
        payload = yaml.safe_load(file) or {}
    if not isinstance(payload, dict):
        raise ValueError(f"config root must be a mapping: {path}")
    normalized = {str(key).replace("-", "_"): value for key, value in payload.items()}
    allowed = {action.dest for action in parser._actions if action.dest != "help"}
    unknown = sorted(set(normalized) - allowed)
    if unknown:
        raise ValueError(f"unknown config keys in {path}: {', '.join(unknown)}")
    normalized["config"] = path
    return normalized


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    config_parser = argparse.ArgumentParser(add_help=False)
    config_parser.add_argument("--config")
    known, _ = config_parser.parse_known_args(argv)
    parser = build_arg_parser()
    if known.config:
        parser.set_defaults(**load_config_defaults(known.config, parser))
    return parser.parse_args(argv)


def main() -> int:
    args = parse_args()
    if not args.train_input or not args.train_target:
        raise ValueError("--train-input and --train-target are required (CLI or YAML config)")
    if bool(args.val_input) != bool(args.val_target):
        raise ValueError("--val-input and --val-target must be provided together")
    seed_everything(args.seed)
    device = torch.device(args.device)
    if device.type == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA requested but unavailable")

    train_set = PairedExposureDataset(args.train_input, args.train_target, args.crop_size, True)
    val_set = (PairedExposureDataset(args.val_input, args.val_target, args.crop_size, False)
               if args.val_input else None)
    loader_args = {
        "batch_size": args.batch_size,
        "num_workers": args.workers,
        "pin_memory": device.type == "cuda",
    }
    train_loader = DataLoader(train_set, shuffle=True, drop_last=False, **loader_args)
    val_loader = DataLoader(val_set, shuffle=False, drop_last=False, **loader_args) if val_set else None

    model = build_param_net(down=args.down, ch=args.channels, lut_dim=args.lut_dim).to(device)
    model.train()
    lut_apply = LUTApply(lut_dim=args.lut_dim).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=args.lr, betas=(0.9, 0.99),
                                 weight_decay=args.weight_decay)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=args.epochs)
    amp_enabled = args.amp and device.type == "cuda"
    try:
        scaler = torch.amp.GradScaler("cuda", enabled=amp_enabled)
    except (AttributeError, TypeError):
        scaler = torch.cuda.amp.GradScaler(enabled=amp_enabled)
    start_epoch = 1
    best_score = -math.inf
    previous_training_seconds = 0.0
    if args.resume:
        checkpoint = torch.load(args.resume, map_location=device, weights_only=False)
        model.load_state_dict(checkpoint["model"])
        optimizer.load_state_dict(checkpoint["optimizer"])
        if "scheduler" in checkpoint:
            scheduler.load_state_dict(checkpoint["scheduler"])
        if "scaler" in checkpoint:
            scaler.load_state_dict(checkpoint["scaler"])
        start_epoch = int(checkpoint["epoch"]) + 1
        best_score = float(checkpoint.get("best_score", -math.inf))
        previous_training_seconds = float(checkpoint.get("training_seconds", 0.0))
        restore_rng_state(checkpoint.get("rng_state"))

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "config.json").write_text(json.dumps(vars(args), indent=2, ensure_ascii=False) + "\n")
    weights = {
        "pixel": args.pixel_weight,
        "gradient": args.gradient_weight,
        "smooth": args.smooth_weight,
        "monotonic": args.monotonic_weight,
    }
    thumb_size = (args.thumb_height, args.thumb_width)
    steps_per_epoch = len(train_loader)
    total_steps = args.epochs * steps_per_epoch
    completed_steps = (start_epoch - 1) * steps_per_epoch
    progress = TrainingProgress(total_steps, completed_steps, previous_training_seconds)
    print(f"device={device} train_pairs={len(train_set)} val_pairs={len(val_set) if val_set else 0} "
          f"params={sum(p.numel() for p in model.parameters()):,} "
          f"epochs={start_epoch}-{args.epochs} batches_per_epoch={steps_per_epoch}")
    if args.resume:
        _, remaining = progress.estimate(completed_steps)
        print(f"resume={args.resume} completed_epoch={start_epoch - 1} "
              f"remaining={format_duration(remaining)} finish={progress.finish_time(remaining)}")

    for epoch in range(start_epoch, args.epochs + 1):
        started = time.perf_counter()
        running = {"loss": 0.0, "pixel": 0.0}
        seen = 0
        for batch_index, batch in enumerate(train_loader, start=1):
            source = batch["input"].to(device, non_blocking=True)
            target = batch["target"].to(device, non_blocking=True)
            optimizer.zero_grad(set_to_none=True)
            with torch.autocast(device_type=device.type, dtype=torch.float16, enabled=amp_enabled):
                loss, stats, _ = compute_loss(model, lut_apply, source, target, thumb_size, weights)
            scaler.scale(loss).backward()
            scaler.step(optimizer)
            scaler.update()
            batch_size = source.shape[0]
            running["loss"] += stats["loss"] * batch_size
            running["pixel"] += stats["pixel"] * batch_size
            seen += batch_size
            completed_steps += 1
            should_log = (args.log_every > 0 and
                          (batch_index % args.log_every == 0 or batch_index == steps_per_epoch))
            if should_log:
                elapsed, remaining = progress.estimate(completed_steps)
                overall = 100.0 * completed_steps / max(total_steps, 1)
                print(
                    f"epoch={epoch:04d}/{args.epochs:04d} "
                    f"batch={batch_index:05d}/{steps_per_epoch:05d} "
                    f"overall={overall:6.2f}% "
                    f"loss={running['loss'] / seen:.6f} "
                    f"elapsed={format_duration(elapsed)} "
                    f"eta={format_duration(remaining)} "
                    f"finish={progress.finish_time(remaining)}",
                    flush=True,
                )
        scheduler.step()

        validation_started = time.perf_counter()
        metrics = {
            "train_loss": running["loss"] / seen,
            "train_pixel": running["pixel"] / seen,
            "epoch_seconds": time.perf_counter() - started,
            "lr": optimizer.param_groups[0]["lr"],
        }
        if val_loader:
            print(f"epoch={epoch:04d}/{args.epochs:04d} validating batches={len(val_loader)}",
                  flush=True)
            metrics.update({f"val_{k}": v for k, v in
                            validate(model, lut_apply, val_loader, device, thumb_size, weights).items()})
        metrics["validation_seconds"] = time.perf_counter() - validation_started
        score = metrics.get("val_psnr", -metrics["train_loss"])
        elapsed, remaining = progress.estimate(completed_steps)
        print(
            f"epoch={epoch:04d}/{args.epochs:04d} complete "
            + " ".join(f"{key}={value:.6g}" for key, value in metrics.items())
            + f" elapsed={format_duration(elapsed)} eta={format_duration(remaining)} "
              f"finish={progress.finish_time(remaining)}",
            flush=True,
        )

        improved = score > best_score
        best_score = max(best_score, score)
        training_seconds = progress.elapsed_seconds
        save_checkpoint(output_dir / "last.pt", model, optimizer, scheduler, scaler, epoch, args,
                        metrics, best_score, training_seconds)
        if epoch % args.save_every == 0:
            save_checkpoint(output_dir / f"epoch_{epoch:04d}.pt", model, optimizer, scheduler,
                            scaler, epoch, args, metrics, best_score, training_seconds)
        if improved:
            save_checkpoint(output_dir / "best.pt", model, optimizer, scheduler, scaler, epoch,
                            args, metrics, best_score, training_seconds)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
