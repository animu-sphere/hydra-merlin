#!/usr/bin/env python3
"""Convert hdMerlin's regression event log into a versioned JSON report."""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path


STAGES = (
    "hydra_sync_ns",
    "scene_index_processing_ns",
    "render_world_update_ns",
    "snapshot_extraction_ns",
    "gpu_scene_update_ns",
    "command_recording_ns",
    "queue_submission_ns",
    "gpu_execution_ns",
    "completion_wait_ns",
    "readback_ns",
    "render_buffer_resolve_ns",
    "render_buffer_map_ns",
    "host_upload_ns",
    "host_composite_ns",
    "presentation_ns",
    "render_pass_execute_ns",
)
AVAILABILITY = {
    "scene_index_processing_ns": "scene_index_processing_available",
    "host_upload_ns": "host_upload_ns_available",
    "host_composite_ns": "host_composite_ns_available",
    "presentation_ns": "presentation_ns_available",
}
COUNTERS = (
    "mesh_sync_count",
    "material_sync_count",
    "light_sync_count",
    "instancer_sync_count",
    "camera_sync_count",
    "points_fetch_count",
    "topology_fetch_count",
    "primvar_descriptor_fetch_count",
    "primvar_fetch_count",
    "material_fetch_count",
    "requested_aov_count",
    "rendered_aov_count",
    "cpu_readback_aov_count",
    "requested_aov_mask",
    "rendered_aov_mask",
    "cpu_readback_aov_mask",
    "upload_bytes",
    "readback_bytes",
    "host_upload_bytes",
    "visible_primitive_count",
    "wait_count",
    "map_count",
    "resolve_count",
    "descriptor_pool_creation_count",
    "descriptor_allocation_count",
    "descriptor_update_count",
    "allocation_count",
    "buffer_allocation_bytes",
    "image_allocation_bytes",
    "pipeline_creation_count",
    "shader_module_cache_misses",
    "geometry_cache_misses",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--host-trace", type=Path)
    return parser.parse_args()


def parse_events(path: Path) -> list[dict]:
    events = []
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), 1
    ):
        event = {}
        for field in line.split():
            key, separator, value = field.partition("=")
            if not separator:
                raise ValueError(f"{path}:{line_number}: malformed field")
            event[key] = value if key == "phase" else int(value)
        if event.get("schema_version") != 3:
            raise ValueError(f"{path}:{line_number}: unsupported event schema")
        events.append(event)
    if not events:
        raise ValueError(f"{path}: no events")
    return events


