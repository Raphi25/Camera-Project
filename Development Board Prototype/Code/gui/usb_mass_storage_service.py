"""Automatic Windows USB Mass Storage retrieval for the camera SD card."""

from __future__ import annotations

import ctypes
import hashlib
import os
import shutil
import string
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from collections.abc import Callable

from media_utils import capture_date_folder, promote_sharpest_burst_images


DRIVE_REMOVABLE = 2
DRIVE_FIXED = 3
OTG_APPEAR_TIMEOUT_SECONDS = 10.0
INVENTORY_STABLE_TIMEOUT_SECONDS = 20.0
INVENTORY_STABLE_POLLS = 3
FILE_IO_RETRIES = 5
IMAGE_CRYPTO_MAGIC = 0x31474D49
IMAGE_HEADER = struct.Struct("<IHHI16s12s16s")


@dataclass(frozen=True)
class UsbMassStorageResult:
    used: bool
    copied: int = 0
    skipped: int = 0
    bytes_copied: int = 0
    drive: str | None = None


@dataclass(frozen=True)
class UsbMassStorageDeleteResult:
    used: bool
    deleted: int = 0
    failed: int = 0
    drive: str | None = None


def _windows_volumes() -> set[Path]:
    """Return accessible data-volume roots without probing network drives."""
    if os.name != "nt":
        return set()
    mask = ctypes.windll.kernel32.GetLogicalDrives()
    roots: set[Path] = set()
    for index, letter in enumerate(string.ascii_uppercase):
        if not mask & (1 << index):
            continue
        root = f"{letter}:\\"
        if ctypes.windll.kernel32.GetDriveTypeW(root) in (DRIVE_REMOVABLE, DRIVE_FIXED):
            roots.add(Path(root))
    return roots


def _find_camera_volume(candidates: set[Path]) -> Path | None:
    """Identify the exported SD card by its firmware-created captures folder."""
    for root in sorted(candidates, key=str):
        try:
            if (root / "captures").is_dir():
                return root
        except OSError:
            pass
    return None


def _wait_for_camera_volume(previous: set[Path]) -> Path | None:
    deadline = time.monotonic() + OTG_APPEAR_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        current = _windows_volumes()
        volume = _find_camera_volume(current - previous)
        if volume is None:
            # The firmware enumerates an MSC standby device so it can report
            # OTG connectivity before transfer. Windows may therefore reserve
            # its drive letter before USB_MSC_START makes the card readable.
            volume = _find_camera_volume(current)
        if volume is not None:
            return volume
        time.sleep(0.25)
    return None


def _copy_or_decrypt(source: Path, temporary: Path, media_password: bytes) -> None:
    """Copy legacy/plain files or decrypt the firmware's IMG1 AES-GCM format."""
    with source.open("rb") as handle:
        header = handle.read(IMAGE_HEADER.size)
        if len(header) != IMAGE_HEADER.size:
            shutil.copyfile(source, temporary)
            return
        magic, version, _reserved, plain_len, salt, nonce, tag = IMAGE_HEADER.unpack(header)
        if magic != IMAGE_CRYPTO_MAGIC or version != 1:
            shutil.copyfile(source, temporary)
            return
        ciphertext = handle.read()
    if len(ciphertext) != plain_len:
        raise ValueError(f"Invalid encrypted-media length in {source.name}")
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError as exc:
        raise RuntimeError("The packaged GUI is missing its AES-GCM support") from exc
    key = hashlib.pbkdf2_hmac("sha256", media_password, salt, 10000, dklen=32)
    plaintext = AESGCM(key).decrypt(nonce, ciphertext + tag, None)
    temporary.write_bytes(plaintext)


def _scan_stable_inventory(volume: Path) -> list[Path]:
    """Wait for Windows to return the same complete-looking file list repeatedly."""
    deadline = time.monotonic() + INVENTORY_STABLE_TIMEOUT_SECONDS
    previous: tuple[tuple[str, int], ...] | None = None
    stable_polls = 0
    while time.monotonic() < deadline:
        captures = volume / "captures"
        if not captures.is_dir():
            previous = None
            stable_polls = 0
            time.sleep(0.5)
            continue
        try:
            files = sorted(
                (path for path in captures.rglob("*") if path.is_file()),
                key=lambda path: str(path).lower(),
            )
            signature = tuple(
                (str(path.relative_to(volume)).lower(), path.stat().st_size)
                for path in files
            )
        except OSError:
            previous = None
            stable_polls = 0
            time.sleep(0.5)
            continue
        if signature and signature == previous:
            stable_polls += 1
            if stable_polls >= INVENTORY_STABLE_POLLS:
                return files
        else:
            previous = signature
            stable_polls = 1
        time.sleep(0.5)
    raise RuntimeError(
        "The OTG drive inventory did not remain stable; check the USB cable and retry."
    )


