#!/usr/bin/env python3
"""Require Hydra compile/package evidence while excluding host/device execution."""

import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET


REQUIRED_TESTS = (
    "merlin-openusd-python-includes",
    "merlin-hydra2-discovery",
    "merlin-hydra2-install-discovery",
    "merlin-hydra2-render-buffer",
    "merlin-hydra2-render-settings",
    "merlin-hydra2-hgi-vulkan-bridge",
    "merlin-hydra2-gaussian-usd",
    "merlin-shader-abi",
    "merlin-shader-artifact-key",
    "merlin-install-consumer",
    "merlin-backend-boundaries",
)
EXCLUDED_LABELS = {"gpu", "runtime", "host-smoke"}
DEVICE_TESTS = {
    "merlin-vulkan-offscreen",
    "merlin-vulkan-install-runtime",
    "merlin-metal-install-runtime",
}


def check_evidence(selection: Path, junit: Path, cache: Path) -> None:
    cache_lines = cache.read_text(encoding="utf-8").splitlines()
    if "MERLIN_GENERATE_RENDERER_REPORT:BOOL=OFF" not in cache_lines:
        raise ValueError("build must disable post-link renderer execution")
    selected = json.loads(selection.read_text(encoding="utf-8-sig"))["tests"]
    names = set()
    for test in selected:
        name = test["name"]
        if name in names:
            raise ValueError(f"duplicate selected test: {name}")
        names.add(name)
        labels = next((prop["value"] for prop in test["properties"]
                       if prop["name"] == "LABELS"), [])
        if name in DEVICE_TESTS or EXCLUDED_LABELS.intersection(labels):
            raise ValueError(f"host/device test selected: {name}")
    missing = set(REQUIRED_TESTS) - names
    if missing:
        raise ValueError("missing required tests: " + ", ".join(sorted(missing)))

    completed = set()
    for case in ET.parse(junit).getroot().findall(".//testcase"):
        name = case.get("name")
        if name not in names or name in completed:
            raise ValueError(f"unexpected or duplicate test result: {name}")
        if case.find("failure") is not None or case.find("error") is not None:
            raise ValueError(f"failed test: {name}")
        if case.find("skipped") is not None or case.get("status") != "run":
            raise ValueError(f"selected test did not run: {name}")
        completed.add(name)
    if completed != names:
        raise ValueError("missing test results: " + ", ".join(sorted(names - completed)))
    print(f"Verified {len(completed)} GPU-free cases including Hydra and installed discovery")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selection", type=Path, required=True)
    parser.add_argument("--junit", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    args = parser.parse_args()
    check_evidence(args.selection, args.junit, args.cache)


if __name__ == "__main__":
    main()
