"""Compare measured Tier 0 and GPU-copy host captures from the same runtime.

Run with the OpenUSD lookdev Python environment (PySide6 is required).
Timing is descriptive; image, transfer mode and readback contracts are gates.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True

from PySide6.QtGui import QImage

spec = importlib.util.spec_from_file_location(
    "gaussian_smoke", Path(__file__).resolve().parents[1] /
    "adapters/merlin-hydra2/tests/gaussian_usdview_test.py")
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


def compare(tier0, gpu_copy):
    reports = [json.loads((directory / "gaussian-hydra-performance.json").read_text())
               for directory in (tier0, gpu_copy)]
    checks = [json.loads((directory / "gaussian-host-verification.json").read_text())
              for directory in (tier0, gpu_copy)]
    for check, mode in zip(checks, ("cpu-readback", "gpu-copy")):
        assert check["schema"] == "merlin-gaussian-host-verification/v1"
        assert check["host_transfer_mode"] == mode
    for key in ("camera_sequence", "measured_frames", "phase_samples"):
        assert checks[0][key] == checks[1][key], f"host {key} changed"
    for key in ("width", "height", "requested_aov_mask"):
        assert checks[0]["products"][key] == checks[1]["products"][key]
    captured = [json.loads((directory / "capture-provenance.json").read_text())
                for directory in (tier0, gpu_copy)]
    for capture in captured:
        assert capture["schema"] == "merlin-gaussian-host-capture/v1"
        assert capture["measured_frames"] == checks[0]["measured_frames"]
    for key in ("scene_sha256", "runtime_config_sha256", "renderer_sha256", "validation_enabled"):
        assert captured[0][key] == captured[1][key], f"host provenance {key} changed"
    phases = []
    for name in checks[0]["phase_samples"]:
        pair = [next(phase for phase in report["phases"] if phase["name"] == name)
                for report in reports]
        tolerance = smoke.REFERENCE_CHANNEL_TOLERANCE[
            "tiled" if name.endswith("gpu-tiled") else "sorted-stream"]
        image = QImage(str(gpu_copy / (name + ".png")))
        assert not image.isNull(), "missing GPU-copy capture"
        difference = smoke.compare_reference(image, str(tier0 / (name + ".png")), tolerance)
        width, height = image.width(), image.height()
        expected_readback = (width * height * 16, width * height * 12)
        for phase, expected in zip(pair, expected_readback):
            assert phase["last_counters"]["readback_bytes"] == expected
            assert phase["samples"] == checks[0]["measured_frames"]
        phases.append({"name": name, "image_difference": difference,
            "tier0_stages": pair[0]["stages"], "gpu_copy_stages": pair[1]["stages"],
            "tier0_counters": pair[0]["last_counters"],
            "gpu_copy_counters": pair[1]["last_counters"],
            "color_cpu_readback_bytes_saved": expected_readback[0] - expected_readback[1]})
    return {"schema": "merlin-gaussian-host-comparison/v1", "status": "pass",
            "tier0": str(tier0), "gpu_copy": str(gpu_copy),
            "capture_provenance": captured[0], "phases": phases}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("tier0", type=Path)
    parser.add_argument("gpu_copy", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = compare(args.tier0, args.gpu_copy)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