def _copy_with_retry(source: Path, temporary: Path, media_password: bytes) -> int:
    """Copy one camera file, tolerating short Windows removable-drive glitches."""
    last_error: OSError | ValueError | RuntimeError | None = None
    for attempt in range(1, FILE_IO_RETRIES + 1):
        try:
            source_size = source.stat().st_size
            _copy_or_decrypt(source, temporary, media_password)
            return source_size
        except (OSError, ValueError, RuntimeError) as exc:
            last_error = exc
            if attempt < FILE_IO_RETRIES:
                time.sleep(0.5 * attempt)
    raise RuntimeError(
        f"Could not read {source.name} after {FILE_IO_RETRIES} attempts: {last_error}"
    ) from last_error


def _copy_all(volume: Path, media_root: Path, media_password: bytes,
              progress: Callable[[int, int, str], None]) -> tuple[int, int, int]:
    progress(0, 0, "USB Mass Storage: verifying SD-card inventory...")
    files = _scan_stable_inventory(volume)
    expected_inventory = {
        str(path.relative_to(volume)).lower(): path.stat().st_size for path in files
    }
    copied = skipped = byte_count = 0
    total = len(files)
    started = time.monotonic()
    for index, source in enumerate(files, 1):
        relative = source.relative_to(volume)
        # Known media goes into the existing GUI libraries; every other SD file
        # is retained under SD Card Files with its original directory structure.
        name = source.name.lower()
        if name.startswith("summary_") and source.suffix.lower() == ".txt":
            target = media_root / "Summaries" / source.name
        elif source.suffix.lower() in (".jpg", ".jpeg", ".png"):
            image_root = media_root / ("Burst Images" if "burst" in name else "Images")
            capture_date = capture_date_folder(source.name)
            target = image_root / capture_date / source.name if capture_date else image_root / source.name
        elif source.suffix.lower() in (".mjpeg", ".avi", ".mp4"):
            target = media_root / "Legacy_Videos" / source.name
        else:
            target = media_root / "SD Card Files" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        elapsed = max(time.monotonic() - started, 0.001)
        rate = (index - 1) / elapsed
        eta = (total - index + 1) / rate if rate > 0 else 0
        progress(index - 1, total,
                 f"USB Mass Storage: processing {index}/{total} - {source.name} "
                 f"({rate:.1f} files/s, ETA {eta / 60:.1f} min)")
        # Every completed target is installed atomically from a .part file.
        # Therefore an existing non-empty target is already verified enough to
        # skip without reopening the removable source file.
        if target.exists() and target.stat().st_size > 0:
            skipped += 1
        else:
            temporary = target.with_name(target.name + ".part")
            source_size = _copy_with_retry(source, temporary, media_password)
            os.replace(temporary, target)
            copied += 1
            byte_count += source_size
        elapsed = max(time.monotonic() - started, 0.001)
        rate = index / elapsed
        eta = (total - index) / rate if rate > 0 else 0
        progress(index, total, f"USB Mass Storage: {index}/{total} complete "
                 f"({rate:.1f} files/s, ETA {eta / 60:.1f} min)")

    progress(total, total, "USB Mass Storage: verifying completed transfer...")
    final_files = _scan_stable_inventory(volume)
    final_inventory = {
        str(path.relative_to(volume)).lower(): path.stat().st_size
        for path in final_files
    }
    if final_inventory != expected_inventory:
        raise RuntimeError(
            "The SD-card inventory changed during transfer; transfer was not marked complete."
        )
    return copied, skipped, byte_count


