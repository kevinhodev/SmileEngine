"""Compare image noise and measured critical-path distributions of the Phase 6 A/B/A."""
import importlib.util
import json
import sys
from pathlib import Path


def main():
    path = Path(sys.argv[1]).resolve()
    source = Path(__file__).with_name("compare-gpu-driven-phase5.py")
    spec = importlib.util.spec_from_file_location("phase5_compare", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.main()  # Same capture schema; writes -compared.json and rejects image differences.
    compared = path.with_name(path.stem + "-compared.json")
    report = json.loads(compared.read_text(encoding="utf-8"))
    output = {}
    for scene in report["scenes"]:
        cases = {entry["label"]: entry["profile"] for entry in scene["cases"]}
        frame = {
            key: next(p["rawMilliseconds"] for p in value["passes"] if p["name"] == "Frame (GPU)")
            for key, value in cases.items()
        }
        # OFF/OFF brackets the ON measurement in the same process. Drift allowance is measured,
        # not a universal percentage of GPU time. Retain raw distributions in the report.
        drift = abs(frame["off_a"]["median"] - frame["off_b"]["median"])
        floor = max(frame["off_a"]["p95"], frame["off_b"]["p95"])
        on = frame["on"]
        good = on["median"] <= floor and on["p95"] <= floor + drift
        output[scene["key"]] = {
            "frameGpuMs": frame, "offMedianDriftMs": drift,
            "p95CeilingMs": floor + drift, "passed": good,
        }
        if not good:
            report["performanceComparison"] = output
            compared.write_text(json.dumps(report, indent=2), encoding="utf-8")
            raise RuntimeError(f"{scene['key']}: critical path regression: {output[scene['key']]}")
    report["performanceComparison"] = output
    compared.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Phase 6: pixels and critical-path gate passed")


if __name__ == "__main__":
    main()
