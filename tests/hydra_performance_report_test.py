"""Report contracts: missing GPU timers, policy, and host-frame attribution."""
import importlib.util
from pathlib import Path
import unittest
import sys

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location(
    "report", Path(__file__).resolve().parents[1] / "scripts/hydra-performance-report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)
compare_spec = importlib.util.spec_from_file_location(
    "compare", Path(__file__).resolve().parents[1] / "scripts/compare-benchmarks.py")
compare = importlib.util.module_from_spec(compare_spec)
compare_spec.loader.exec_module(compare)


def event():
    result = {key: 0 for key in (*report.STAGES, *report.COUNTERS, *report.AVAILABILITY.values())}
    result.update(phase="static-gpu-tiled", render_pass_execute_ns=100,
                  gaussian_gpu_mode="require", gaussian_raster_path="tiled",
                  gaussian_gpu_timestamps_available=1,
                  gaussian_gpu_preparation_ns=20, gaussian_gpu_sort_ns=30,
                  gaussian_gpu_tile_ns=40, gaussian_raster_ns=50,
                  gaussian_visible_count=123, gaussian_gpu_fallback_count=0,
                  hgi_transfer_mode="gpu-copy")
    return result


class ReportTests(unittest.TestCase):
    def test_gpu_stages_and_policy_survive_report(self):
        phase = report.build_report(Path("capture.log"), [event()])["phases"][0]
        self.assertEqual(phase["gaussian_policy"], {
            "gaussian_gpu_mode": "require", "gaussian_raster_path": "tiled"})
        self.assertEqual(phase["host_transfer_mode"], "gpu-copy")
        self.assertEqual(phase["gaussian_counters"]["gaussian_visible_count"], 123)
        for stage, duration in (("gaussian_gpu_preparation", 20), ("gaussian_gpu_sort", 30),
                                ("gaussian_gpu_tile", 40), ("gaussian_raster", 50)):
            self.assertEqual(phase["stages"][stage]["summary_ns"]["median"], duration)
            self.assertTrue(phase["stages"][stage]["available"])

    def test_legacy_and_unsupported_gpu_timing_are_unavailable(self):
        for missing in (False, True):
            sample = event()
            sample.pop("gaussian_gpu_timestamps_available")
            if missing:
                for key in tuple(sample):
                    if key.startswith("gaussian_"):
                        sample.pop(key)
            stages = report.build_report(Path("legacy.log"), [sample])["phases"][0]["stages"]
            self.assertFalse(stages["gaussian_gpu_preparation"]["available"])
            self.assertIsNone(stages["gaussian_gpu_preparation"]["summary_ns"])
            self.assertEqual(stages["gaussian_gpu_preparation"]["samples"], 0)

    def test_host_upload_scopes_sum_per_presented_frame(self):
        intervals = [
            {"name": "HgiGLOps::CopyTextureCpuToGpu", "start": 11, "end": 13},
            {"name": "HgiGLOps::CopyTextureCpuToGpu", "start": 14, "end": 17},
            {"name": "HdxPresentTask::Execute", "start": 19, "end": 20},
            {"name": "HgiGLOps::CopyTextureCpuToGpu", "start": 21, "end": 25},
            {"name": "HdxPresentTask::Execute", "start": 29, "end": 30},
            {"name": "HgiGLOps::CopyTextureCpuToGpu", "start": 1, "end": 9},
        ]
        values = report.trace_stage_samples("measured", {"measured": (10, 30)}, intervals)
        self.assertEqual(values["host_upload_ns"], [5000, 4000])
        self.assertEqual(values["presentation_ns"], [1000, 1000])

    def test_gaussian_fallback_regression_is_structural(self):
        def capture(fallback):
            return {"fixture": {"name": "one-million-gaussians"}, "baselines": [{
                "name": "static-gpu-tiled", "stages_ns": {},
                "counters": {"gaussian_gpu_tile_raster_overflow_fallback_count": fallback}}]}
        result = compare.compare(capture(0), capture(1), None)
        self.assertEqual(result["status"], "regression")
        self.assertEqual(result["regressions"][0]["metric"],
                         "gaussian_gpu_tile_raster_overflow_fallback_count")

    def test_old_report_does_not_fabricate_gaussian_counters(self):
        old = {"fixture": {}, "baselines": [{"name": "legacy", "stages_ns": {}, "counters": {}}]}
        new = {"fixture": {}, "baselines": [{"name": "legacy", "stages_ns": {},
            "counters": {"gaussian_gpu_sorted_count": 0}}]}
        result = compare.compare(old, new, None)
        self.assertEqual(result["status"], "pass")
        self.assertIn("gaussian_gpu_sorted_count", result["notes"][-1])

    def test_failed_image_cannot_pass_comparison_even_against_itself(self):
        capture = {"fixture": {}, "baselines": [], "gaussian_verification": {
            "camera_sequence": "camera/v1", "comparisons": [{
                "name": "static-gpu-tiled", "passed": False, "max_color_channel_error": 8}]}}
        result = compare.compare(capture, capture, None)
        self.assertEqual(result["status"], "regression")
        self.assertEqual(result["regressions"][0]["kind"], "image")
        current = {**capture, "gaussian_verification": {}}
        result = compare.compare(capture, current, None)
        self.assertIn("image-set", [item["kind"] for item in result["regressions"]])


if __name__ == "__main__":
    unittest.main()
