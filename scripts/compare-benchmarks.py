#!/usr/bin/env python3
"""Compare two merlin-benchmark/v3 reports.

Structural work is checked by default because it is stable across machines.
Timing regression gates are opt-in and intended only for controlled hardware.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


SCHEMA = "merlin-benchmark/v3"
ADDITIVE_COUNTERS = {
    "snapshot_visited_records",
    "snapshot_copied_records",
    "snapshot_rebuilt_draws",
    "snapshot_fully_rebuilt_tables",
    "vertex_upload_bytes",
    "index_upload_bytes",
    "texture_upload_bytes",
    "upload_ring_reserved_bytes",
    "geometry_range_reuse_count",
    "geometry_arena_growth_count",
    "geometry_arena_growth_bytes",
    "upload_ring_growth_count",
    "upload_ring_growth_bytes",
    "bindless_sampled_image_descriptor_update_count",
    "bindless_sampler_descriptor_update_count",
    "transfer_submission_count",
    "queue_ownership_transfer_count",
}
STABLE_COUNTERS = (
    "snapshot_visited_records",
    "snapshot_copied_records",
    "snapshot_rebuilt_draws",
    "snapshot_fully_rebuilt_tables",
    "draw_count",
    "visible_primitive_count",
    "triangle_count",
    "upload_bytes",
    "vertex_upload_bytes",
    "index_upload_bytes",
    "texture_upload_bytes",
    "upload_ring_reserved_bytes",
    "readback_bytes",
    "requested_aov_mask",
    "rendered_aov_mask",
    "cpu_readback_aov_mask",
    "requested_aov_count",
    "rendered_aov_count",
    "cpu_readback_aov_count",
    "wait_count",
    "resolve_count",
    "map_count",
    "allocation_count",
    "buffer_allocation_count",
    "image_allocation_count",
    "buffer_allocation_bytes",
    "pipeline_creation_count",
    "scene_cache_hits",
    "scene_cache_misses",
    "geometry_cache_hits",
    "geometry_cache_misses",
    "texture_cache_hits",
    "texture_cache_misses",
    "sampler_cache_hits",
    "sampler_cache_misses",
    "geometry_reconcile_count",
    "texture_reconcile_count",
    "sampler_reconcile_count",
    "buffer_suballocation_count",
    "buffer_range_release_count",
    "geometry_range_reuse_count",
    "geometry_arena_growth_count",
    "geometry_arena_growth_bytes",
    "upload_ring_growth_count",
    "upload_ring_growth_bytes",
    "pipeline_cache_hits",
    "pipeline_cache_misses",
    "shader_module_cache_hits",
    "shader_module_cache_misses",
    "descriptor_layout_cache_hits",
    "descriptor_layout_cache_misses",
    "descriptor_pool_creation_count",
    "descriptor_allocation_count",
    "descriptor_update_count",
    "bindless_sampled_image_descriptor_update_count",
    "bindless_sampler_descriptor_update_count",
    "transfer_submission_count",
    "queue_ownership_transfer_count",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=Path)
    parser.add_argument("current", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--timing-threshold-percent",
        type=float,
        help="Fail median stage regressions above this percentage.",
    )
    return parser.parse_args()


def load(path: Path) -> dict:
    with path.open(encoding="utf-8") as stream:
        report = json.load(stream)
    if report.get("schema") != SCHEMA:
        raise ValueError(f"{path}: expected {SCHEMA}")
    return report


def indexed(report: dict) -> dict[str, dict]:
    result = {}
    for baseline in report.get("baselines", []):
        name = baseline["name"]
        if name in result:
            raise ValueError(f"duplicate baseline: {name}")
        result[name] = baseline
    return result


def compare(baseline: dict, current: dict, timing_percent: float | None) -> dict:
    regressions: list[dict] = []
    notes: list[str] = []
    unavailable_baseline_counters: set[str] = set()
    if baseline.get("fixture") != current.get("fixture"):
        regressions.append(
            {
                "kind": "fixture",
                "expected": baseline.get("fixture"),
                "actual": current.get("fixture"),
            }
        )

    if timing_percent is not None:
        comparable_fields = (
            "build_type",
            "compiler",
            "os",
            "architecture",
            "gpu",
            "driver",
            "vulkan_api",
            "timestamp_queries",
        )
        for field in comparable_fields:
            old_value = baseline.get("environment", {}).get(field)
            new_value = current.get("environment", {}).get(field)
            if old_value != new_value:
                regressions.append(
                    {
                        "kind": "environment",
                        "metric": field,
                        "expected": old_value,
                        "actual": new_value,
                    }
                )

    expected = indexed(baseline)
    actual = indexed(current)
    if list(expected) != list(actual):
        regressions.append(
            {
                "kind": "baseline-set",
                "expected": list(expected),
                "actual": list(actual),
            }
        )

    limiting: dict[str, dict] = {}
    for name in expected.keys() & actual.keys():
        old = expected[name]
        new = actual[name]
        for counter in STABLE_COUNTERS:
            old_value = old["counters"].get(counter)
            new_value = new["counters"].get(counter)
            if old_value is None and counter in ADDITIVE_COUNTERS:
                unavailable_baseline_counters.add(counter)
                continue
            if old_value != new_value:
                regressions.append(
                    {
                        "kind": "structural",
                        "baseline": name,
                        "metric": counter,
                        "expected": old_value,
                        "actual": new_value,
                    }
                )

        stages = {
            stage: values["median"]
            for stage, values in new["stages_ns"].items()
            if stage != "total_frame"
        }
        if stages:
            stage = max(stages, key=stages.get)
            limiting[name] = {"stage": stage, "median_ns": stages[stage]}

        if timing_percent is not None:
            for stage, old_values in old["stages_ns"].items():
                old_median = old_values["median"]
                new_median = new["stages_ns"][stage]["median"]
                if old_median == 0:
                    continue
                limit = old_median * (1.0 + timing_percent / 100.0)
                if new_median > limit:
                    regressions.append(
                        {
                            "kind": "timing",
                            "baseline": name,
                            "metric": stage,
                            "expected_max_ns": int(limit),
                            "actual_ns": new_median,
                        }
                    )
    if timing_percent is None:
        notes.append("timing thresholds disabled; structural checks only")
    if unavailable_baseline_counters:
        notes.append(
            "baseline predates additive counters: "
            + ", ".join(sorted(unavailable_baseline_counters))
        )
    return {
        "schema": "merlin-benchmark-comparison/v1",
        "status": "regression" if regressions else "pass",
        "regressions": regressions,
        "limiting_stages": limiting,
        "notes": notes,
    }


def main() -> int:
    args = parse_args()
    if (
        args.timing_threshold_percent is not None
        and args.timing_threshold_percent < 0
    ):
        print(
            "compare-benchmarks: timing threshold must be non-negative",
            file=sys.stderr,
        )
        return 1
    try:
        result = compare(
            load(args.baseline),
            load(args.current),
            args.timing_threshold_percent,
        )
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"compare-benchmarks: {error}", file=sys.stderr)
        return 1
    payload = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="utf-8")
    else:
        sys.stdout.write(payload)
    return 2 if result["regressions"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
