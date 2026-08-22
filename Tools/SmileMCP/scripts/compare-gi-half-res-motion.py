#!/usr/bin/env python3
"""Analisa continuidade temporal do ReSTIR GI half-res sob DLSS Ray Reconstruction."""

from __future__ import annotations

import importlib.util
import json
import statistics
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.dont_write_bytecode = True


def load_common():
    path = Path(__file__).with_name("compare-gi-adaptive.py")
    spec = importlib.util.spec_from_file_location("smile_gi_visual_common", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"nao foi possivel carregar {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def gaussian(image: np.ndarray, radius: int) -> np.ndarray:
    source = Image.fromarray(np.clip(image, 0.0, 255.0).astype(np.uint8), mode="RGB")
    filtered = source.filter(ImageFilter.GaussianBlur(radius=float(radius)))
    return np.asarray(filtered, dtype=np.float32)


def average_metric(entries: list[dict[str, object]], key: str) -> float:
    return statistics.fmean(float(entry[key]) for entry in entries)


def save_path_sheet(common, images: dict[str, np.ndarray], frame_ids: list[str],
                    path: Path) -> None:
    tile_width = 300
    sample = Image.fromarray(np.clip(next(iter(images.values())), 0, 255).astype(np.uint8))
    tile_height = max(1, round(sample.height * tile_width / sample.width))
    header = 28
    sheet = Image.new("RGB", (tile_width * len(frame_ids), (tile_height + header) * 2), "black")
    draw = ImageDraw.Draw(sheet)
    for row, mode in enumerate(("off", "on")):
        for column, frame_id in enumerate(frame_ids):
            key = f"{mode}_{frame_id}"
            image = Image.fromarray(np.clip(images[key], 0, 255).astype(np.uint8), mode="RGB")
            image = image.resize((tile_width, tile_height), Image.Resampling.LANCZOS)
            x = column * tile_width
            y = row * (tile_height + header)
            draw.text((x + 7, y + 7), key, fill="white")
            sheet.paste(image, (x, y + header))
    sheet.save(path)


def analyze_scene(common, scene: dict[str, object], output: Path) -> dict[str, object]:
    frame_ids = ["base"] + [f"step_{step:02d}" for step in range(1, 9)] + ["final_settled"]
    expected = {f"{mode}_{frame}" for mode in ("off", "on") for frame in frame_ids}
    captures = {entry["id"]: entry for entry in scene["captures"]}
    common.require(set(captures) == expected,
                   f"{scene['id']}: capturas inesperadas: {sorted(captures)}")
    images = {key: common.load_rgb(Path(entry["pngPath"]))
              for key, entry in captures.items()}
    common.require(len({image.shape for image in images.values()}) == 1,
                   f"{scene['id']}: resolucoes diferentes")

    cross_raw = {}
    cross_radius4 = {}
    for frame_id in frame_ids:
        off = images[f"off_{frame_id}"]
        on = images[f"on_{frame_id}"]
        cross_raw[frame_id] = common.metrics(off, on)
        cross_radius4[frame_id] = common.metrics(gaussian(off, 4), gaussian(on, 4))

    path_frames = ["base"] + [f"step_{step:02d}" for step in range(1, 9)]
    path_raw_values = [cross_raw[frame_id] for frame_id in path_frames]
    path_low_values = [cross_radius4[frame_id] for frame_id in path_frames]
    path_summary = {
        "rawMeanMaePerChannel": average_metric(path_raw_values, "meanAbsolutePerChannel"),
        "rawMeanSsim": average_metric(path_raw_values, "boxSsim11Luma"),
        "radius4MeanMaePerChannel": average_metric(path_low_values, "meanAbsolutePerChannel"),
        "radius4MeanPsnrDb": average_metric(path_low_values, "psnrDb"),
        "radius4MeanSsim": average_metric(path_low_values, "boxSsim11Luma"),
    }

    settle = {
        mode: {
            "raw": common.metrics(images[f"{mode}_step_08"],
                                  images[f"{mode}_final_settled"]),
            "gaussianRadius4": common.metrics(
                gaussian(images[f"{mode}_step_08"], 4),
                gaussian(images[f"{mode}_final_settled"], 4)),
        }
        for mode in ("off", "on")
    }
    off_raw_mae = float(settle["off"]["raw"]["meanAbsolutePerChannel"])
    on_raw_mae = float(settle["on"]["raw"]["meanAbsolutePerChannel"])
    off_low_mae = float(settle["off"]["gaussianRadius4"]["meanAbsolutePerChannel"])
    on_low_mae = float(settle["on"]["gaussianRadius4"]["meanAbsolutePerChannel"])
    settle_comparison = {
        "rawHalfToFullMaeRatio": on_raw_mae / max(off_raw_mae, 1e-6),
        "rawHalfMinusFullMae": on_raw_mae - off_raw_mae,
        "radius4HalfToFullMaeRatio": on_low_mae / max(off_low_mae, 1e-6),
        "radius4HalfMinusFullMae": on_low_mae - off_low_mae,
    }

    scene_dir = output / str(scene["id"])
    scene_dir.mkdir(parents=True, exist_ok=True)
    artifacts = {
        "pathSheet": str(scene_dir / "path-sheet.png"),
        "finalMontage": str(scene_dir / "final-immediate-vs-settled.png"),
        "offSettleDiff8x": str(scene_dir / "diff-off-immediate-to-settled-8x.png"),
        "onSettleDiff8x": str(scene_dir / "diff-on-immediate-to-settled-8x.png"),
        "finalCrossDiff8x": str(scene_dir / "diff-final-settled-off-to-on-8x.png"),
    }
    save_path_sheet(common, images, path_frames, Path(artifacts["pathSheet"]))
    montage_keys = ["off_step_08", "on_step_08", "off_final_settled", "on_final_settled"]
    panels = [common.labeled_panel(images[key], key) for key in montage_keys]
    width, height = panels[0].width, panels[0].height
    montage = Image.new("RGB", (width * 2, height * 2), "black")
    for index, panel in enumerate(panels):
        montage.paste(panel, ((index % 2) * width, (index // 2) * height))
    montage.save(artifacts["finalMontage"])
    common.save_diff(images["off_step_08"], images["off_final_settled"],
                     Path(artifacts["offSettleDiff8x"]))
    common.save_diff(images["on_step_08"], images["on_final_settled"],
                     Path(artifacts["onSettleDiff8x"]))
    common.save_diff(images["off_final_settled"], images["on_final_settled"],
                     Path(artifacts["finalCrossDiff8x"]))

    decision = {
        "pathLowFrequencySsimAtLeast098": path_summary["radius4MeanSsim"] >= 0.98,
        "pathLowFrequencyPsnrAtLeast38Db": path_summary["radius4MeanPsnrDb"] >= 38.0,
        "noMaterialExtraLowFrequencySettleError":
            on_low_mae <= max(off_low_mae * 1.25, off_low_mae + 0.05),
    }
    decision["motionPromising"] = all(decision.values())
    return {
        "id": scene["id"], "scenePath": scene["scenePath"],
        "resolution": {"width": images["off_base"].shape[1],
                       "height": images["off_base"].shape[0]},
        "crossMode": {"raw": cross_raw, "gaussianRadius4": cross_radius4,
                      "pathSummary": path_summary},
        "immediateToSettled": settle,
        "settleComparison": settle_comparison,
        "decision": decision,
        "artifacts": artifacts,
    }


def main() -> None:
    common = load_common()
    common.require(len(sys.argv) == 2,
                   "uso: compare-gi-half-res-motion.py <relatorio-motion.json>")
    report_path = Path(sys.argv[1]).resolve()
    report = json.loads(report_path.read_text(encoding="utf-8"))
    common.require(report.get("passed") is True, "a validacao nao terminou com sucesso")
    output = report_path.parent / f"{report_path.stem}-analysis"
    output.mkdir(parents=True, exist_ok=True)
    scenes = [analyze_scene(common, scene, output) for scene in report["scenes"]]
    decision = {
        "allScenesMotionPromising": all(scene["decision"]["motionPromising"]
                                         for scene in scenes)
    }
    analysis = {
        "schema": 1, "sourceReport": str(report_path),
        "space": "8-bit display RGB from gameplay DLSS RR PNG; luma Rec.709",
        "method": "OFF/ON at identical continuous-camera poses; step_08 compared with the same pose after 64 rendered frames",
        "scenes": scenes, "decision": decision,
    }
    analysis_path = output / "analysis.json"
    analysis_path.write_text(json.dumps(analysis, indent=2, allow_nan=False), encoding="utf-8")
    print(analysis_path)


if __name__ == "__main__":
    main()
