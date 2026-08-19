#!/usr/bin/env python3
"""Compara o ciclo visual full-64/adaptive gerado por gi-adaptive-visual.mjs."""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def load_rgb(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.float32)


def luminance(rgb: np.ndarray) -> np.ndarray:
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def box_mean(values: np.ndarray, radius: int = 5) -> np.ndarray:
    kernel = radius * 2 + 1
    padded = np.pad(values.astype(np.float64), radius, mode="reflect")
    integral = np.pad(padded, ((1, 0), (1, 0)), mode="constant").cumsum(0).cumsum(1)
    summed = (
        integral[kernel:, kernel:]
        - integral[:-kernel, kernel:]
        - integral[kernel:, :-kernel]
        + integral[:-kernel, :-kernel]
    )
    return (summed / float(kernel * kernel)).astype(np.float32)


def box_ssim(left: np.ndarray, right: np.ndarray) -> float:
    x = luminance(left)
    y = luminance(right)
    mean_x = box_mean(x)
    mean_y = box_mean(y)
    var_x = np.maximum(0.0, box_mean(x * x) - mean_x * mean_x)
    var_y = np.maximum(0.0, box_mean(y * y) - mean_y * mean_y)
    covariance = box_mean(x * y) - mean_x * mean_y
    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2
    numerator = (2.0 * mean_x * mean_y + c1) * (2.0 * covariance + c2)
    denominator = (mean_x * mean_x + mean_y * mean_y + c1) * (var_x + var_y + c2)
    return float(np.mean(numerator / np.maximum(denominator, 1e-12)))


def metrics(left: np.ndarray, right: np.ndarray) -> dict[str, object]:
    require(left.shape == right.shape, f"dimensoes diferentes: {left.shape} vs {right.shape}")
    signed = right - left
    absolute = np.abs(signed)
    pixel_max = np.max(absolute, axis=2)
    luma_left = luminance(left)
    luma_right = luminance(right)
    luma_delta = luma_right - luma_left
    mse = float(np.mean(signed * signed))
    height, width, _ = left.shape
    tile_rows, tile_columns = 4, 8
    tiles: list[dict[str, object]] = []
    for row in range(tile_rows):
        y0 = row * height // tile_rows
        y1 = (row + 1) * height // tile_rows
        for column in range(tile_columns):
            x0 = column * width // tile_columns
            x1 = (column + 1) * width // tile_columns
            tile = pixel_max[y0:y1, x0:x1]
            tiles.append({
                "column": column,
                "row": row,
                "x": x0,
                "y": y0,
                "width": x1 - x0,
                "height": y1 - y0,
                "meanMaxChannelAbs": float(np.mean(tile)),
                "p95MaxChannelAbs": float(np.percentile(tile, 95)),
            })
    tiles.sort(key=lambda entry: float(entry["meanMaxChannelAbs"]), reverse=True)
    return {
        "meanAbsolutePerChannel": float(np.mean(absolute)),
        "rmsePerChannel": math.sqrt(mse),
        "identical": mse == 0,
        "psnrDb": None if mse == 0 else 10.0 * math.log10((255.0 * 255.0) / mse),
        "boxSsim11Luma": box_ssim(left, right),
        "meanSignedRgb": [float(value) for value in np.mean(signed, axis=(0, 1))],
        "meanLumaLeft": float(np.mean(luma_left)),
        "meanLumaRight": float(np.mean(luma_right)),
        "meanSignedLuma": float(np.mean(luma_delta)),
        "meanAbsoluteLuma": float(np.mean(np.abs(luma_delta))),
        "p95MaxChannelAbs": float(np.percentile(pixel_max, 95)),
        "p99MaxChannelAbs": float(np.percentile(pixel_max, 99)),
        "maxChannelAbs": float(np.max(pixel_max)),
        "pixelsOverMaxChannelThresholdPercent": {
            str(threshold): float(np.mean(pixel_max > threshold) * 100.0)
            for threshold in (1, 2, 5, 10, 20)
        },
        "topTiles": tiles[:8],
    }


def save_diff(left: np.ndarray, right: np.ndarray, path: Path, gain: float = 8.0) -> None:
    absolute = np.max(np.abs(right - left), axis=2)
    scaled = np.clip(absolute * gain, 0.0, 255.0).astype(np.uint8)
    # Preto -> azul -> ciano -> amarelo -> branco, mantendo zero realmente preto.
    normalized = scaled.astype(np.float32) / 255.0
    red = np.clip((normalized - 0.45) * 2.2, 0.0, 1.0)
    green = np.clip(normalized * 1.8, 0.0, 1.0)
    blue = np.clip((0.65 - np.abs(normalized - 0.35)) * 2.0, 0.0, 1.0)
    heat = np.stack((red, green, blue), axis=2)
    heat[scaled == 0] = 0.0
    Image.fromarray((heat * 255.0).astype(np.uint8), mode="RGB").save(path)


def labeled_panel(array: np.ndarray, label: str) -> Image.Image:
    image = Image.fromarray(np.clip(array, 0.0, 255.0).astype(np.uint8), mode="RGB")
    panel = Image.new("RGB", (image.width, image.height + 32), "black")
    panel.paste(image, (0, 32))
    draw = ImageDraw.Draw(panel)
    draw.text((10, 9), label, fill="white")
    return panel


