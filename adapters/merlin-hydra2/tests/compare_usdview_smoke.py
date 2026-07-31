#!/usr/bin/env python3
"""Compare Tier 0 and HgiVulkan usdview smoke evidence."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PySide6.QtGui import QImage


PHASES = (
    "baseline",
    "points",
    "topology",
    "primvar",
    "transform",
    "visibility",
    "camera",
    "material_parameter",
    "diagnostic",
    "recovery",
    "remove",
    "readd",
    "resize",
)
MAX_CHANGED_PIXEL_FRACTION = 0.0025
MAX_MEAN_CHANNEL_ERROR = 0.25


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("tier0", type=Path)
    parser.add_argument("gpu_copy", type=Path)
    parser.add_argument("output", type=Path)
    return parser.parse_args()


def load_image(path: Path) -> QImage:
    image = QImage(str(path))
    if image.isNull():
        raise ValueError(f"could not load {path}")
    return image.convertToFormat(QImage.Format.Format_RGBA8888)


def compare_image(tier0_path: Path, gpu_copy_path: Path) -> dict:
    tier0 = load_image(tier0_path)
    gpu_copy = load_image(gpu_copy_path)
    if tier0.size() != gpu_copy.size():
        raise ValueError(
            f"image size mismatch for {tier0_path.name}: "
            f"{tier0.width()}x{tier0.height()} != "
            f"{gpu_copy.width()}x{gpu_copy.height()}"
        )

    tier0_bytes = bytes(tier0.constBits())[: tier0.sizeInBytes()]
    gpu_copy_bytes = bytes(gpu_copy.constBits())[: gpu_copy.sizeInBytes()]
    differences = [
        abs(tier0_value - gpu_copy_value)
        for tier0_value, gpu_copy_value in zip(tier0_bytes, gpu_copy_bytes)
    ]
    pixel_count = tier0.width() * tier0.height()
    changed_pixels = sum(
        any(differences[offset : offset + 4])
        for offset in range(0, len(differences), 4)
    )
    changed_fraction = changed_pixels / pixel_count
    mean_channel_error = sum(differences) / len(differences)
    if changed_fraction > MAX_CHANGED_PIXEL_FRACTION:
        raise ValueError(
            f"{tier0_path.name}: changed-pixel fraction "
            f"{changed_fraction:.6f} exceeds {MAX_CHANGED_PIXEL_FRACTION:.6f}"
        )
    if mean_channel_error > MAX_MEAN_CHANNEL_ERROR:
        raise ValueError(
            f"{tier0_path.name}: mean channel error "
            f"{mean_channel_error:.6f} exceeds {MAX_MEAN_CHANNEL_ERROR:.6f}"
        )
    return {
        "name": tier0_path.name,
        "width": tier0.width(),
        "height": tier0.height(),
        "pixel_count": pixel_count,
        "changed_pixels": changed_pixels,
        "changed_pixel_fraction": changed_fraction,
        "maximum_channel_error": max(differences, default=0),
        "mean_channel_error": mean_channel_error,
    }


def load_baseline(directory: Path) -> dict:
    report_path = directory / "merlin-hydra-performance.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    return next(phase for phase in report["phases"] if phase["name"] == "baseline")


def stage_median(phase: dict, stage: str) -> int | None:
    summary = phase["stages"][stage]["summary_ns"]
    return None if summary is None else summary["median"]


def performance_evidence(tier0: dict, gpu_copy: dict) -> dict:
    if not tier0["stages"]["render_buffer_map"]["available"]:
        raise ValueError("Tier 0 baseline has no RenderBuffer Map evidence")
    if not tier0["stages"]["host_upload"]["available"]:
        raise ValueError("Tier 0 baseline has no host-upload evidence")
    if not gpu_copy["stages"]["gpu_copy"]["available"]:
        raise ValueError("GPU-copy baseline has no copy evidence")
    if gpu_copy["stages"]["render_buffer_map"]["available"]:
        raise ValueError("GPU-copy baseline unexpectedly mapped color")
    if gpu_copy["stages"]["host_upload"]["available"]:
        raise ValueError("GPU-copy baseline unexpectedly uploaded color")

    tier0_counters = tier0["last_counters"]
    gpu_copy_counters = gpu_copy["last_counters"]
    if tier0_counters["cpu_readback_aov_count"] != 4:
        raise ValueError("Tier 0 baseline did not read back four AOVs")
    if gpu_copy_counters["cpu_readback_aov_count"] != 3:
        raise ValueError("GPU-copy baseline did not retain three CPU AOVs")
    if gpu_copy_counters["readback_bytes"] >= tier0_counters["readback_bytes"]:
        raise ValueError("GPU-copy baseline did not reduce CPU readback bytes")

    tier0_readback = stage_median(tier0, "readback")
    tier0_upload = stage_median(tier0, "host_upload")
    gpu_readback = stage_median(gpu_copy, "readback")
    gpu_copy_time = stage_median(gpu_copy, "gpu_copy")
    return {
        "tier0": {
            "cpu_readback_aov_count": tier0_counters["cpu_readback_aov_count"],
            "readback_bytes": tier0_counters["readback_bytes"],
            "readback_median_ns": tier0_readback,
            "host_upload_median_ns": tier0_upload,
            "combined_transfer_median_ns": tier0_readback + tier0_upload,
        },
        "gpu_copy": {
            "cpu_readback_aov_count": gpu_copy_counters["cpu_readback_aov_count"],
            "readback_bytes": gpu_copy_counters["readback_bytes"],
            "readback_median_ns": gpu_readback,
            "gpu_copy_median_ns": gpu_copy_time,
            "combined_transfer_median_ns": gpu_readback + gpu_copy_time,
        },
        "delta": {
            "readback_bytes_saved": (
                tier0_counters["readback_bytes"]
                - gpu_copy_counters["readback_bytes"]
            ),
            "combined_transfer_median_ns_saved": (
                tier0_readback + tier0_upload - gpu_readback - gpu_copy_time
            ),
        },
    }


def main() -> None:
    args = parse_args()
    images = [
        compare_image(
            args.tier0 / f"usdview-first-frame-{phase}.png",
            args.gpu_copy / f"usdview-first-frame-{phase}.png",
        )
        for phase in PHASES
    ]
    evidence = {
        "schema": "merlin-hydra-presentation-comparison/v1",
        "thresholds": {
            "maximum_changed_pixel_fraction": MAX_CHANGED_PIXEL_FRACTION,
            "maximum_mean_channel_error": MAX_MEAN_CHANNEL_ERROR,
        },
        "images": images,
        "summary": {
            "maximum_changed_pixel_fraction": max(
                image["changed_pixel_fraction"] for image in images
            ),
            "maximum_mean_channel_error": max(
                image["mean_channel_error"] for image in images
            ),
        },
        "performance": performance_evidence(
            load_baseline(args.tier0), load_baseline(args.gpu_copy)
        ),
    }
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
