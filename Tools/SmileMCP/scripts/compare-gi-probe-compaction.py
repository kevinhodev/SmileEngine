#!/usr/bin/env python3
"""Compara os pares visuais OFF/ON da compactacao DDGI em todas as cenas."""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True


def load_common():
    common_path = Path(__file__).with_name("compare-gi-adaptive.py")
    spec = importlib.util.spec_from_file_location("smile_gi_visual_common", common_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"nao foi possivel carregar {common_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def analyze_scene(common, scene: dict[str, object], output_directory: Path) -> dict[str, object]:
    captures = {entry["id"]: entry for entry in scene["captures"]}
    order = ["off_a", "on_a", "off_b", "on_b"]
    common.require(set(captures) == set(order),
                   f"{scene['id']}: capturas inesperadas: {sorted(captures)}")
    images = {key: common.load_rgb(Path(entry["pngPath"]))
              for key, entry in captures.items()}
    shapes = {array.shape for array in images.values()}
    common.require(len(shapes) == 1, f"{scene['id']}: resolucoes diferentes: {shapes}")

    off_mean = (images["off_a"] + images["off_b"]) * 0.5
    on_mean = (images["on_a"] + images["on_b"]) * 0.5
    comparisons = {
        "repeatOff": common.metrics(images["off_a"], images["off_b"]),
        "repeatOn": common.metrics(images["on_a"], images["on_b"]),
        "pairAOffToOn": common.metrics(images["off_a"], images["on_a"]),
        "pairBOffToOn": common.metrics(images["off_b"], images["on_b"]),
        "meanOffToOn": common.metrics(off_mean, on_mean),
    }
    repeat_mae_ceiling = max(
        float(comparisons["repeatOff"]["meanAbsolutePerChannel"]),
        float(comparisons["repeatOn"]["meanAbsolutePerChannel"]),
    )
    repeat_ssim_floor = min(
        float(comparisons["repeatOff"]["boxSsim11Luma"]),
        float(comparisons["repeatOn"]["boxSsim11Luma"]),
    )
    cross_pairs_within_repeat = all(
        float(comparisons[key]["meanAbsolutePerChannel"]) <= repeat_mae_ceiling
        and float(comparisons[key]["boxSsim11Luma"]) >= repeat_ssim_floor
        for key in ("pairAOffToOn", "pairBOffToOn")
    )
    mean_comparison = comparisons["meanOffToOn"]
    decision = {
        "crossPairsWithinWorstRepeatEnvelope": cross_pairs_within_repeat,
        "meanLumaBiasWithinOneTenthLevel":
            abs(float(mean_comparison["meanSignedLuma"])) < 0.1,
        "meanPixelsOverFiveBelowOneTenthPercent":
            float(mean_comparison["pixelsOverMaxChannelThresholdPercent"]["5"]) < 0.1,
    }
    decision["visualSupportsDefault"] = all(decision.values())

    scene_directory = output_directory / str(scene["id"])
    scene_directory.mkdir(parents=True, exist_ok=True)
    artifacts = {
        "montage": str(scene_directory / "montage.png"),
        "repeatOffDiff8x": str(scene_directory / "diff-repeat-off-8x.png"),
        "repeatOnDiff8x": str(scene_directory / "diff-repeat-on-8x.png"),
        "pairADiff8x": str(scene_directory / "diff-pair-a-8x.png"),
        "pairBDiff8x": str(scene_directory / "diff-pair-b-8x.png"),
        "meanDiff8x": str(scene_directory / "diff-mean-8x.png"),
    }
    panels = [common.labeled_panel(images[key], key) for key in order]
    width, height = panels[0].width, panels[0].height
    montage = common.Image.new("RGB", (width * 2, height * 2), "black")
    for index, panel in enumerate(panels):
        montage.paste(panel, ((index % 2) * width, (index // 2) * height))
    montage.save(artifacts["montage"])
    common.save_diff(images["off_a"], images["off_b"], Path(artifacts["repeatOffDiff8x"]))
    common.save_diff(images["on_a"], images["on_b"], Path(artifacts["repeatOnDiff8x"]))
    common.save_diff(images["off_a"], images["on_a"], Path(artifacts["pairADiff8x"]))
    common.save_diff(images["off_b"], images["on_b"], Path(artifacts["pairBDiff8x"]))
    common.save_diff(off_mean, on_mean, Path(artifacts["meanDiff8x"]))

    return {
        "id": scene["id"],
        "scenePath": scene["scenePath"],
        "resolution": {"width": images["off_a"].shape[1],
                       "height": images["off_a"].shape[0]},
        "comparisons": comparisons,
        "performanceDecision": scene.get("performanceDecision"),
        "decision": decision,
        "artifacts": artifacts,
    }


def main() -> None:
    common = load_common()
    common.require(len(sys.argv) == 2,
                   "uso: compare-gi-probe-compaction.py <relatorio.json>")
    report_path = Path(sys.argv[1]).resolve()
    report = json.loads(report_path.read_text(encoding="utf-8"))
    common.require(report.get("passed") is True,
                   "o relatorio falhou antes de completar a validacao")
    output_directory = report_path.parent / f"{report_path.stem}-analysis"
    output_directory.mkdir(parents=True, exist_ok=True)
    scenes = [analyze_scene(common, scene, output_directory) for scene in report["scenes"]]
    decision = {
        "allScenesVisualSupportDefault":
            all(scene["decision"]["visualSupportsDefault"] for scene in scenes),
        "allScenesPerformanceSupportDefault":
            all(scene["performanceDecision"]["performanceSupportsDefault"] for scene in scenes),
        "allScenesObservedPeriodicWake":
            all(scene["performanceDecision"]["wakeObservedInEveryOnProfile"] for scene in scenes),
    }
    decision["recommendProbeCompactionDefault"] = all(decision.values())
    analysis = {
        "schema": 1,
        "sourceReport": str(report_path),
        "space": "8-bit display RGB from scientific PNG; luma uses Rec.709 coefficients",
        "scenes": scenes,
        "decision": decision,
    }
    analysis_path = output_directory / "analysis.json"
    analysis_path.write_text(json.dumps(analysis, indent=2, allow_nan=False), encoding="utf-8")
    print(analysis_path)


if __name__ == "__main__":
    main()
