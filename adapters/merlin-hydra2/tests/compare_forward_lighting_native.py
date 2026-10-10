"""Compare the same world-normal response before native/host presentation."""

import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("native", type=Path)
    parser.add_argument("host", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    host = json.loads(args.host.read_text(encoding="utf-8"))
    if host["schema"] != "merlin-usdview-forward-lighting/v1":
        raise ValueError("unsupported host lighting report")
    host_phases = {phase["phase"]: phase for phase in host["phases"]}
    comparisons = []
    for albedo in ("color", "white"):
        native = json.loads((args.native / albedo / "comparison.json").read_text(encoding="utf-8"))
        if native["schema"] != "merlin-forward-image-fixture/v1":
            raise ValueError("unsupported native lighting report")
        native_phases = {phase["name"]: phase for phase in native["phases"]}
        if native["off_policy"] != host["off_policy"]:
            raise ValueError("native/host OFF semantics differ")
        for light in ("off", "on"):
            for pose in ("static", "moving", "restored"):
                phase = f"{light}-{pose}"
                a = native_phases[phase]["center_rgb"]
                b = host_phases[f"lighting-{albedo}-linear-{phase}"]["center_rgb"]
                if len(a) != 3 or len(b) != 3:
                    raise ValueError("expected three color channels")
                error = max(abs(x - y) for x, y in zip(a, b))
                if error > 1:
                    raise ValueError(f"{albedo}/{phase}: native {a} != host {b}")
                comparisons.append({"albedo": albedo, "phase": phase,
                                    "native_rgb": a, "host_rgb": b,
                                    "maximum_channel_error": error})
    args.output.write_text(json.dumps({
        "schema": "merlin-forward-native-host-comparison/v1",
        "center_channel_tolerance": 1, "comparisons": comparisons,
    }, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
