"""Apply a conservative, known-motion Wiener deconvolution to captured JPEGs.

The capture sidecars contain ``expected_blur_px``.  Because the fixture pans
horizontally, that value is used as the length of a horizontal motion PSF.
This can improve appearance, but it cannot recreate detail lost to blur; keep
the original download for quantitative measurements.
"""
from __future__ import annotations

import argparse
import csv
import shutil
from pathlib import Path
import numpy as np
from PIL import Image, ImageFile

ImageFile.LOAD_TRUNCATED_IMAGES = False

def expected_length(path: Path) -> float:
    try:
        with Path(str(path) + ".csv").open(newline="") as f:
            row = next(csv.DictReader(f))
        return max(0.0, float(row.get("expected_blur_px", 0) or 0))
    except (OSError, StopIteration, ValueError, csv.Error):
        return 0.0

def wiener_channel(channel: np.ndarray, length: float, regularization: float) -> np.ndarray:
    if length < 1.0:
        return channel.astype(np.float32)
    # A centred horizontal line PSF models the servo pan during exposure.
    n = max(3, int(round(length)) | 1)
    psf = np.zeros_like(channel, dtype=np.float32)
    mid = psf.shape[1] // 2
    half = n // 2
    psf[0, max(0, mid-half):min(psf.shape[1], mid+half+1)] = 1.0 / n
    H = np.fft.fft2(psf)
    F = np.fft.fft2(channel.astype(np.float32))
    restored = np.fft.ifft2(F * np.conj(H) / (np.abs(H) ** 2 + regularization)).real
    return np.clip(restored, 0, 255).astype(np.float32)

def deblur(source: Path, destination: Path, max_length: float, regularization: float) -> bool:
    try:
        with Image.open(source) as image:
            rgb = np.asarray(image.convert("RGB"), dtype=np.float32)
    except (OSError, ValueError):
        return False
    length = min(expected_length(source), max_length)
    if length >= 1.0:
        # Deconvolve luminance and apply its correction to all RGB channels,
        # avoiding colour fringing from three independent inversions.
        y = 0.299*rgb[..., 0] + 0.587*rgb[..., 1] + 0.114*rgb[..., 2]
        restored = wiener_channel(y, length, regularization)
        delta = restored - y
        rgb = np.clip(rgb + delta[..., None], 0, 255)
    destination.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.rint(rgb).astype(np.uint8), "RGB").save(destination, quality=95, subsampling=0)
    sidecar = Path(str(source) + ".csv")
    if sidecar.exists(): shutil.copy2(sidecar, Path(str(destination) + ".csv"))
    return True

def main() -> None:
    p = argparse.ArgumentParser(description="Known-motion Wiener deblur for captured JPEGs")
    p.add_argument("folder", type=Path, nargs="?", help="download folder (defaults to newest download)")
    p.add_argument("--output", type=Path, help="output folder (default: <folder>_deblurred)")
    p.add_argument("--max-length", type=float, default=30.0, help="maximum PSF length in pixels (default: 30)")
    p.add_argument("--regularization", type=float, default=0.10, help="Wiener regularization; higher suppresses ringing (default: 0.10)")
    a = p.parse_args()
    if a.folder is None:
        candidates = sorted((Path(__file__).resolve().parent / "retrieved_images").glob("download_*"), key=lambda x: x.stat().st_mtime, reverse=True)
        candidates = [x for x in candidates if x.is_dir() and "_deblurred" not in x.name]
        if not candidates: p.error("No download folder found under retrieved_images")
        a.folder = candidates[0]
        print(f"Using newest download: {a.folder}")
    source = a.folder.resolve()
    if not source.is_dir(): p.error(f"Folder does not exist: {source}")
    output = (a.output or source.with_name(source.name + "_deblurred")).resolve()
    if output == source: p.error("Output folder must differ from input folder")
    done = skipped = 0
    for image in sorted(source.rglob("*.jpg")):
        if deblur(image, output / image.relative_to(source), a.max_length, a.regularization): done += 1
        else: skipped += 1
    manifest = source / "transfer_manifest.json"
    if manifest.exists(): shutil.copy2(manifest, output / manifest.name)
    print(f"Deblurred {done} images; skipped {skipped}")
    print(f"Output: {output}")

if __name__ == "__main__":
    main()
