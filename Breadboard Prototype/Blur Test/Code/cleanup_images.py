"""Create a cleaned copy of a TinyUSB image download.

This is a conservative enhancement pass: mild colour denoising followed by
unsharp masking. It cannot recover detail that motion blur removed, so the
original download is never modified. JPEG sidecar metadata is copied beside
each processed image so the result can be analysed with analyze_blur.py.
"""
from __future__ import annotations

import argparse
import shutil
from pathlib import Path

import cv2
from PIL import Image, ImageFile

ImageFile.LOAD_TRUNCATED_IMAGES = False


def clean_image(source: Path, destination: Path, denoise: float, sharpen: float) -> bool:
    # Pillow's verify catches the truncated JPEGs that OpenCV may decode with
    # warnings and partial pixel data.
    try:
        with Image.open(source) as checked:
            checked.verify()
        # verify() checks headers; load() also detects truncated entropy data.
        with Image.open(source) as checked:
            checked.load()
    except (OSError, ValueError):
        return False
    image = cv2.imread(str(source), cv2.IMREAD_COLOR)
    if image is None:
        return False

    # fastNlMeansColored is deliberately kept mild so edges are not erased.
    if denoise > 0:
        d = max(1, int(round(denoise)))
        image = cv2.fastNlMeansDenoisingColored(image, None, d, d, 7, 21)

    if sharpen > 0:
        softened = cv2.GaussianBlur(image, (0, 0), 1.0)
        image = cv2.addWeighted(image, 1.0 + sharpen, softened, -sharpen, 0)

    destination.parent.mkdir(parents=True, exist_ok=True)
    return bool(cv2.imwrite(str(destination), image, [cv2.IMWRITE_JPEG_QUALITY, 95]))


def main() -> None:
    parser = argparse.ArgumentParser(description="Make a cleaned copy of captured JPEGs")
    parser.add_argument("folder", type=Path, nargs="?", help="TinyUSB download folder (defaults to newest download)")
    parser.add_argument("--output", type=Path,
                        help="Output folder (default: <folder>_cleaned)")
    parser.add_argument("--denoise", type=float, default=3.0,
                        help="Colour denoising strength; 0 disables it (default: 3)")
    parser.add_argument("--sharpen", type=float, default=0.55,
                        help="Unsharp-mask amount; 0 disables it (default: 0.55)")
    args = parser.parse_args()

    if args.folder is None:
        candidates = sorted(
            (Path(__file__).resolve().parent / "retrieved_images").glob("download_*")
        , key=lambda path: path.stat().st_mtime, reverse=True)
        candidates = [path for path in candidates if path.is_dir() and
                      not any(tag in path.name for tag in ("_cleaned", "_sharpened"))]
        if not candidates:
            parser.error("No download folder found under retrieved_images")
        args.folder = candidates[0]
        print(f"Using newest download: {args.folder}")
    source_root = args.folder.resolve()
    if not source_root.is_dir():
        parser.error(f"Folder does not exist: {source_root}")
    output_root = (args.output or source_root.with_name(source_root.name + "_cleaned")).resolve()
    if output_root == source_root:
        parser.error("Output folder must differ from the input folder")

    processed = skipped = 0
    for source in sorted(source_root.rglob("*.jpg")):
        relative = source.relative_to(source_root)
        destination = output_root / relative
        if clean_image(source, destination, args.denoise, args.sharpen):
            processed += 1
            sidecar = Path(str(source) + ".csv")
            if sidecar.exists():
                shutil.copy2(sidecar, Path(str(destination) + ".csv"))
        else:
            skipped += 1
            print(f"Skipped unreadable JPEG: {relative}")

    # Keep the transfer manifest available for provenance, but do not copy the
    # old analysis outputs because they describe the unprocessed images.
    manifest = source_root / "transfer_manifest.json"
    if manifest.exists():
        shutil.copy2(manifest, output_root / manifest.name)

    print(f"Cleaned {processed} images; skipped {skipped} unreadable images")
    print(f"Output: {output_root}")
    print("Run blur analysis on the output folder to compare the metrics.")


if __name__ == "__main__":
    main()