def save_montage(images: dict[str, np.ndarray], path: Path) -> None:
    order = ["full64_a", "adaptive_a", "full64_b", "adaptive_b"]
    panels = [labeled_panel(images[key], key) for key in order]
    width = panels[0].width
    height = panels[0].height
    montage = Image.new("RGB", (width * 2, height * 2), "black")
    for index, panel in enumerate(panels):
        montage.paste(panel, ((index % 2) * width, (index // 2) * height))
    montage.save(path)


def main() -> None:
    require(len(sys.argv) == 2, "uso: compare-gi-adaptive.py <relatorio-visual.json>")
    report_path = Path(sys.argv[1]).resolve()
    report = json.loads(report_path.read_text(encoding="utf-8"))
    require(report.get("passed") is True, "o relatorio de captura nao passou")
    captures = {entry["id"]: entry for entry in report["captures"]}
    expected = {"full64_a", "adaptive_a", "full64_b", "adaptive_b"}
    require(set(captures) == expected, f"capturas inesperadas: {sorted(captures)}")
    images = {key: load_rgb(Path(entry["pngPath"])) for key, entry in captures.items()}
    shapes = {array.shape for array in images.values()}
    require(len(shapes) == 1, f"resolucoes diferentes: {shapes}")

    full64_mean = (images["full64_a"] + images["full64_b"]) * 0.5
    adaptive_mean = (images["adaptive_a"] + images["adaptive_b"]) * 0.5
    comparisons = {
        "repeatFull64": metrics(images["full64_a"], images["full64_b"]),
        "repeatAdaptive": metrics(images["adaptive_a"], images["adaptive_b"]),
        "pairAFull64ToAdaptive": metrics(images["full64_a"], images["adaptive_a"]),
        "pairBFull64ToAdaptive": metrics(images["full64_b"], images["adaptive_b"]),
        "meanFull64ToAdaptive": metrics(full64_mean, adaptive_mean),
    }
    repeat_mae_ceiling = max(
        float(comparisons["repeatFull64"]["meanAbsolutePerChannel"]),
        float(comparisons["repeatAdaptive"]["meanAbsolutePerChannel"]),
    )
    repeat_ssim_floor = min(
        float(comparisons["repeatFull64"]["boxSsim11Luma"]),
        float(comparisons["repeatAdaptive"]["boxSsim11Luma"]),
    )
    cross_pairs_within_repeat = all(
        float(comparisons[key]["meanAbsolutePerChannel"]) <= repeat_mae_ceiling
        and float(comparisons[key]["boxSsim11Luma"]) >= repeat_ssim_floor
        for key in ("pairAFull64ToAdaptive", "pairBFull64ToAdaptive")
    )
    mean_comparison = comparisons["meanFull64ToAdaptive"]
    decision = {
        "crossPairsWithinWorstRepeatEnvelope": cross_pairs_within_repeat,
        "meanLumaBiasWithinOneTenthLevel": abs(float(mean_comparison["meanSignedLuma"])) < 0.1,
        "meanPixelsOverFiveBelowOneTenthPercent":
            float(mean_comparison["pixelsOverMaxChannelThresholdPercent"]["5"]) < 0.1,
    }
    decision["recommendAdaptiveDefault"] = all(decision.values())

    output_directory = report_path.parent / f"{report_path.stem}-analysis"
    output_directory.mkdir(parents=True, exist_ok=True)
    artifacts = {
        "montage": str(output_directory / "montage.png"),
        "repeatFull64Diff8x": str(output_directory / "diff-repeat-full64-8x.png"),
        "repeatAdaptiveDiff8x": str(output_directory / "diff-repeat-adaptive-8x.png"),
        "pairADiff8x": str(output_directory / "diff-pair-a-8x.png"),
        "pairBDiff8x": str(output_directory / "diff-pair-b-8x.png"),
        "meanDiff8x": str(output_directory / "diff-mean-8x.png"),
    }
    save_montage(images, Path(artifacts["montage"]))
    save_diff(images["full64_a"], images["full64_b"], Path(artifacts["repeatFull64Diff8x"]))
    save_diff(images["adaptive_a"], images["adaptive_b"], Path(artifacts["repeatAdaptiveDiff8x"]))
    save_diff(images["full64_a"], images["adaptive_a"], Path(artifacts["pairADiff8x"]))
    save_diff(images["full64_b"], images["adaptive_b"], Path(artifacts["pairBDiff8x"]))
    save_diff(full64_mean, adaptive_mean, Path(artifacts["meanDiff8x"]))

    analysis = {
        "schema": 1,
        "sourceReport": str(report_path),
        "resolution": {"width": images["full64_a"].shape[1], "height": images["full64_a"].shape[0]},
        "space": "8-bit display RGB from scientific PNG; luma uses Rec.709 coefficients",
        "comparisons": comparisons,
        "decision": decision,
        "artifacts": artifacts,
    }
    analysis_path = output_directory / "analysis.json"
    analysis_path.write_text(json.dumps(analysis, indent=2, allow_nan=False), encoding="utf-8")
    print(analysis_path)


if __name__ == "__main__":
    main()
