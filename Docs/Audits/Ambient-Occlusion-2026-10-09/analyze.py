"""Rebuild comparison sheets and measured summaries from the saved AO captures."""
from pathlib import Path
import csv
import hashlib
import json
import statistics
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
FONT = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 23)
SMALL = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 17)
MATCHED_FIELDS = (
    "outputWidth", "outputHeight", "renderWidth", "renderHeight", "renderScale",
    "taa", "upscaler", "upscalerQuality", "denoiser", "warmupFrames",
    "temporalSampleIndex", "historyReset", "cameraX", "cameraY", "cameraZ",
    "pitchDeg", "yawDeg", "fovYDeg", "sunX", "sunY", "sunZ", "timeOfDayHours",
    "timeOfDayEnabled", "pinnedTimeOfDayRequested", "pinnedTimeOfDayApplied",
    "useGI", "restirGI", "reflections", "indirectPrimaryEffective",
    "indirectFallbackEffective",
)


def capture(directory, label):
    paths = sorted(directory.glob(label + "_game*.json"))
    if len(paths) != 1:
        raise RuntimeError(f"Expected one {label} capture in {directory}, found {len(paths)}")
    manifest = json.loads(paths[0].read_text(encoding="utf-8"))
    return Image.open(paths[0].with_suffix(".png")).convert("RGB"), manifest


def timing(directory):
    with (directory / "gpu.csv").open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    result = {}
    for mode in ("full", "half"):
        chosen = [r for r in rows if r["mode"] == mode]
        values = [float(r["gtaoMilliseconds"]) for r in chosen]
        assert len(values) == 32 and len({r["sourceFrame"] for r in chosen}) == 32
        result[mode] = dict(samples=len(values), meanMs=statistics.mean(values),
                            medianMs=statistics.median(values), minMs=min(values), maxMs=max(values))
    return result


def sheet(scene):
    dirs = [ROOT / f"{scene}-original", ROOT / f"{scene}-revised"]
    if not all((d / "complete.txt").exists() for d in dirs):
        return None
    labels = [("full-ao", "AO full-res"), ("half-ao", "AO half-res reconstruido"),
              ("taa-full", "Composto com TAA")]
    width, height = 1280, 720
    out = Image.new("RGB", (width * 2, 3 * (height + 40) + 92), "#f5f5f5")
    draw = ImageDraw.Draw(out)
    draw.text((18, 8), f"{scene.capitalize()} | GTAO original e revisado", font=FONT, fill="#171717")
    draw.text((18, 43), "Mesma camera e iluminacao, 128 frames de aquecimento. Esquerda: original. Direita: revisao.",
              font=SMALL, fill="#424242")
    for row, (label, title) in enumerate(labels):
        y = 85 + row * (height + 40)
        draw.text((18, y), title, font=FONT, fill="#171717")
        manifests = []
        for col, d in enumerate(dirs):
            image, manifest = capture(d, label)
            assert image.size == (width, height)
            manifests.append(manifest)
            out.paste(image, (col * width, y + 34))
        for key in MATCHED_FIELDS:
            assert manifests[0][key] == manifests[1][key], (scene, label, key)
    target = ROOT / f"{scene}-comparison.png"
    out.save(target)
    a, _ = capture(dirs[0], "ao-off")
    b, _ = capture(dirs[1], "ao-off")
    delta = np.asarray(a, dtype=np.float64) - np.asarray(b, dtype=np.float64)
    rmse = float(np.sqrt(np.mean(delta * delta)))
    compared_labels = []
    for path in sorted(dirs[0].glob("*_game*.json")):
        label = path.name.split("_game", 1)[0]
        _, first = capture(dirs[0], label)
        _, second = capture(dirs[1], label)
        for key in MATCHED_FIELDS:
            assert first[key] == second[key], (scene, label, key)
        compared_labels.append(label)
    return dict(comparison=target.name, original=timing(dirs[0]), revised=timing(dirs[1]),
                aoOffControl=dict(rmse8Bit=rmse, maxAbsolute8Bit=float(np.max(np.abs(delta)))),
                capturesPerArm=len(list(dirs[0].glob("*.png"))),
                matchedCaptureLabels=compared_labels, matchedManifestFields=MATCHED_FIELDS,
                note="Shader A/B on revised C++ orchestration. Timings are one sequential 32-frame block per mode.")


summary = {s: data for s in ("bistro", "emerald") if (data := sheet(s)) is not None}
summary["shaderHashes"] = {}
for arm, folder in (("original", "ao-original"), ("revised", "ao-revised")):
    shader_dir = ROOT.parents[2] / "tmp" / folder
    if shader_dir.exists():
        summary["shaderHashes"][arm] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                         for p in sorted(shader_dir.glob("*.cso"))}
(ROOT / "analysis.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
print(json.dumps(summary, indent=2))
