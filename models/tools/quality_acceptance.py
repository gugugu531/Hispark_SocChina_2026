"""Analyze repeated 1024x600 NV21 OFF/ON quality-capture triplets.

Expected input layout:
  root/quality-XX/model_off.nv21
  root/quality-XX/model_on_before.nv21
  root/quality-XX/model_on_after.nv21

The tool writes PNG previews, contact sheets, per-frame CSV, and a JSON summary.
Metrics are no-reference engineering guardrails, not substitutes for a diverse
human-rated camera dataset.
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


WIDTH = 1024
HEIGHT = 600
FRAME_NAMES = ("model_off", "model_on_before", "model_on_after")


def load_nv21(path: Path) -> tuple[np.ndarray, np.ndarray]:
    raw = np.fromfile(path, dtype=np.uint8)
    expected = WIDTH * HEIGHT * 3 // 2
    if raw.size != expected:
        raise ValueError(f"{path}: expected {expected} bytes, got {raw.size}")
    y = raw[: WIDTH * HEIGHT].reshape(HEIGHT, WIDTH)
    vu = raw[WIDTH * HEIGHT :].reshape(HEIGHT // 2, WIDTH)
    return y, vu


def nv21_to_rgb(y: np.ndarray, vu: np.ndarray) -> np.ndarray:
    v = vu[:, 0::2].repeat(2, axis=0).repeat(2, axis=1).astype(np.float32)
    u = vu[:, 1::2].repeat(2, axis=0).repeat(2, axis=1).astype(np.float32)
    yf = y.astype(np.float32)
    c = np.maximum(yf - 16.0, 0.0)
    d = u - 128.0
    e = v - 128.0
    r = 1.164383 * c + 1.596027 * e
    g = 1.164383 * c - 0.391762 * d - 0.812968 * e
    b = 1.164383 * c + 2.017232 * d
    return np.clip(np.stack([r, g, b], axis=-1), 0, 255).astype(np.uint8)


def frame_metrics(y: np.ndarray, vu: np.ndarray, rgb: np.ndarray) -> dict[str, float]:
    yf = y.astype(np.float32)
    gx = np.abs(np.diff(yf, axis=1)).mean()
    gy = np.abs(np.diff(yf, axis=0)).mean()
    means = rgb.reshape(-1, 3).mean(axis=0)
    return {
        "y_mean": float(yf.mean()),
        "y_p01": float(np.percentile(yf, 1)),
        "y_p99": float(np.percentile(yf, 99)),
        "clip_low_pct": float((yf <= 5).mean() * 100),
        "clip_high_pct": float((yf >= 250).mean() * 100),
        "gradient_mean": float(gx + gy),
        "uv_distance_mean": float(
            np.sqrt((vu[:, 0::2].astype(float) - 128) ** 2 +
                    (vu[:, 1::2].astype(float) - 128) ** 2).mean()
        ),
        "rgb_mean_r": float(means[0]),
        "rgb_mean_g": float(means[1]),
        "rgb_mean_b": float(means[2]),
        "rgb_mean_spread": float(means.max() - means.min()),
    }


def pair_metrics(a_y: np.ndarray, a_vu: np.ndarray, b_y: np.ndarray,
                 b_vu: np.ndarray) -> dict[str, float]:
    return {
        "y_mae": float(np.abs(a_y.astype(float) - b_y.astype(float)).mean()),
        "uv_mae": float(np.abs(a_vu.astype(float) - b_vu.astype(float)).mean()),
    }


def labeled_preview(rgb: np.ndarray, label: str) -> Image.Image:
    image = Image.fromarray(rgb).resize((512, 300))
    canvas = Image.new("RGB", (512, 330), "white")
    canvas.paste(image, (0, 30))
    ImageDraw.Draw(canvas).text((8, 8), label, fill="black")
    return canvas


def aggregate(rows: list[dict], mode: str, metric: str) -> dict[str, float]:
    values = np.array([row[metric] for row in rows if row["mode"] == mode], dtype=float)
    return {
        "mean": float(values.mean()),
        "min": float(values.min()),
        "max": float(values.max()),
        "std": float(values.std()),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output or args.root / "analysis"
    output.mkdir(parents=True, exist_ok=True)

    rows: list[dict] = []
    pair_rows: list[dict] = []
    triplet_images: list[Image.Image] = []
    captures = sorted(p for p in args.root.glob("quality-*") if p.is_dir())
    if not captures:
        raise SystemExit(f"no quality-* directories under {args.root}")

    for capture in captures:
        loaded: dict[str, tuple[np.ndarray, np.ndarray, np.ndarray]] = {}
        previews = []
        for name in FRAME_NAMES:
            y, vu = load_nv21(capture / f"{name}.nv21")
            rgb = nv21_to_rgb(y, vu)
            loaded[name] = (y, vu, rgb)
            metrics = frame_metrics(y, vu, rgb)
            rows.append({"capture": capture.name, "mode": name, **metrics})
            Image.fromarray(rgb).save(output / f"{capture.name}_{name}.png")
            previews.append(labeled_preview(rgb, f"{capture.name} {name}"))

        sheet = Image.new("RGB", (512 * 3, 330), "white")
        for index, preview in enumerate(previews):
            sheet.paste(preview, (index * 512, 0))
        sheet.save(output / f"{capture.name}_triplet.png")
        triplet_images.append(sheet)

        off_y, off_vu, _ = loaded["model_off"]
        before_y, before_vu, _ = loaded["model_on_before"]
        after_y, after_vu, _ = loaded["model_on_after"]
        pair_rows.append({
            "capture": capture.name,
            "pair": "off_vs_on_after",
            **pair_metrics(off_y, off_vu, after_y, after_vu),
        })
        pair_rows.append({
            "capture": capture.name,
            "pair": "on_before_vs_on_after",
            **pair_metrics(before_y, before_vu, after_y, after_vu),
        })

    contact = Image.new("RGB", (512 * 3, 330 * len(triplet_images)), "white")
    for index, sheet in enumerate(triplet_images):
        contact.paste(sheet, (0, index * 330))
    contact.save(output / "contact_sheet.png")

    with (output / "frame_metrics.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    with (output / "pair_metrics.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(pair_rows[0]))
        writer.writeheader()
        writer.writerows(pair_rows)

    metrics = ("y_mean", "clip_low_pct", "clip_high_pct", "gradient_mean",
               "uv_distance_mean", "rgb_mean_spread")
    aggregates = {
        mode: {metric: aggregate(rows, mode, metric) for metric in metrics}
        for mode in FRAME_NAMES
    }
    off_high = aggregates["model_off"]["clip_high_pct"]["mean"]
    after_high = aggregates["model_on_after"]["clip_high_pct"]["mean"]
    off_low = aggregates["model_off"]["clip_low_pct"]["mean"]
    after_low = aggregates["model_on_after"]["clip_low_pct"]["mean"]
    off_gradient = aggregates["model_off"]["gradient_mean"]["mean"]
    after_gradient = aggregates["model_on_after"]["gradient_mean"]["mean"]
    after_spread = aggregates["model_on_after"]["rgb_mean_spread"]["mean"]
    feedback_values = np.array(
        [row["y_mae"] for row in pair_rows if row["pair"] == "on_before_vs_on_after"]
    )

    checks = {
        "highlight_delta_le_1pp": after_high - off_high <= 1.0,
        "low_clip_delta_le_1pp": after_low - off_low <= 1.0,
        "rgb_mean_spread_le_12": after_spread <= 12.0,
        "gradient_retention_ge_80pct": after_gradient >= off_gradient * 0.8,
        "feedback_y_mae_le_8": float(feedback_values.mean()) <= 8.0,
        "sample_count_ge_20_frames": len(rows) >= 20,
        "scene_count_ge_5": False,
    }
    summary = {
        "scope": {
            "captures": len(captures),
            "frames": len(rows),
            "scene_count": 1,
            "resolution": [WIDTH, HEIGHT],
            "limitation": "Repeated captures of one current camera scene; not a diverse final dataset.",
        },
        "aggregates": aggregates,
        "pair_aggregates": {
            "feedback_y_mae": {
                "mean": float(feedback_values.mean()),
                "max": float(feedback_values.max()),
                "std": float(feedback_values.std()),
            }
        },
        "deltas": {
            "on_after_minus_off_highlight_pp": after_high - off_high,
            "on_after_minus_off_low_clip_pp": after_low - off_low,
            "on_after_vs_off_gradient_ratio": after_gradient / off_gradient,
        },
        "checks": checks,
        "current_scene_pass": all(value for key, value in checks.items()
                                  if key != "scene_count_ge_5"),
        "final_acceptance_pass": all(checks.values()),
    }

    gamma_rows: list[dict] = []
    gamma_pairs: list[dict] = []
    gamma_sheets: list[Image.Image] = []
    fair025_captures = sorted(
        p for p in args.root.glob("gamma-fair025-*") if p.is_dir()
    )
    fair_captures = fair025_captures or sorted(
        p for p in args.root.glob("gamma-fair-*")
        if p.is_dir() and not p.name.startswith("gamma-fair025-")
    )
    gamma_captures = fair_captures or sorted(
        p for p in args.root.glob("gamma-feedback-*") if p.is_dir()
    )
    for capture in gamma_captures:
        if capture.name.startswith("gamma-fair"):
            paths = {
                "gamma_bypass": capture / "gamma_bypass_stable.nv21",
                "gamma_bidir_after": capture / "gamma_bidir_0.nv21",
            }
        else:
            paths = {
                "gamma_bypass": capture / "gamma_bypass_0.nv21",
                "gamma_bidir_before": capture / "gamma_bidir_1.nv21",
                "gamma_bidir_after": capture / "gamma_bidir_2.nv21",
            }
        loaded_gamma = {}
        previews = []
        for mode, path in paths.items():
            y, vu = load_nv21(path)
            rgb = nv21_to_rgb(y, vu)
            loaded_gamma[mode] = (y, vu, rgb)
            gamma_rows.append({
                "capture": capture.name,
                "mode": mode,
                **frame_metrics(y, vu, rgb),
            })
            Image.fromarray(rgb).save(output / f"{capture.name}_{mode}.png")
            previews.append(labeled_preview(rgb, f"{capture.name} {mode}"))
        sheet = Image.new("RGB", (512 * len(previews), 330), "white")
        for index, preview in enumerate(previews):
            sheet.paste(preview, (index * 512, 0))
        sheet.save(output / f"{capture.name}_triplet.png")
        gamma_sheets.append(sheet)

        bypass_y, bypass_vu, _ = loaded_gamma["gamma_bypass"]
        after_y, after_vu, _ = loaded_gamma["gamma_bidir_after"]
        gamma_pairs.append({
            "capture": capture.name,
            "pair": "bypass_vs_bidir_after",
            **pair_metrics(bypass_y, bypass_vu, after_y, after_vu),
        })
        if "gamma_bidir_before" in loaded_gamma:
            before_y, before_vu, _ = loaded_gamma["gamma_bidir_before"]
            gamma_pairs.append({
                "capture": capture.name,
                "pair": "bidir_before_vs_after",
                **pair_metrics(before_y, before_vu, after_y, after_vu),
            })

    if gamma_rows:
        gamma_width = max(image.width for image in gamma_sheets)
        gamma_contact = Image.new("RGB", (gamma_width, 330 * len(gamma_sheets)), "white")
        for index, sheet in enumerate(gamma_sheets):
            gamma_contact.paste(sheet, (0, index * 330))
        gamma_contact.save(output / "gamma_contact_sheet.png")
        with (output / "gamma_frame_metrics.csv").open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(gamma_rows[0]))
            writer.writeheader()
            writer.writerows(gamma_rows)
        with (output / "gamma_pair_metrics.csv").open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(gamma_pairs[0]))
            writer.writeheader()
            writer.writerows(gamma_pairs)

        gamma_modes = sorted({row["mode"] for row in gamma_rows})
        gamma_aggregates = {
            mode: {metric: aggregate(gamma_rows, mode, metric) for metric in metrics}
            for mode in gamma_modes
        }
        bypass = gamma_aggregates["gamma_bypass"]
        bidir = gamma_aggregates["gamma_bidir_after"]
        gamma_feedback = np.array(
            [row["y_mae"] for row in gamma_pairs
             if row["pair"] == "bidir_before_vs_after"]
        )
        gamma_checks = {
            "highlight_not_worse": (
                bidir["clip_high_pct"]["mean"] <= bypass["clip_high_pct"]["mean"] + 1.0
            ),
            "low_clip_delta_le_1pp": (
                bidir["clip_low_pct"]["mean"] - bypass["clip_low_pct"]["mean"] <= 1.0
            ),
            "gradient_retention_ge_80pct": (
                bidir["gradient_mean"]["mean"] >= bypass["gradient_mean"]["mean"] * 0.8
            ),
            "rgb_spread_delta_le_5": (
                bidir["rgb_mean_spread"]["mean"] -
                bypass["rgb_mean_spread"]["mean"] <= 5.0
            ),
            "repeat_y_std_le_5": bidir["y_mean"]["std"] <= 5.0,
            "capture_count_ge_7": len(gamma_captures) >= 7,
        }
        summary["gamma_fallback"] = {
            "captures": len(gamma_captures),
            "aggregates": gamma_aggregates,
            "feedback_y_mae": (
                {
                    "mean": float(gamma_feedback.mean()),
                    "max": float(gamma_feedback.max()),
                    "std": float(gamma_feedback.std()),
                }
                if gamma_feedback.size
                else None
            ),
            "deltas": {
                "bidir_minus_bypass_highlight_pp": (
                    bidir["clip_high_pct"]["mean"] - bypass["clip_high_pct"]["mean"]
                ),
                "bidir_minus_bypass_low_clip_pp": (
                    bidir["clip_low_pct"]["mean"] - bypass["clip_low_pct"]["mean"]
                ),
                "bidir_vs_bypass_gradient_ratio": (
                    bidir["gradient_mean"]["mean"] / bypass["gradient_mean"]["mean"]
                ),
                "bidir_minus_bypass_rgb_spread": (
                    bidir["rgb_mean_spread"]["mean"] -
                    bypass["rgb_mean_spread"]["mean"]
                ),
            },
            "checks": gamma_checks,
            "current_scene_pass": all(gamma_checks.values()),
        }
        summary["production_current_scene_pass"] = summary["gamma_fallback"][
            "current_scene_pass"
        ]
    else:
        summary["production_current_scene_pass"] = False

    (output / "summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
