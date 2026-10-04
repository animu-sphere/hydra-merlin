#!/usr/bin/env python3
"""Require real lavapipe execution; CTest's optional skips are not evidence."""

import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET


REQUIRED_TESTS = (
    "merlin-shader-abi",
    "merlin-shader-artifact-key",
    "merlin-vulkan-offscreen",
    "merlin-vulkan-validation",
    "merlin-vulkan-gaussian-raster",
    "merlin-vulkan-execution-lifetime",
    "merlin-vulkan-resource-update",
    "merlin-vulkan-material-resources",
    "merlin-benchmark-json",
    "merlin-benchmark-compare",
    "merlin-viewport-vulkan",
    "merlin-install-consumer",
)


def check_evidence(metadata: Path, junit: Path) -> None:
    capabilities = json.loads(metadata.read_text(encoding="utf-8"))["vulkan"]
    # VK_DRIVER_ID_MESA_LLVMPIPE reports "llvmpipe" for the lavapipe Vulkan ICD.
    if capabilities["driver_name"] != "llvmpipe":
        raise ValueError(f"expected Mesa lavapipe, got {capabilities['driver_name']!r}")
    api = tuple(int(part) for part in capabilities["device_api_version"].split("."))
    if api < (1, 4, 0):
        raise ValueError(f"lavapipe must expose Vulkan 1.4, got {api}")
    if capabilities["validation_enabled"] is not True:
        raise ValueError("Khronos validation was not enabled")

    cases = ET.parse(junit).getroot().findall(".//testcase")
    by_name = {}
    for case in cases:
        by_name.setdefault(case.get("name"), []).append(case)
        if case.find("failure") is not None or case.find("error") is not None:
            raise ValueError(f"failed test: {case.get('name')}")
    for name in REQUIRED_TESTS:
        matches = by_name.get(name, [])
        if len(matches) != 1:
            raise ValueError(f"expected one result for {name}, got {len(matches)}")
        case = matches[0]
        if case.find("skipped") is not None or case.get("status") != "run":
            raise ValueError(f"required test did not run: {name}")
    optional_skips = [case.get("name") for case in cases if case.find("skipped") is not None]
    print(f"Verified lavapipe Vulkan 1.4 and {len(REQUIRED_TESTS)} required tests")
    if optional_skips:
        print("Optional capability skips: " + ", ".join(optional_skips))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--junit", type=Path, required=True)
    args = parser.parse_args()
    check_evidence(args.metadata, args.junit)


if __name__ == "__main__":
    main()
