#!/usr/bin/env python3
"""Compara o ciclo visual full/interleaved gerado por gi-interleaved.mjs."""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True


def load_common():
    common_path = Path(__file__).with_name("compare-gi-adaptive.py")
    spec = importlib.util.spec_from_file_location("smile_gi_visual_common", common_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"nao foi possivel carregar {common_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> None:
    common = load_common()
    common.require(len(sys.argv) == 2,
                   "uso: compare-gi-interleaved.py <relatorio-visual.json>")
    report_path = Path(sys.argv[1]).resolve()
    report = json.loads(report_path.read_text(encoding="utf-8"))
    source_passed = report.get("passed") is True
    source_error = str(report.get("error", ""))
    log_audit_error = (
        "log do editor PID" in source_error
        or "smile_get_latest_logs" in source_error
    )
    log_only_failure = (
        not source_passed
        and log_audit_error
        and len(report.get("scheduleSamples", [])) >= 24
        and len(report.get("edgeCases", [])) >= 4
        and len(report.get("captures", [])) == 4
        and bool(report.get("performanceDecision", {}).get("phaseScopesValid", False))
    )
    common.require(source_passed or log_only_failure,
                   "o relatorio falhou antes de completar a validacao visual")
    captures = {entry["id"]: entry for entry in report["captures"]}
    order = ["full_a", "interleaved_a", "full_b", "interleaved_b"]
    common.require(set(captures) == set(order), f"capturas inesperadas: {sorted(captures)}")
    images = {key: common.load_rgb(Path(entry["pngPath"])) for key, entry in captures.items()}
    shapes = {array.shape for array in images.values()}
    common.require(len(shapes) == 1, f"resolucoes diferentes: {shapes}")

    full_mean = (images["full_a"] + images["full_b"]) * 0.5
    interleaved_mean = (images["interleaved_a"] + images["interleaved_b"]) * 0.5
    comparisons = {
        "repeatFull": common.metrics(images["full_a"], images["full_b"]),
        "repeatInterleaved": common.metrics(
            images["interleaved_a"], images["interleaved_b"]),
        "pairAFullToInterleaved": common.metrics(
            images["full_a"], images["interleaved_a"]),
        "pairBFullToInterleaved": common.metrics(
            images["full_b"], images["interleaved_b"]),
        "meanFullToInterleaved": common.metrics(full_mean, interleaved_mean),
    }
    repeat_mae_ceiling = max(
        float(comparisons["repeatFull"]["meanAbsolutePerChannel"]),
        float(comparisons["repeatInterleaved"]["meanAbsolutePerChannel"]),
    )
    repeat_ssim_floor = min(
        float(comparisons["repeatFull"]["boxSsim11Luma"]),
        float(comparisons["repeatInterleaved"]["boxSsim11Luma"]),
    )
    cross_pairs_within_repeat = all(
        float(comparisons[key]["meanAbsolutePerChannel"]) <= repeat_mae_ceiling
        and float(comparisons[key]["boxSsim11Luma"]) >= repeat_ssim_floor
        for key in ("pairAFullToInterleaved", "pairBFullToInterleaved")
    )
    mean_comparison = comparisons["meanFullToInterleaved"]
    decision = {
        "crossPairsWithinWorstRepeatEnvelope": cross_pairs_within_repeat,
        "meanLumaBiasWithinOneTenthLevel":
            abs(float(mean_comparison["meanSignedLuma"])) < 0.1,
        "meanPixelsOverFiveBelowOneTenthPercent":
            float(mean_comparison["pixelsOverMaxChannelThresholdPercent"]["5"]) < 0.1,
    }
    performance_supports = bool(
        report.get("performanceDecision", {}).get("performanceSupportsDefault", False))
    decision["performanceSupportsDefault"] = performance_supports
    decision["recommendInterleavedDefault"] = all(decision.values())

    output_directory = report_path.parent / f"{report_path.stem}-analysis"
    output_directory.mkdir(parents=True, exist_ok=True)
    artifacts = {
        "montage": str(output_directory / "montage.png"),
        "repeatFullDiff8x": str(output_directory / "diff-repeat-full-8x.png"),
        "repeatInterleavedDiff8x":
            str(output_directory / "diff-repeat-interleaved-8x.png"),
        "pairADiff8x": str(output_directory / "diff-pair-a-8x.png"),
        "pairBDiff8x": str(output_directory / "diff-pair-b-8x.png"),
        "meanDiff8x": str(output_directory / "diff-mean-8x.png"),
    }
    panels = [common.labeled_panel(images[key], key) for key in order]
    width, height = panels[0].width, panels[0].height
    montage = common.Image.new("RGB", (width * 2, height * 2), "black")
    for index, panel in enumerate(panels):
        montage.paste(panel, ((index % 2) * width, (index // 2) * height))
    montage.save(artifacts["montage"])
    common.save_diff(images["full_a"], images["full_b"], Path(artifacts["repeatFullDiff8x"]))
    common.save_diff(images["interleaved_a"], images["interleaved_b"],
                     Path(artifacts["repeatInterleavedDiff8x"]))
    common.save_diff(images["full_a"], images["interleaved_a"],
                     Path(artifacts["pairADiff8x"]))
    common.save_diff(images["full_b"], images["interleaved_b"],
                     Path(artifacts["pairBDiff8x"]))
    common.save_diff(full_mean, interleaved_mean, Path(artifacts["meanDiff8x"]))

    analysis = {
        "schema": 1,
        "sourceReport": str(report_path),
        "sourceValidation": {
            "passed": source_passed,
            "logOnlyFailureAcceptedForVisualAnalysis": log_only_failure,
            "error": source_error if not source_passed else None,
        },
        "resolution": {"width": images["full_a"].shape[1],
                       "height": images["full_a"].shape[0]},
        "space": "8-bit display RGB from scientific PNG; luma uses Rec.709 coefficients",
        "comparisons": comparisons,
        "performanceDecision": report.get("performanceDecision"),
        "decision": decision,
        "artifacts": artifacts,
    }
    analysis_path = output_directory / "analysis.json"
    analysis_path.write_text(json.dumps(analysis, indent=2, allow_nan=False), encoding="utf-8")
    print(analysis_path)


if __name__ == "__main__":
    main()
