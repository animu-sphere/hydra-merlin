"""Exercise false-success cases for the Linux runtime evidence gate."""

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
    "linux_vulkan_evidence",
    Path(__file__).resolve().parents[1] / "scripts/check-linux-vulkan-evidence.py",
)
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)


class EvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.metadata = Path(self.temporary.name) / "metadata.json"
        self.junit = Path(self.temporary.name) / "tests.xml"
        self.capabilities = {
            "driver_name": "llvmpipe",
            "device_api_version": "1.4.338",
            "validation_enabled": True,
        }
        self.suite = ET.Element("testsuite")
        for name in evidence.REQUIRED_TESTS:
            ET.SubElement(self.suite, "testcase", name=name, status="run")

    def check(self):
        self.metadata.write_text(json.dumps({"vulkan": self.capabilities}), encoding="utf-8")
        ET.ElementTree(self.suite).write(self.junit)
        with contextlib.redirect_stdout(io.StringIO()):
            evidence.check_evidence(self.metadata, self.junit)

    def test_complete_with_optional_capability_skip(self):
        optional = ET.SubElement(self.suite, "testcase", name="optional-bindless", status="notrun")
        ET.SubElement(optional, "skipped")
        self.check()

    def test_required_skip_is_rejected(self):
        ET.SubElement(self.suite[0], "skipped")
        with self.assertRaisesRegex(ValueError, "did not run"):
            self.check()

    def test_missing_and_duplicate_results_are_rejected(self):
        self.suite.remove(self.suite[0])
        with self.assertRaisesRegex(ValueError, "got 0"):
            self.check()
        for _ in range(2):
            ET.SubElement(self.suite, "testcase", name=evidence.REQUIRED_TESTS[0], status="run")
        with self.assertRaisesRegex(ValueError, "got 2"):
            self.check()

    def test_any_failure_is_rejected(self):
        optional = ET.SubElement(self.suite, "testcase", name="optional-bindless", status="run")
        ET.SubElement(optional, "failure")
        with self.assertRaisesRegex(ValueError, "failed test"):
            self.check()

    def test_incorrect_driver_api_or_disabled_validation_is_rejected(self):
        for key, value in (("driver_name", "NVIDIA"), ("device_api_version", "1.3.290"),
                           ("validation_enabled", False)):
            with self.subTest(key=key):
                original = self.capabilities[key]
                self.capabilities[key] = value
                with self.assertRaises(ValueError):
                    self.check()
                self.capabilities[key] = original


if __name__ == "__main__":
    unittest.main()
