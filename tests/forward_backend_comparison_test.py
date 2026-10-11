"""Boundary checks for accepting independent backend image evidence."""

import copy
import unittest

import compare_forward_backends as comparison


def report(backend, host=False):
    names = comparison.HOST_PHASES if host else comparison.NATIVE_PHASES
    key = "phase" if host else "name"
    return {
        "schema": ("merlin-usdview-forward-lighting/v1" if host else
                   "merlin-forward-image-fixture/v1" if backend == "vulkan" else
                   "merlin-metal-forward-lighting/v1"),
        "backend": backend, "lighting_profile": comparison.PROFILE,
        "color_aov": "linear-rgba8-with-embedded-reinhard",
        "white_off_policy": "headroom-required",
        "off_policy": "unit-white-plus-z-diagnostic-fallback",
        "phases": [{key: name, "center_rgb": [64, 128, 192],
                    "upload_bytes": 0, "pipeline_creation_count": 0} for name in names],
    }


class ComparisonTests(unittest.TestCase):
    def test_native_one_step_passes_two_steps_fail(self):
        a, b = report("vulkan"), report("metal")
        b["phases"][0]["center_rgb"][1] += 1
        self.assertEqual(len(comparison.compare_native(a, b)), 10)
        b["phases"][0]["center_rgb"][1] += 1
        with self.assertRaises(ValueError):
            comparison.compare_native(a, b)

    def test_stale_output_and_incomplete_evidence_fail(self):
        a, baseline = report("vulkan"), report("metal")
        for field in ("lighting_profile", "color_aov", "off_policy"):
            b = copy.deepcopy(baseline)
            b[field] = "old-profile"
            with self.assertRaises(ValueError):
                comparison.compare_native(a, b)
        for mutation in ("missing", "duplicate", "saturated", "uploaded"):
            b = copy.deepcopy(baseline)
            if mutation == "missing":
                b["phases"].pop()
            elif mutation == "duplicate":
                b["phases"].append(b["phases"][0])
            elif mutation == "saturated":
                b["phases"][0]["center_rgb"][0] = 255
            else:
                b["phases"][0]["upload_bytes"] = 1
            with self.assertRaises(ValueError):
                comparison.compare_native(a, b)

    def test_host_transfer_tolerance_and_backend_identity(self):
        a, b = report("vulkan", True), report("metal", True)
        b["phases"][0]["center_rgb"][0] += 2
        self.assertEqual(len(comparison.compare_host(a, b)), 24)
        b["phases"][0]["center_rgb"][0] += 1
        with self.assertRaises(ValueError):
            comparison.compare_host(a, b)
        b = report("metal", True)
        b["backend"] = "vulkan"
        with self.assertRaises(ValueError):
            comparison.compare_host(a, b)

    def test_dark_environment_quantization_keeps_saturation_gate(self):
        a, b = report("vulkan"), report("metal")
        for item in (a, b):
            item["phases"][-2]["center_rgb"][2] = 0
        self.assertEqual(len(comparison.compare_native(a, b)), 10)
        b["phases"][-2]["center_rgb"][2] = 255
        with self.assertRaises(ValueError):
            comparison.compare_native(a, b)


if __name__ == "__main__":
    unittest.main()
