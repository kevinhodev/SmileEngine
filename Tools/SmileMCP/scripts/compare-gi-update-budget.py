import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def load_rgb(path: str) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def block_ssim(a: np.ndarray, b: np.ndarray, block: int = 8) -> float:
    height = min(a.shape[0], b.shape[0]) // block * block
    width = min(a.shape[1], b.shape[1]) // block * block
    a = a[:height, :width]
    b = b[:height, :width]
    a = a.reshape(height // block, block, width // block, block).transpose(0, 2, 1, 3)
    b = b.reshape(height // block, block, width // block, block).transpose(0, 2, 1, 3)
    mean_a = a.mean(axis=(2, 3))
    mean_b = b.mean(axis=(2, 3))
    var_a = a.var(axis=(2, 3))
    var_b = b.var(axis=(2, 3))
    covariance = ((a - mean_a[:, :, None, None]) *
                  (b - mean_b[:, :, None, None])).mean(axis=(2, 3))
    c1 = 0.01 ** 2
    c2 = 0.03 ** 2
    score = ((2 * mean_a * mean_b + c1) * (2 * covariance + c2)) / (
        (mean_a ** 2 + mean_b ** 2 + c1) * (var_a + var_b + c2)
    )
    return float(np.mean(score))


def metrics(reference: np.ndarray, candidate: np.ndarray) -> dict:
    assert reference.shape == candidate.shape, (reference.shape, candidate.shape)
    diff = np.abs(reference - candidate)
    luma_weights = np.asarray([0.2126, 0.7152, 0.0722], dtype=np.float32)
    ref_luma = reference @ luma_weights
    cand_luma = candidate @ luma_weights
    luma_diff = np.abs(ref_luma - cand_luma)
    mse = float(np.mean((reference - candidate) ** 2))
    return {
        "maeRgb": float(np.mean(diff)),
        "maeLuma": float(np.mean(luma_diff)),
        "p95LumaAbs": float(np.percentile(luma_diff, 95)),
        "psnrDb": float(99.0 if mse == 0 else 10 * math.log10(1.0 / mse)),
        "ssimLuma8x8": block_ssim(ref_luma, cand_luma),
        "pixelsAbove2of255Percent": float(np.mean(luma_diff > 2 / 255) * 100),
        "pixelsAbove5of255Percent": float(np.mean(luma_diff > 5 / 255) * 100),
    }


def pass_stats(profile: dict, name: str) -> dict:
    return next(entry["rawMilliseconds"] for entry in profile["passes"] if entry["name"] == name)


def mean(values: list[float]) -> float:
    return float(sum(values) / len(values))


def scaled_image(path: str, width: int) -> Image.Image:
    image = Image.open(path).convert("RGB")
    height = round(image.height * width / image.width)
    return image.resize((width, height), Image.Resampling.LANCZOS)


def label(draw: ImageDraw.ImageDraw, position: tuple[int, int], text: str) -> None:
    draw.text(position, text, fill=(235, 235, 235), font=ImageFont.load_default())


def main() -> None:
    report_path = Path(sys.argv[1]).resolve()
    report = json.loads(report_path.read_text(encoding="utf-8"))
    cases = report["cases"]
    baseline = cases[0]
    ref_static = load_rgb(baseline["staticCapture"]["pngPath"])
    ref_scroll = load_rgb(baseline["scroll"]["pngPath"])

    analysis = {
        "schema": 1,
        "sourceReport": str(report_path),
        "reference": baseline["id"],
        "cases": [],
    }
    if len(sys.argv) >= 4:
        repeat_static = load_rgb(sys.argv[2])
        repeat_scroll = load_rgb(sys.argv[3])
        analysis["independentLegacyNoiseFloor"] = {
            "staticPath": str(Path(sys.argv[2]).resolve()),
            "scrollPath": str(Path(sys.argv[3]).resolve()),
            "static": metrics(ref_static, repeat_static),
            "scroll": metrics(ref_scroll, repeat_scroll),
        }
    legacy_ddgi = mean([pass_stats(profile, "DDGI")["mean"]
                        for profile in baseline["profiles"]])
    legacy_frame = mean([pass_stats(profile, "Frame (GPU)")["mean"]
                         for profile in baseline["profiles"]])
    for case in cases:
        case_static = load_rgb(case["staticCapture"]["pngPath"])
        case_scroll = load_rgb(case["scroll"]["pngPath"])
        ddgi_stats = [pass_stats(profile, "DDGI") for profile in case["profiles"]]
        frame_stats = [pass_stats(profile, "Frame (GPU)") for profile in case["profiles"]]
        ddgi_mean = mean([entry["mean"] for entry in ddgi_stats])
        frame_mean = mean([entry["mean"] for entry in frame_stats])
        scheduled = [sample["scheduledProbeCount"] for sample in case["schedule"]]
        active = [sample["activeProbeCount"] for sample in case["schedule"]]
        analysis["cases"].append({
            "id": case["id"],
            "budget": case["budget"],
            "performance": {
                "ddgiMeanMilliseconds": ddgi_mean,
                "ddgiMedianMilliseconds": mean([entry["median"] for entry in ddgi_stats]),
                "ddgiP95Milliseconds": mean([entry["p95"] for entry in ddgi_stats]),
                "ddgiSavedMilliseconds": legacy_ddgi - ddgi_mean,
                "ddgiSavedPercent": (legacy_ddgi - ddgi_mean) * 100 / legacy_ddgi,
                "frameMeanMilliseconds": frame_mean,
                "frameMedianMilliseconds": mean([entry["median"] for entry in frame_stats]),
                "frameP95Milliseconds": mean([entry["p95"] for entry in frame_stats]),
                "frameSavedMilliseconds": legacy_frame - frame_mean,
                "frameSavedPercent": (legacy_frame - frame_mean) * 100 / legacy_frame,
            },
            "schedule": {
                "scheduledProbeCountMean": mean(scheduled),
                "activeProbeCountMean": mean(active),
                "activeProbeCountMin": min(active),
                "activeProbeCountMax": max(active),
                "totalProbes": case["steady"]["ddgi"]["totalProbes"],
            },
            "static": metrics(ref_static, case_static),
            "scroll": metrics(ref_scroll, case_scroll),
        })

    output_json = report_path.with_name(report_path.stem + "-image-analysis.json")
    output_json.write_text(json.dumps(analysis, indent=2), encoding="utf-8")

    thumb_width = 520
    first_thumb = scaled_image(cases[0]["staticCapture"]["pngPath"], thumb_width)
    row_height = first_thumb.height + 42
    sheet = Image.new("RGB", (thumb_width * 2 + 30, row_height * len(cases)), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for row, case in enumerate(cases):
        y = row * row_height
        static_image = scaled_image(case["staticCapture"]["pngPath"], thumb_width)
        scroll_image = scaled_image(case["scroll"]["pngPath"], thumb_width)
        label(draw, (10, y + 8), f"{case['id']} | static convergida")
        label(draw, (thumb_width + 20, y + 8), f"{case['id']} | apos scroll")
        sheet.paste(static_image, (10, y + 30))
        sheet.paste(scroll_image, (thumb_width + 20, y + 30))
    contact_path = report_path.with_name(report_path.stem + "-contact-sheet.png")
    sheet.save(contact_path)

    diff_cases = cases[1:]
    diff_sheet = Image.new("RGB", (thumb_width * 2 + 30, row_height * len(diff_cases)), (24, 24, 24))
    diff_draw = ImageDraw.Draw(diff_sheet)
    for row, case in enumerate(diff_cases):
        y = row * row_height
        static_diff = np.clip(np.abs(load_rgb(case["staticCapture"]["pngPath"]) - ref_static) * 8, 0, 1)
        scroll_diff = np.clip(np.abs(load_rgb(case["scroll"]["pngPath"]) - ref_scroll) * 8, 0, 1)
        static_image = Image.fromarray((static_diff * 255).astype(np.uint8)).resize(
            (thumb_width, first_thumb.height), Image.Resampling.LANCZOS)
        scroll_image = Image.fromarray((scroll_diff * 255).astype(np.uint8)).resize(
            (thumb_width, first_thumb.height), Image.Resampling.LANCZOS)
        label(diff_draw, (10, y + 8), f"{case['id']} - legado | static | erro x8")
        label(diff_draw, (thumb_width + 20, y + 8), f"{case['id']} - legado | scroll | erro x8")
        diff_sheet.paste(static_image, (10, y + 30))
        diff_sheet.paste(scroll_image, (thumb_width + 20, y + 30))
    diff_path = report_path.with_name(report_path.stem + "-diff-x8.png")
    diff_sheet.save(diff_path)

    print(json.dumps({
        "analysisPath": str(output_json),
        "contactSheetPath": str(contact_path),
        "diffSheetPath": str(diff_path),
        "analysis": analysis,
    }))


if __name__ == "__main__":
    main()