def _delete_all_captures(volume: Path,
                         progress: Callable[[int, int, str], None]) -> tuple[int, int]:
    """Delete the firmware-owned captures tree through the mounted OTG volume."""
    captures = volume / "captures"
    if not captures.is_dir():
        raise RuntimeError("The OTG drive does not contain the camera captures folder.")

    files = sorted(
        (path for path in captures.rglob("*") if path.is_file()),
        key=lambda path: str(path).lower(),
    )
    deleted = failed = 0
    total = len(files)
    for index, path in enumerate(files, 1):
        progress(index - 1, total or 1,
                 f"USB Mass Storage: deleting {index}/{total} - {path.name}")
        last_error: OSError | None = None
        for attempt in range(1, FILE_IO_RETRIES + 1):
            try:
                path.unlink()
                deleted += 1
                last_error = None
                break
            except OSError as exc:
                last_error = exc
                if attempt < FILE_IO_RETRIES:
                    time.sleep(0.25 * attempt)
        if last_error is not None:
            failed += 1
        progress(index, total or 1,
                 f"USB Mass Storage: deleted {deleted}/{total}; failed {failed}")

    # Remove empty date/subdirectories while retaining the firmware's captures root.
    directories = sorted(
        (path for path in captures.rglob("*") if path.is_dir()),
        key=lambda path: len(path.parts),
        reverse=True,
    )
    for directory in directories:
        try:
            directory.rmdir()
        except OSError:
            pass
    return deleted, failed


def _dismount_windows_volume(volume: Path) -> bool:
    """Flush, lock, and dismount the FAT volume before returning it to firmware."""
    if os.name != "nt":
        return True
    kernel32 = ctypes.windll.kernel32
    kernel32.CreateFileW.restype = ctypes.c_void_p
    drive = str(volume).rstrip("\\/")
    path = f"\\\\.\\{drive}"
    invalid_handle = ctypes.c_void_p(-1).value
    handle = kernel32.CreateFileW(
        path, 0x80000000 | 0x40000000, 0x00000001 | 0x00000002,
        None, 3, 0, None,
    )
    if handle == invalid_handle:
        return False
    returned = ctypes.c_ulong()
    try:
        kernel32.FlushFileBuffers(ctypes.c_void_p(handle))
        # Explorer/indexing may briefly own a handle; retry the exclusive lock.
        locked = False
        for _ in range(20):
            locked = bool(kernel32.DeviceIoControl(
                ctypes.c_void_p(handle), 0x00090018, None, 0, None, 0,
                ctypes.byref(returned), None,
            ))
            if locked:
                break
            time.sleep(0.25)
        if not locked:
            return False
        return bool(kernel32.DeviceIoControl(
            ctypes.c_void_p(handle), 0x00090020, None, 0, None, 0,
            ctypes.byref(returned), None,
        ))
    finally:
        kernel32.CloseHandle(ctypes.c_void_p(handle))


def _request_sd_return(*, request, volume: Path | None, log,
                       progress=None) -> None:
    """Safely withdraw MSC and return SD ownership to the firmware."""
    if volume is not None:
        if progress is not None:
            progress(0, 0, "Safely ejecting the OTG drive...")
        if not _dismount_windows_volume(volume):
            raise RuntimeError(
                "Windows is still using the OTG drive. Close Explorer/previews and retry Eject OTG."
            )
    for attempt in range(1, 4):
        stop_lines = request(
            "USB_MSC_STOP",
            lambda line: line.startswith(("OK USB_MSC_STOP", "ERR USB_MSC_STOP")),
            20000,
        )
        if any("OK USB_MSC_STOP" in line or "not_active" in line for line in stop_lines):
            return
        log(f"SD remount attempt {attempt}/3 did not receive confirmation.")
        time.sleep(0.5)
    raise RuntimeError("The SD card could not be returned to the ESP32-P4.")


def eject_otg_session(*, request, log, progress) -> None:
    """Explicitly end a persistent MSC session and remount SD on the camera."""
    volume = _find_camera_volume(_windows_volumes())
    _request_sd_return(request=request, volume=volume, log=log, progress=progress)
    progress(0, 1, "OTG safely ejected; SD card returned to ESP32-P4")
    log("OTG Mass Storage session ended; SD card remounted on ESP32-P4.")


