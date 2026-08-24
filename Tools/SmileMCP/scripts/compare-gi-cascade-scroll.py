#!/usr/bin/env python3
"""Analisa a rota A/B de cascatas produzida por gi-cascade-scroll-validation.mjs."""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def load_common():
    source = Path(__file__).with_name("compare-gi-adaptive.py")
    spec = importlib.util.spec_from_file_location("smile_gi_visual_common", source)
    require(spec is not None and spec.loader is not None, f"nao foi possivel carregar {source}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def labeled(image: Image.Image, label: str, width: int = 420) -> Image.Image:
    height = round(image.height * width / image.width)
    resized = image.resize((width, height), Image.Resampling.LANCZOS)
    panel = Image.new("RGB", (width, height + 28), "black")
    panel.paste(resized, (0, 28))
    ImageDraw.Draw(panel).text((8, 8), label, fill="white")
    return panel


def montage(report: dict, output: Path, selected_steps: list[int]) -> None:
    rows = []
    for regime in report["regimes"]:
        captures = {int(entry["id"].rsplit("_", 1)[1]): entry for entry in regime["captures"]}
        rows.append([
            labeled(Image.open(captures[step]["pngPath"]).convert("RGB"),
                    f"{regime['id']} / step {step}")
            for step in selected_steps
        ])
    cell_width = rows[0][0].width
    cell_height = rows[0][0].height
    sheet = Image.new("RGB", (cell_width * len(selected_steps), cell_height * len(rows)), "black")
    for row_index, row in enumerate(rows):
        for column_index, panel in enumerate(row):
            sheet.paste(panel, (column_index * cell_width, row_index * cell_height))
    sheet.save(output)


def heatmap(left: np.ndarray, right: np.ndarray, gain: float = 8.0) -> Image.Image:
    absolute = np.max(np.abs(right - left), axis=2)
    scaled = np.clip(absolute * gain, 0.0, 255.0).astype(np.uint8)
    normalized = scaled.astype(np.float32) / 255.0
    red = np.clip((normalized - 0.45) * 2.2, 0.0, 1.0)
    green = np.clip(normalized * 1.8, 0.0, 1.0)
    blue = np.clip((0.65 - np.abs(normalized - 0.35)) * 2.0, 0.0, 1.0)
    rgb = np.stack((red, green, blue), axis=2)
    rgb[scaled == 0] = 0.0
    return Image.fromarray((rgb * 255.0).astype(np.uint8), mode="RGB")


def diff_montage(images: dict[str, dict[int, np.ndarray]], output: Path,
                 selected_steps: list[int]) -> None:
    comparisons = [
        ("single", "dual_all", "single -> dual_all"),
    ]
    if "triple_all" in images:
        comparisons.append(("dual_all", "triple_all", "dual_all -> triple_all"))
    comparisons.append(("dual_all", "dual_interleaved", "dual_all -> interleaved"))
    rows = [[
        labeled(heatmap(images[left][step], images[right][step]), f"{label} / step {step}")
        for step in selected_steps
    ] for left, right, label in comparisons]
    cell_width = rows[0][0].width
    cell_height = rows[0][0].height
    sheet = Image.new("RGB", (cell_width * len(selected_steps), cell_height * len(rows)), "black")
    for row_index, row in enumerate(rows):
        for column_index, panel in enumerate(row):
            sheet.paste(panel, (column_index * cell_width, row_index * cell_height))
    sheet.save(output)


def baseline_diff_montage(baseline: dict[str, dict[int, np.ndarray]],
                          candidate: dict[str, dict[int, np.ndarray]], output: Path,
                          selected_steps: list[int]) -> None:
    regime_ids = [regime_id for regime_id in
                  ("single", "dual_all", "triple_all", "dual_interleaved")
                  if regime_id in candidate and regime_id in baseline]
    rows = [[
        labeled(heatmap(baseline[regime_id][step], candidate[regime_id][step]),
                f"before -> after {regime_id} / step {step}")
        for step in selected_steps
    ] for regime_id in regime_ids]
    cell_width = rows[0][0].width
    cell_height = rows[0][0].height
    sheet = Image.new("RGB", (cell_width * len(selected_steps), cell_height * len(rows)), "black")
    for row_index, row in enumerate(rows):
        for column_index, panel in enumerate(row):
            sheet.paste(panel, (column_index * cell_width, row_index * cell_height))
    sheet.save(output)


def main() -> None:
    require(len(sys.argv) in (2, 3),
            "uso: compare-gi-cascade-scroll.py <relatorio.json> [baseline.json]")
    common = load_common()
    report_path = Path(sys.argv[1]).resolve()
    report = json.loads(report_path.read_text(encoding="utf-8"))
    require(report.get("passed") is True, "captura nao passou")
    regimes = {entry["id"]: entry for entry in report["regimes"]}
    require({"single", "dual_all", "dual_interleaved"} <= regimes.keys(), "regimes ausentes")

    images: dict[str, dict[int, np.ndarray]] = {}
    for regime_id, regime in regimes.items():
        images[regime_id] = {
            index: common.load_rgb(Path(capture["pngPath"]))
            for index, capture in enumerate(regime["captures"])
        }

    comparisons = []
    for step in range(len(regimes["single"]["captures"])):
        single_to_dual = common.metrics(images["single"][step], images["dual_all"][step])
        all_to_interleaved = common.metrics(
            images["dual_all"][step], images["dual_interleaved"][step])
        comparison = {
            "step": step,
            "singleToDualAll": single_to_dual,
            "dualAllToInterleaved": all_to_interleaved,
        }
        if "triple_all" in regimes:
            comparison["dualAllToTripleAll"] = common.metrics(
                images["dual_all"][step], images["triple_all"][step])
            comparison["singleToTripleAll"] = common.metrics(
                images["single"][step], images["triple_all"][step])
        comparisons.append(comparison)

    transitions = []
    placement_samples = regimes.get("triple_all", regimes["dual_all"])["samples"]
    for step in range(1, len(placement_samples)):
        before = placement_samples[step - 1]["status"]["ddgi"]["cascades"][0]
        after = placement_samples[step]["status"]["ddgi"]["cascades"][0]
        if before["gridMin"] != after["gridMin"]:
            transitions.append(step)

    def summarize(key: str) -> dict:
        values = [entry[key] for entry in comparisons]
        return {
            "meanSsim": float(np.mean([value["boxSsim11Luma"] for value in values])),
            "minSsim": float(np.min([value["boxSsim11Luma"] for value in values])),
            "meanAbsolutePerChannel": float(np.mean([
                value["meanAbsolutePerChannel"] for value in values
            ])),
            "meanPixelsOverFivePercent": float(np.mean([
                value["pixelsOverMaxChannelThresholdPercent"]["5"] for value in values
            ])),
        }

    output_base = report_path.with_suffix("")
    analysis_path = Path(str(output_base) + "-analysis.json")
    contact_path = Path(str(output_base) + "-contact-sheet.jpg")
    diff_path = Path(str(output_base) + "-diff-x8.jpg")
    analysis = {
        "schema": 1,
        "sourceReport": str(report_path),
        "fineScrollTransitionSteps": transitions,
        "summary": {
            "singleToDualAll": summarize("singleToDualAll"),
            "dualAllToInterleaved": summarize("dualAllToInterleaved"),
        },
        "comparisons": comparisons,
    }
    if "triple_all" in regimes:
        analysis["summary"]["dualAllToTripleAll"] = summarize("dualAllToTripleAll")
        analysis["summary"]["singleToTripleAll"] = summarize("singleToTripleAll")
    selected = sorted(set([0, *transitions, len(comparisons) - 1]))
    baseline_diff_path = None
    if len(sys.argv) == 3:
        baseline_path = Path(sys.argv[2]).resolve()
        baseline_report = json.loads(baseline_path.read_text(encoding="utf-8"))
        require(baseline_report.get("passed") is True, "baseline nao passou")
        baseline_regimes = {entry["id"]: entry for entry in baseline_report["regimes"]}
        common_regime_ids = regimes.keys() & baseline_regimes.keys()
        require({"single", "dual_all", "dual_interleaved"} <= common_regime_ids,
                "regimes comuns ausentes na baseline")
        baseline_images = {
            regime_id: {
                index: common.load_rgb(Path(capture["pngPath"]))
                for index, capture in enumerate(baseline_regimes[regime_id]["captures"])
            }
            for regime_id in common_regime_ids
        }
        baseline_comparisons = {}
        for regime_id in ("single", "dual_all", "triple_all", "dual_interleaved"):
            if regime_id not in common_regime_ids:
                continue
            values = [common.metrics(baseline_images[regime_id][step], images[regime_id][step])
                      for step in range(len(images[regime_id]))]
            baseline_comparisons[regime_id] = {
                "meanSsim": float(np.mean([value["boxSsim11Luma"] for value in values])),
                "meanAbsolutePerChannel": float(np.mean([
                    value["meanAbsolutePerChannel"] for value in values
                ])),
                "perStep": values,
            }
        analysis["baselineComparison"] = {
            "sourceReport": str(baseline_path),
            "regimes": baseline_comparisons,
        }
        if "triple_all" in regimes and "dual_all" in baseline_regimes:
            values = [common.metrics(baseline_images["dual_all"][step],
                                     images["triple_all"][step])
                      for step in range(len(images["triple_all"]))]
            analysis["baselineComparison"]["baselineDualToCandidateTriple"] = {
                "meanSsim": float(np.mean([value["boxSsim11Luma"] for value in values])),
                "meanAbsolutePerChannel": float(np.mean([
                    value["meanAbsolutePerChannel"] for value in values
                ])),
                "meanSignedLuma": float(np.mean([value["meanSignedLuma"] for value in values])),
                "perStep": values,
            }
        baseline_diff_path = Path(str(output_base) + "-baseline-diff-x8.jpg")
        baseline_diff_montage(baseline_images, images, baseline_diff_path, selected)
    analysis_path.write_text(json.dumps(analysis, indent=2, allow_nan=False), encoding="utf-8")
    montage(report, contact_path, selected)
    diff_montage(images, diff_path, selected)
    print(analysis_path)
    print(contact_path)
    print(diff_path)
    if baseline_diff_path is not None:
        print(baseline_diff_path)


if __name__ == "__main__":
    main()
