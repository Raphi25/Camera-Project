"""Discover downloaded captures and build stable UI catalog entries."""

from dataclasses import dataclass
from pathlib import Path


JPEG_SUFFIXES = frozenset({".jpg", ".jpeg"})


@dataclass(frozen=True)
class ImageCatalogEntry:
    path: Path
    category: str

    @property
    def display_text(self) -> str:
        parent = self.path.parent.name
        return f"[{self.category}] [{parent}] {self.path.name}"


@dataclass(frozen=True)
class ImageCatalog:
    entries: tuple[ImageCatalogEntry, ...]
    normal_count: int
    burst_count: int

    @property
    def preview_status(self) -> str:
        if not self.entries:
            return "No transferred images found.\nUse Get SD Media first."
        return (
            f"{self.normal_count} image(s), {self.burst_count} burst image(s)\n"
            "Select one to preview."
        )

    def path_at(self, index: int) -> Path | None:
        if index < 0 or index >= len(self.entries):
            return None
        return self.entries[index].path


def _discover(directory: Path) -> list[Path]:
    if not directory.exists():
        return []
    try:
        return [
            path
            for path in directory.rglob("*")
            if path.is_file() and path.suffix.lower() in JPEG_SUFFIXES
        ]
    except OSError:
        return []


def build_image_catalog(
    normal_directory: Path, burst_directory: Path
) -> ImageCatalog:
    normal = sorted(_discover(normal_directory), key=lambda path: path.name, reverse=True)
    burst = sorted(_discover(burst_directory), key=lambda path: path.name, reverse=True)
    entries = tuple(
        [ImageCatalogEntry(path, "Image") for path in normal]
        + [ImageCatalogEntry(path, "Burst") for path in burst]
    )
    return ImageCatalog(entries, len(normal), len(burst))
