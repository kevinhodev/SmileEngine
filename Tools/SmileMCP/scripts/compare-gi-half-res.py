#!/usr/bin/env python3
"""Compara ReSTIR GI nativo contra half-res em todas as cenas da rodada."""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

sys.dont_write_bytecode = True


def load_common():
    path = Path(__file__).with_name("compare-gi-adaptive.py")
    spec = importlib.util.spec_from_file_location("smile_gi_visual_common", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"nao foi possivel carregar {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def gaussian_low_frequency(image: np.ndarray, radius: int) -> np.ndarray:
    source = Image.fromarray(np.clip(image, 0.0, 255.0).astype(np.uint8), mode="RGB")
    filtered = source.filter(ImageFilter.GaussianBlur(radius=float(radius)))
    return np.asarray(filtered, dtype=np.float32)


def analyze_scene(common, scene: dict[str, object], output: Path) -> dict[str, object]:
    captures = {entry["id"]: entry for entry in scene["captures"]}
    order = ["off_a", "on_a", "on_b", "off_b"]
    common.require(set(captures) == set(order),
                   f"{scene['id']}: capturas inesperadas: {sorted(captures)}")
    images = {key: common.load_rgb(Path(captures[key]["pngPath"])) for key in order}
    common.require(len({image.shape for image in images.values()}) == 1,
                   f"{scene['id']}: resolucoes diferentes")

    off_mean = (images["off_a"] + images["off_b"]) * 0.5
    on_mean = (images["on_a"] + images["on_b"]) * 0.5
    comparisons = {
        "repeatOff": common.metrics(images["off_a"], images["off_b"]),
        "repeatOn": common.metrics(images["on_a"], images["on_b"]),
        "pairAOffToOn": common.metrics(images["off_a"], images["on_a"]),
        "pairBOffToOn": common.metrics(images["off_b"], images["on_b"]),
        "meanOffToOn": common.metrics(off_mean, on_mean),
    }
    low_frequency = {
        f"gaussianRadius{radius}": common.metrics(
            gaussian_low_frequency(off_mean, radius),
            gaussian_low_frequency(on_mean, radius))
        for radius in (2, 4, 8)
    }
    mean_cmp = comparisons["meanOffToOn"]
    left_luma = max(abs(float(mean_cmp["meanLumaLeft"])), 1e-6)
    energy_delta_percent = 100.0 * abs(float(mean_cmp["meanSignedLuma"])) / left_luma
    visual = {
        "meanEnergyDeltaPercent": energy_delta_percent,
        "meanEnergyWithinOnePercent": energy_delta_percent < 1.0,
        "psnrAtLeast40Db": float(mean_cmp["psnrDb"]) >= 40.0,
        "ssimAtLeast0995": float(mean_cmp["boxSsim11Luma"]) >= 0.995,
        "pixelsOverFiveBelowOnePercent":
            float(mean_cmp["pixelsOverMaxChannelThresholdPercent"]["5"]) < 1.0,
    }
    visual["visualPromising"] = all(value for key, value in visual.items()
                                      if key != "meanEnergyDeltaPercent")

    scene_dir = output / str(scene["id"])
    scene_dir.mkdir(parents=True, exist_ok=True)
    artifacts = {
        "montage": str(scene_dir / "montage.png"),
        "repeatOffDiff8x": str(scene_dir / "diff-repeat-off-8x.png"),
        "repeatOnDiff8x": str(scene_dir / "diff-repeat-on-8x.png"),
        "pairADiff8x": str(scene_dir / "diff-pair-a-8x.png"),
        "pairBDiff8x": str(scene_dir / "diff-pair-b-8x.png"),
        "meanDiff8x": str(scene_dir / "diff-mean-8x.png"),
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
        "id": scene["id"], "scenePath": scene["scenePath"],
        "resolution": {"width": images["off_a"].shape[1],
                       "height": images["off_a"].shape[0]},
        "comparisons": comparisons, "lowFrequencyComparisons": low_frequency,
        "visualDecision": visual,
        "performanceDecision": scene["performanceDecision"], "artifacts": artifacts,
    }


def main() -> None:
    common = load_common()
    common.require(len(sys.argv) == 2, "uso: compare-gi-half-res.py <relatorio.json>")
    report_path = Path(sys.argv[1]).resolve()
    report = json.loads(report_path.read_text(encoding="utf-8"))
    common.require(report.get("passed") is True, "a validacao nao terminou com sucesso")
    output = report_path.parent / f"{report_path.stem}-analysis"
    output.mkdir(parents=True, exist_ok=True)
    scenes = [analyze_scene(common, scene, output) for scene in report["scenes"]]
    decision = {
        "allScenesPerformancePromising": all(
            scene["performanceDecision"]["performancePromising"] for scene in scenes),
        "allScenesVisualPromising": all(
            scene["visualDecision"]["visualPromising"] for scene in scenes),
    }
    decision["prototypePromising"] = all(decision.values())
    preset = report.get("regime", {}).get("visual", {}).get("preset", "unknown")
    analysis = {"schema": 1, "sourceReport": str(report_path),
                "space": f"8-bit display RGB from {preset} PNG; luma Rec.709",
                "scenes": scenes, "decision": decision}
    analysis_path = output / "analysis.json"
    analysis_path.write_text(json.dumps(analysis, indent=2, allow_nan=False), encoding="utf-8")
    print(analysis_path)


if __name__ == "__main__":
    main()
