import importlib.util
import json
import sys
from pathlib import Path

import numpy as np


def load_common():
    source = Path(__file__).with_name("compare-gi-adaptive.py")
    spec = importlib.util.spec_from_file_location("smile_image_compare", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def metrics(left, right):
    absolute = np.abs(left.astype(np.float32) - right.astype(np.float32))
    per_pixel = np.max(absolute, axis=2)
    return {
        "meanAbs": float(np.mean(absolute)),
        "p99MaxChannelAbs": float(np.percentile(per_pixel, 99)),
        "maxChannelAbs": float(np.max(per_pixel)),
        "pixelsOver5Percent": float(np.mean(per_pixel > 5.0) * 100.0),
    }


def main():
    require(len(sys.argv) == 2, "uso: compare-gpu-driven-phase5.py <relatorio.json>")
    report_path = Path(sys.argv[1]).resolve()
    report = json.loads(report_path.read_text(encoding="utf-8"))
    require(report.get("passed") is True, "o aparelho de runtime nao passou")
    common = load_common()
    comparisons = {}

    for scene in report["scenes"]:
        captures = {entry["label"]: entry for entry in scene["captures"]}
        images = {
            key: common.load_rgb(Path(value["pngPath"]))
            for key, value in captures.items()
        }
        require(len({image.shape for image in images.values()}) == 1,
                f"{scene['key']}: resolucoes diferentes")

        off_pair = metrics(images["off_a"], images["off_b"])
        off_mean = (images["off_a"].astype(np.float32) +
                    images["off_b"].astype(np.float32)) * 0.5
        on_vs_mean = metrics(images["on"], off_mean)
        # O piso e o proprio OFF/A -> OFF/B. A captura ON fica entre as duas no tempo, entao a
        # media e a referencia temporal mais justa. A folga minima cobre quantizacao de 8 bits.
        require(on_vs_mean["meanAbs"] <= max(0.75, off_pair["meanAbs"] * 1.25),
                f"{scene['key']}: media absoluta ON excede o piso OFF ({on_vs_mean} vs {off_pair})")
        require(on_vs_mean["pixelsOver5Percent"] <=
                max(0.05, off_pair["pixelsOver5Percent"] * 1.25),
                f"{scene['key']}: pixels divergentes ON excedem o piso OFF ({on_vs_mean} vs {off_pair})")
        comparisons[scene["key"]] = {
            "offA_vs_offB": off_pair,
            "on_vs_meanOff": on_vs_mean,
        }

    report["imageComparison"] = comparisons
    output = report_path.with_name(report_path.stem + "-compared.json")
    output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
