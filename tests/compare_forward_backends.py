"""Compare independently captured Vulkan and Metal Forward center responses."""

import argparse
import json
from pathlib import Path


PROFILE = "forward-sh-l2-reinhard/v1"
NATIVE_PHASES = (
    "off-static", "on-static", "on-moving", "off-moving", "off-restored",
    "on-restored", "zero-energy", "environment-up", "environment-down",
    "environment-side",
)
HOST_PHASES = tuple(
    f"lighting-{albedo}-{output}-{light}-{pose}"
    for albedo in ("color", "white")
    for output in ("linear", "srgb")
    for light in ("off", "on")
    for pose in ("static", "moving", "restored")
)
OUTPUT_PHASES = tuple(
    f"output-{output}-{tone}-ev{ev}"
    for output in ("linear", "srgb")
    for tone in ("none", "reinhard")
    for ev in (-2, 0, 1, 4)
)


def phases(report, schema, key):
    if report.get("schema") != schema or report.get("lighting_profile") != PROFILE:
        raise ValueError("unsupported schema or stale lighting profile")
    if report.get("off_policy") != "unit-white-plus-z-diagnostic-fallback":
        raise ValueError("OFF semantics differ")
    result = {}
    for phase in report["phases"]:
        name = phase[key]
        if name in result:
            raise ValueError(f"duplicate phase: {name}")
        rgb = phase["center_rgb"]
        minimum = 0 if name.startswith("environment-") else 1
        if len(rgb) != 3 or any(type(c) is not int or not minimum <= c < 255 for c in rgb):
            raise ValueError(f"invalid RGB or lost headroom: {name}")
        if phase["upload_bytes"] != 0 or phase["pipeline_creation_count"] != 0:
            raise ValueError(f"resources rebuilt: {name}")
        result[name] = rgb
    return result


def compare(a, b, required, tolerance):
    if not set(required) <= a.keys() or not set(required) <= b.keys():
        raise ValueError("missing required phases")
    results = []
    for name in required:
        error = max(abs(x - y) for x, y in zip(a[name], b[name]))
        if error > tolerance:
            raise ValueError(f"{name}: Vulkan {a[name]} != Metal {b[name]}")
        results.append({"phase": name, "vulkan_rgb": a[name], "metal_rgb": b[name],
                        "maximum_channel_error": error})
    return results


def compare_native(vulkan, metal):
    for report in (vulkan, metal):
        if report.get("color_aov") != "linear-rgba8-with-embedded-reinhard":
            raise ValueError("native output transforms differ")
    return compare(
        phases(vulkan, "merlin-forward-image-fixture/v1", "name"),
        phases(metal, "merlin-metal-forward-lighting/v1", "name"), NATIVE_PHASES, 1)


def compare_host(vulkan, metal):
    for report, backend in ((vulkan, "vulkan"), (metal, "metal")):
        if report.get("backend") != backend or report.get("white_off_policy") != "headroom-required":
            raise ValueError("host backend or headroom policy differs")
    return compare(
        phases(vulkan, "merlin-usdview-forward-lighting/v1", "phase"),
        phases(metal, "merlin-usdview-forward-lighting/v1", "phase"), HOST_PHASES, 2)


def output_phases(report):
    if report.get("schema") != "merlin-usdview-forward-output/v1":
        raise ValueError("unsupported output schema")
    result = {}
    for phase in report["phases"]:
        name = phase["phase"]
        if name in result:
            raise ValueError(f"duplicate phase: {name}")
        rgb = phase["center_rgb"]
        if len(rgb) != 3 or any(type(c) is not int or not 0 <= c <= 255 for c in rgb):
            raise ValueError(f"invalid RGB: {name}")
        if phase["upload_bytes"] != 0 or phase["pipeline_creation_count"] != 0:
            raise ValueError(f"resources rebuilt: {name}")
        result[name] = rgb
    return result


def compare_output(vulkan, metal):
    return compare(output_phases(vulkan), output_phases(metal), OUTPUT_PHASES, 2)


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("vulkan_native", type=Path)
    parser.add_argument("metal_native", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--vulkan-host", type=Path)
    parser.add_argument("--metal-host", type=Path)
    parser.add_argument("--vulkan-output", type=Path)
    parser.add_argument("--metal-output", type=Path)
    args = parser.parse_args()
    if bool(args.vulkan_host) != bool(args.metal_host):
        parser.error("both host reports are required for a host comparison")
    if bool(args.vulkan_output) != bool(args.metal_output):
        parser.error("both output reports are required for an output comparison")
    result = {"schema": "merlin-forward-backend-comparison/v1",
              "lighting_profile": PROFILE, "native_center_tolerance": 1,
              "host_center_tolerance": 2, "native": {}, "host": None, "output": None}
    for albedo in ("color", "white"):
        result["native"][albedo] = compare_native(
            read(args.vulkan_native / albedo / "comparison.json"),
            read(args.metal_native / albedo / "comparison.json"))
    if args.vulkan_host:
        result["host"] = compare_host(read(args.vulkan_host), read(args.metal_host))
    if args.vulkan_output:
        result["output"] = compare_output(read(args.vulkan_output), read(args.metal_output))
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
