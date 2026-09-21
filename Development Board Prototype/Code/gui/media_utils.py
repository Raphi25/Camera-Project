"""Local media validation, MJPEG repair, and optional FFmpeg discovery."""

import datetime as dt
import os
import re
import shutil
import tempfile
from pathlib import Path

from PIL import Image, ImageFilter, ImageStat


_CAPTURE_DATE_PATTERNS = (
    re.compile(r"^(?P<year>\d{4})y(?P<month>\d{2})m(?P<day>\d{2})d(?:_|$)", re.IGNORECASE),
    re.compile(r"^(?P<year>\d{4})(?P<month>\d{2})(?P<day>\d{2})(?:_|$)"),
)


def capture_date_folder(name: str) -> str | None:
    """Return YYYY-MM-DD for current and legacy timestamped media names."""
    filename = Path(name).name
    for pattern in _CAPTURE_DATE_PATTERNS:
        match = pattern.match(filename)
        if match:
            year = int(match.group("year"))
            month = int(match.group("month"))
            day = int(match.group("day"))
            try:
                dt.date(year, month, day)
            except ValueError:
                return None
            return f"{year:04d}-{month:02d}-{day:02d}"
    return None


def is_jpeg_file_name(name: str) -> bool:
    return name.lower().endswith((".jpg", ".jpeg"))


def is_mjpeg_file_name(name: str) -> bool:
    return name.lower().endswith((".mjpeg", ".mjpg"))


def local_media_matches(path: Path, expected_size: int | None) -> bool:
    """Return whether a completed local file matches the advertised size."""
    if expected_size is None or expected_size <= 0:
        return False
    try:
        return path.is_file() and path.stat().st_size == expected_size
    except OSError:
        return False


_BURST_GROUP_PATTERN = re.compile(
    r"^(?P<prefix>.+)_burst_(?P<index>\d+)\.(?:jpg|jpeg)$", re.IGNORECASE
)


def _image_sharpness_score(path: Path) -> float:
    """Estimate focus from edge-strength variance without changing the JPEG."""
    with Image.open(path) as image:
        gray = image.convert("L")
        if gray.width > 640:
            height = max(1, round(gray.height * 640 / gray.width))
            gray = gray.resize((640, height), Image.Resampling.BILINEAR)
        edges = gray.filter(ImageFilter.FIND_EDGES)
        # FIND_EDGES exaggerates the outer image boundary, which is unrelated
        # to focus. Exclude a small border before measuring detail variance.
        border = min(4, max(0, min(edges.size) // 8))
        if border:
            edges = edges.crop((border, border, edges.width - border, edges.height - border))
        return float(ImageStat.Stat(edges).var[0])


def promote_sharpest_burst_images(
    burst_directory: Path,
    image_directory: Path,
    expected_count: int = 3,
) -> list[tuple[Path, Path, float]]:
    """Copy the sharpest member of every complete burst into Images.

    Existing promoted files are left untouched, making repeated serial/OTG
    retrieval idempotent. The source burst images are deliberately retained.
    """
    if not burst_directory.exists():
        return []

    # A burst can cross a one-second timestamp boundary. Build groups from the
    # explicit _01, _02, _03 suffix sequence rather than identical timestamps.
    groups: list[tuple[Path, list[Path]]] = []
    for source_directory in sorted(
        (path for path in burst_directory.rglob("*") if path.is_dir()),
        key=str,
    ) + [burst_directory]:
        candidates: list[tuple[str, int, Path]] = []
        for path in source_directory.iterdir():
            if not path.is_file():
                continue
            match = _BURST_GROUP_PATTERN.match(path.name)
            if match is not None:
                candidates.append(
                    (match.group("prefix").lower(), int(match.group("index")), path)
                )
        candidates.sort(key=lambda entry: (entry[0], entry[1]))
        current: list[Path] = []
        next_index = 1
        for _prefix, index, path in candidates:
            if index == 1:
                current = [path]
                next_index = 2
            elif current and index == next_index:
                current.append(path)
                next_index += 1
                if len(current) == expected_count:
                    groups.append((source_directory, current))
                    current = []
                    next_index = 1

    promoted: list[tuple[Path, Path, float]] = []
    for source_directory, members in groups:
        try:
            scored = [(path, _image_sharpness_score(path)) for path in members]
        except (OSError, ValueError):
            continue
        source, score = max(scored, key=lambda entry: entry[1])
        relative_parent = source_directory.relative_to(burst_directory)
        destination = image_directory / relative_parent / source.name
        if destination.exists() and destination.stat().st_size > 0:
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                delete=False,
                dir=destination.parent,
                prefix=f".{destination.name}.",
                suffix=".part",
            ) as temp_file:
                temporary = Path(temp_file.name)
            shutil.copyfile(source, temporary)
            os.replace(temporary, destination)
            temporary = None
            promoted.append((source, destination, score))
        finally:
            discard_local_temp(temporary)
    return promoted


def discard_local_temp(path: Path | None) -> None:
    """Best-effort cleanup for an uncommitted transfer file."""
    if path is None:
        return
    try:
        path.unlink(missing_ok=True)
    except OSError:
        pass


def extract_clean_mjpeg_frames(payload: bytes) -> tuple[bytes, int, int]:
    """Keep complete JPEG frames and count discarded boundary fragments."""
    frames: list[bytes] = []
    pos = 0
    dropped = 0

    while pos < len(payload):
        soi = payload.find(b"\xFF\xD8", pos)
        if soi < 0:
            dropped += len(payload) - pos
            break
        if soi > pos:
            dropped += soi - pos

        eoi = payload.find(b"\xFF\xD9", soi + 2)
        if eoi < 0:
            dropped += len(payload) - soi
            break

        end = eoi + 2
        frames.append(payload[soi:end])
        pos = end

    return b"".join(frames), len(frames), dropped


def find_ffmpeg_executable() -> str | None:
    """Find FFmpeg on PATH or in WinGet's per-user package directory."""
    found = shutil.which("ffmpeg")
    if found:
        return found

    local_appdata = os.environ.get("LOCALAPPDATA")
    if not local_appdata:
        return None

    winget_packages = Path(local_appdata) / "Microsoft" / "WinGet" / "Packages"
    if not winget_packages.exists():
        return None

    candidates = sorted(
        winget_packages.glob("Gyan.FFmpeg*/*/bin/ffmpeg.exe"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    return str(candidates[0]) if candidates else None