def percentile(values: list[int], percent: int) -> int:
    rank = max(1, (percent * len(values) + 99) // 100)
    return values[rank - 1]


def summarize(values: list[int]) -> dict:
    values = sorted(values)
    middle = len(values) // 2
    median = (
        values[middle]
        if len(values) % 2
        else values[middle - 1] + (values[middle] - values[middle - 1]) // 2
    )
    return {
        "median": median,
        "p95": percentile(values, 95),
        "p99": percentile(values, 99),
        "max": values[-1],
    }


def trace_intervals(path: Path) -> tuple[dict[str, tuple[float, float]], list[dict]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    stacks: dict[str, list[dict]] = defaultdict(list)
    intervals = []
    for event in document.get("traceEvents", []):
        phase = event.get("ph")
        if phase == "X":
            intervals.append(
                {
                    "name": event["name"],
                    "start": event["ts"],
                    "end": event["ts"] + event.get("dur", 0),
                }
            )
        elif phase == "B":
            stacks[str(event.get("tid"))].append(event)
        elif phase == "E":
            stack = stacks[str(event.get("tid"))]
            if not stack:
                continue
            begin = stack.pop()
            intervals.append(
                {
                    "name": begin["name"],
                    "start": begin["ts"],
                    "end": event["ts"],
                }
            )
    phases = {
        interval["name"].removeprefix("MerlinPhase:"): (
            interval["start"], interval["end"]
        )
        for interval in intervals
        if interval["name"].startswith("MerlinPhase:")
    }
    return phases, intervals


TRACE_STAGE_NAMES = {
    "scene_index_processing_ns": (
        "UsdImagingStageSceneIndex::ApplyPendingUpdates",
        "HdNoticeBatchingSceneIndex::Flush",
    ),
    "host_upload_ns": ("HgiGLOps::CopyTextureCpuToGpu",),
    "host_composite_ns": (
        "HdxColorCorrectionTask::Execute",
        "HdxColorizeSelectionTask::Execute",
    ),
    "presentation_ns": ("HdxPresentTask::Execute",),
    "render_buffer_resolve_ns": ("HdMerlinRenderBuffer::Resolve",),
    "render_buffer_map_ns": ("HdMerlinRenderBuffer::Map",),
}


def trace_stage_samples(
    phase: str,
    phase_intervals: dict[str, tuple[float, float]],
    intervals: list[dict],
) -> dict[str, list[int]]:
    if phase not in phase_intervals:
        return {}
    phase_start, phase_end = phase_intervals[phase]
    phase_events = [
        interval
        for interval in intervals
        if interval["start"] >= phase_start and interval["end"] <= phase_end
    ]
    presentations = sorted(
        (
            interval
            for interval in phase_events
            if interval["name"].endswith("HdxPresentTask::Execute")
        ),
        key=lambda interval: interval["end"],
    )
    windows = []
    window_start = phase_start
    for presentation in presentations:
        windows.append((window_start, presentation["end"]))
        window_start = presentation["end"]
    result = {}
    for stage, suffixes in TRACE_STAGE_NAMES.items():
        matches = [
            interval
            for interval in phase_events
            if any(interval["name"].endswith(suffix) for suffix in suffixes)
        ]
        if windows and matches:
            # Sum all scopes belonging to a presented host frame. This keeps
            # four AOV uploads comparable with one renderer-frame stage rather
            # than treating each texture copy as an independent frame sample.
            values = [
                round(
                    sum(
                        interval["end"] - interval["start"]
                        for interval in matches
                        if interval["start"] >= start
                        and interval["end"] <= end
                    )
                    * 1000
                )
                for start, end in windows
            ]
        else:
            # Chrome trace timestamps and durations are microseconds.
            values = [
                round((interval["end"] - interval["start"]) * 1000)
                for interval in matches
            ]
        result[stage] = values
    return result


def build_report(
    source: Path,
    events: list[dict],
    host_trace: Path | None = None,
) -> dict:
    phase_intervals: dict[str, tuple[float, float]] = {}
    trace_events: list[dict] = []
    if host_trace is not None:
        phase_intervals, trace_events = trace_intervals(host_trace)
    grouped: dict[str, list[dict]] = defaultdict(list)
    for event in events:
        grouped[event["phase"]].append(event)
    phases = []
    for name, samples in grouped.items():
        traced = trace_stage_samples(name, phase_intervals, trace_events)
        stage_report = {}
        for stage in STAGES:
            available_field = AVAILABILITY.get(stage)
            trace_values = traced.get(stage, [])
            available = bool(trace_values) or available_field is None or any(
                sample[available_field] for sample in samples
            )
            values = trace_values or [sample[stage] for sample in samples]
            stage_report[stage.removesuffix("_ns")] = {
                "available": available,
                "sample_kind": "trace_scope" if trace_values else "renderer_frame",
                "samples": len(values) if available else 0,
                "summary_ns": summarize(values) if available else None,
            }
        total = [sample["render_pass_execute_ns"] for sample in samples]
        total_summary = summarize(total)
        threshold = max(total_summary["median"] * 2,
                        total_summary["median"] + 2_000_000)
        phases.append(
            {
                "name": name,
                "samples": len(samples),
                "stages": stage_report,
                "frame_hitches": {
                    "threshold_ns": threshold,
                    "count": sum(value > threshold for value in total),
                },
                "last_counters": {
                    counter: samples[-1][counter] for counter in COUNTERS
                },
            }
        )
    return {
        "schema": "merlin-hydra-performance/v1",
        "source": str(source),
        "host_trace": str(host_trace) if host_trace is not None else None,
        "phase_order": list(grouped),
        "phases": phases,
    }


def main() -> int:
    args = parse_args()
    try:
        report = build_report(
            args.log, parse_events(args.log), host_trace=args.host_trace
        )
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"hydra-performance-report: {error}")
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