def delete_all_via_otg_if_connected(*, request, log,
                                    progress) -> UsbMassStorageDeleteResult:
    """Delete camera media directly over OTG MSC, with serial as caller fallback."""
    stop_lines = request(
        "STOP_PROGRAM",
        lambda line: line.startswith(("OK STOP_PROGRAM", "ERR STOP_PROGRAM")),
        3000,
    )
    if not any(line.startswith("OK STOP_PROGRAM") for line in stop_lines):
        raise RuntimeError("Firmware did not confirm that automatic capture stopped.")

    before = _windows_volumes()
    lines = request(
        "USB_MSC_START",
        lambda line: line.startswith(("OK USB_MSC_START", "ERR USB_MSC_START")),
        20000,
    )
    if not any("OK USB_MSC_START" in line for line in lines):
        log("USB Mass Storage is unavailable; using serial delete-all.")
        return UsbMassStorageDeleteResult(False)

    volume = None
    try:
        progress(0, 1, "Waiting up to 10 seconds for the OTG USB drive...")
        volume = _wait_for_camera_volume(before)
        if volume is None:
            log("No OTG mass-storage drive detected; using serial delete-all.")
            _request_sd_return(request=request, volume=None, log=log)
            return UsbMassStorageDeleteResult(False)

        log(f"OTG USB drive detected at {volume}; deleting camera media directly.")
        deleted, failed = _delete_all_captures(volume, progress)
        _request_sd_return(request=request, volume=volume, log=log, progress=progress)
    except Exception as exc:
        raise RuntimeError(
            f"OTG delete-all failed: {exc} Use Eject OTG if the drive remains mounted."
        ) from exc

    log(f"OTG delete-all complete: {deleted} deleted, {failed} failed; SD remounted on ESP32-P4.")
    return UsbMassStorageDeleteResult(True, deleted, failed, str(volume))


def retrieve_if_otg_connected(*, request, media_root: Path, log,
                              progress) -> UsbMassStorageResult:
    """Export the SD, copy it, and restore ownership only after verified success.

    If no OTG volume appears, ownership is restored and ``used`` is false so the
    caller can continue with serial. Once copying starts, transient failures
    deliberately leave MSC active so the user can retry without re-enumeration.
    """
    before = _windows_volumes()
    key_lines = request(
        "MEDIA_EXPORT_KEY",
        lambda line: line.startswith(("MEDIA_EXPORT_KEY ", "ERR MEDIA_EXPORT_KEY")),
        3000,
    )
    key_text = next((line.split("MEDIA_EXPORT_KEY ", 1)[1].strip()
                     for line in key_lines if "MEDIA_EXPORT_KEY " in line), "")
    if len(key_text) != 64 or any(ch not in string.hexdigits for ch in key_text):
        log("Firmware does not support secure-media USB export; using serial transfer.")
        return UsbMassStorageResult(False)
    media_password = key_text.encode("ascii")
    lines = request(
        "USB_MSC_START",
        lambda line: line.startswith(("OK USB_MSC_START", "ERR USB_MSC_START")),
        20000,
    )
    if not any("OK USB_MSC_START" in line for line in lines):
        log("USB Mass Storage is unavailable; using serial transfer.")
        return UsbMassStorageResult(False)

    volume = None
    copied = skipped = byte_count = 0
    try:
        progress(0, 0, "Waiting up to 10 seconds for the OTG USB drive...")
        volume = _wait_for_camera_volume(before)
        if volume is None:
            log("No OTG mass-storage drive detected; using serial transfer.")
            _request_sd_return(request=request, volume=None, log=log)
            return UsbMassStorageResult(False)
        log(f"OTG USB drive detected at {volume}; copying all SD-card files.")
        copied, skipped, byte_count = _copy_all(volume, media_root, media_password, progress)
        for source, destination, score in promote_sharpest_burst_images(
            media_root / "Burst Images", media_root / "Images"
        ):
            log(
                f"Selected least-blurred burst image {source.name} "
                f"(sharpness {score:.1f}); copied to {destination}."
            )
    except Exception as exc:
        if volume is not None:
            progress(copied + skipped, 0,
                     "Transfer interrupted; OTG remains exposed for retry")
            raise RuntimeError(
                f"{exc} The OTG session remains active; retry Get SD Media or click Eject OTG."
            ) from exc
        raise

    # Only verified completion is allowed to withdraw the Windows disk.
    progress(copied + skipped, copied + skipped,
             "Transfer verified; safely releasing the OTG drive...")
    _request_sd_return(request=request, volume=volume, log=log, progress=progress)
    log(f"USB transfer complete: {copied} copied, {skipped} already present; SD remounted on ESP32-P4.")
    return UsbMassStorageResult(True, copied, skipped, byte_count, str(volume))
