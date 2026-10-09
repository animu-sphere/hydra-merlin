"""Catch missing Hydra coverage, capability skips and accidental GPU selection."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "hosted_hydra_evidence",
    Path(__file__).resolve().parents[1] / "scripts/check-hosted-hydra-evidence.py",
)
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)


class EvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.selection = Path(self.temporary.name) / "selection.json"
        self.junit = Path(self.temporary.name) / "tests.xml"
        self.cache = Path(self.temporary.name) / "CMakeCache.txt"
        self.cache.write_text("MERLIN_GENERATE_RENDERER_REPORT:BOOL=OFF\n", encoding="utf-8")
        self.tests = [{"name": name, "properties": []} for name in evidence.REQUIRED_TESTS]
        self.suite = ET.Element("testsuite")
        for name in evidence.REQUIRED_TESTS:
            ET.SubElement(self.suite, "testcase", name=name, status="run")

    def check(self):
        # PowerShell's Out-File can emit a BOM; accept both UTF-8 forms.
        self.selection.write_text(json.dumps({"tests": self.tests}), encoding="utf-8-sig")
        ET.ElementTree(self.suite).write(self.junit)
        with contextlib.redirect_stdout(io.StringIO()):
            evidence.check_evidence(self.selection, self.junit, self.cache)

    def test_complete(self):
        self.check()

    def test_post_link_execution_must_be_disabled(self):
        for value in ("MERLIN_GENERATE_RENDERER_REPORT:BOOL=ON\n", ""):
            self.cache.write_text(value, encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "disable post-link"):
                self.check()

    def test_missing_hydra_case(self):
        self.tests.pop(0)
        with self.assertRaisesRegex(ValueError, "missing required tests"):
            self.check()

    def test_host_device_labels_rejected_but_gpu_scene_allowed(self):
        for label in ("gpu", "runtime", "host-smoke"):
            with self.subTest(label=label):
                self.tests[0]["properties"] = [{"name": "LABELS", "value": ["hydra", label]}]
                with self.assertRaisesRegex(ValueError, "host/device test selected"):
                    self.check()
        self.tests[0]["properties"] = [{"name": "LABELS", "value": ["gpu-scene", "shader"]}]
        self.check()

    def test_missing_result(self):
        self.suite.remove(self.suite[0])
        with self.assertRaisesRegex(ValueError, "missing test results"):
            self.check()

    def test_unlabelled_device_case(self):
        self.tests.append({"name": "merlin-vulkan-offscreen", "properties": []})
        with self.assertRaisesRegex(ValueError, "host/device test selected"):
            self.check()

    def test_duplicate_selection_or_result(self):
        self.tests.append(self.tests[0])
        with self.assertRaisesRegex(ValueError, "duplicate selected test"):
            self.check()
        self.tests.pop()
        ET.SubElement(self.suite, "testcase", name=evidence.REQUIRED_TESTS[0], status="run")
        with self.assertRaisesRegex(ValueError, "duplicate test result"):
            self.check()

    def test_unselected_result(self):
        ET.SubElement(self.suite, "testcase", name="merlin-vulkan-offscreen", status="run")
        with self.assertRaisesRegex(ValueError, "unexpected.*test result"):
            self.check()

    def test_failures_and_skips(self):
        for tag in ("failure", "error", "skipped"):
            with self.subTest(tag=tag):
                node = ET.SubElement(self.suite[0], tag)
                with self.assertRaises(ValueError):
                    self.check()
                self.suite[0].remove(node)
        self.suite[0].set("status", "notrun")
        with self.assertRaisesRegex(ValueError, "did not run"):
            self.check()


if __name__ == "__main__":
    unittest.main()
