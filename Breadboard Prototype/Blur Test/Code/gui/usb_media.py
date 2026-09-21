"""TinyUSB mass-storage operations; serial carries control messages only."""

import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import string
import threading
import time

DEVICE_ID_PATTERN = re.compile(r"BLUR_[0-9A-F]{12}")
RUN_PATTERN = re.compile(r"run_\d{5}")
MEDIA_PATTERN = re.compile(r"\d{6}_[A-Za-z0-9_]+\.jpg(?:\.csv)?")


def request(port, command, expected, stop, timeout=20):
    """Send one firmware command and return the matching response line."""
    port.write((command + "\n").encode("ascii"))
    pending = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not stop.is_set():
        pending.extend(port.read(min(max(port.in_waiting, 1), 4096)))
        while b"\n" in pending:
            raw, _, pending = pending.partition(b"\n")
            line = raw.decode("utf-8", errors="replace").strip()
            if "MEDIA_ERROR:" in line or "MEDIA_BUSY:" in line:
                raise RuntimeError(line)
            if expected in line:
                return line[line.index(expected):]
        if len(pending) > 8192:
            raise RuntimeError("Unexpected serial data. Install the TinyUSB firmware first.")
    raise RuntimeError("No firmware response. Install the TinyUSB firmware and wait for startup.")


def windows_volumes():
    """List removable and fixed Windows volumes that could contain the SD card."""
    if os.name != "nt":
        raise RuntimeError("Automatic USB-drive detection currently requires Windows.")
    kernel = ctypes.windll.kernel32
    mask = kernel.GetLogicalDrives()
    return [
        Path(f"{letter}:\\")
        for index, letter in enumerate(string.ascii_uppercase)
        if mask & (1 << index) and kernel.GetDriveTypeW(f"{letter}:\\") in (2, 3)
    ]


def identify(volume, device_id):
    """Check the board-specific marker written by the firmware."""
    try:
        return (volume / "BLUR_DEVICE.TXT").read_text("ascii").strip() == device_id
    except (OSError, UnicodeError):
        return False


def wait_volume(device_id, stop):
    """Wait for exactly one exported volume with the expected board identity."""
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline and not stop.is_set():
        matches = [volume for volume in windows_volumes() if identify(volume, device_id)]
        if len(matches) > 1:
            raise RuntimeError("Multiple drives match this board. Disconnect duplicate SD-card copies.")
        if matches:
            return matches[0]
        stop.wait(0.5)
    raise RuntimeError(
        "OTG drive did not appear. Connect the board's OTG USB-C port to the PC "
        "as well as its COM-port cable."
    )


def inventory(volume, device_id):
    """Return capture files only from firmware-created run directories."""
    if not identify(volume, device_id):
        raise RuntimeError("USB drive identity changed; operation stopped.")
    base = volume / "blur"
    if not base.is_dir() or base.is_symlink() or base.is_junction():
        raise RuntimeError("Experiment directory is missing or is a link.")
    files = []
    for run in sorted(base.iterdir()):
        if not RUN_PATTERN.fullmatch(run.name):
            continue
        if not run.is_dir() or run.is_symlink() or run.is_junction():
            continue
        for path in sorted(run.iterdir()):
            if path.is_file() and not path.is_symlink() and MEDIA_PATTERN.fullmatch(path.name):
                files.append(path)
    return files


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def copy_verified(source, destination, stop):
    """Copy atomically and verify source and destination with SHA-256."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + ".part")
    original_size = source.stat().st_size
    original_hash = hashlib.sha256()
    try:
        with source.open("rb") as reader, partial.open("wb") as writer:
            while data := reader.read(256 * 1024):
                if stop.is_set():
                    raise RuntimeError("Copy cancelled; completed files are kept.")
                original_hash.update(data)
                writer.write(data)
            writer.flush()
            os.fsync(writer.fileno())
        sha256 = original_hash.hexdigest()
        if (partial.stat().st_size != original_size or
                digest(partial) != sha256 or digest(source) != sha256):
            raise RuntimeError(f"Copy verification failed: {source.name}")
        partial.replace(destination)
        return {"bytes": original_size, "sha256": sha256}
    finally:
        partial.unlink(missing_ok=True)


def dismount(volume):
    """Flush, lock, and dismount a Windows volume before returning the SD card."""
    kernel = ctypes.windll.kernel32
    kernel.CreateFileW.restype = ctypes.c_void_p
    handle = kernel.CreateFileW(
        "\\\\.\\" + str(volume).rstrip("\\/"),
        0xC0000000, 3, None, 3, 0, None,
    )
    if handle == ctypes.c_void_p(-1).value:
        return False
    handle = ctypes.c_void_p(handle)
    returned = ctypes.c_ulong()
    try:
        if not kernel.FlushFileBuffers(handle):
            return False
        for _ in range(20):
            locked = kernel.DeviceIoControl(
                handle, 0x90018, None, 0, None, 0, ctypes.byref(returned), None
            )
            if locked:
                return bool(kernel.DeviceIoControl(
                    handle, 0x90020, None, 0, None, 0, ctypes.byref(returned), None
                ))
            time.sleep(0.25)
        return False
    finally:
        kernel.CloseHandle(handle)


def transfer(port, deleting, output, stop, emit):
    """Export the SD card, copy or delete its captures, then return ownership."""
    emit("status", "Requesting SD access on the OTG USB port...")
    line = request(port, "USB_MSC_START", "MSC_READY ", stop)
    device_id = line.removeprefix("MSC_READY ")
    if not DEVICE_ID_PATTERN.fullmatch(device_id):
        raise RuntimeError("Invalid board identity.")
    volume = wait_volume(device_id, stop)
    failure = None
    count = 0
    try:
        files = inventory(volume, device_id)
        manifest = {"device": device_id, "files": {}}
        for index, source in enumerate(files, start=1):
            if stop.is_set():
                raise RuntimeError("Operation cancelled; completed files are kept.")
            if not identify(volume, device_id):
                raise RuntimeError("USB drive disconnected or identity changed.")
            relative = source.relative_to(volume / "blur")
            action = "Deleting" if deleting else "Copying"
            emit("status", f"{action} {index}/{len(files)}: {source.name}")
            if deleting:
                # Re-inventory immediately before deletion so links or a swapped
                # volume cannot redirect this destructive operation.
                if source not in inventory(volume, device_id):
                    raise RuntimeError("Media inventory changed; deletion stopped.")
                source.unlink()
            else:
                manifest["files"][str(relative)] = copy_verified(
                    source, output / relative, stop
                )
            count += 1
            emit("progress", ("", count, len(files)))
        if not deleting and files:
            (output / "transfer_manifest.json").write_text(
                json.dumps(manifest, indent=2), "utf-8"
            )
    except Exception as exc:  # Preserve the copy error until SD cleanup finishes.
        failure = exc

    emit("status", "Safely releasing the USB drive...")
    if not dismount(volume):
        raise RuntimeError(
            f"{count} files processed. Windows could not release the drive. "
            "Close Explorer, safely eject it in Windows, then reset the board. "
            "SD remains reserved for USB."
        ) from failure

    # Always return ownership after a successful dismount, even if copying failed.
    request(port, "USB_MSC_STOP", "MSC_STOPPED", threading.Event())
    if failure:
        raise failure
    emit("progress", ("", 1, 1))
    if deleting:
        status = f"Deleted {count} SD files. Computer copies kept."
    elif count:
        status = f"Copied and SHA-256 verified {count} files to {output}"
    else:
        status = "No media found on the SD card."
    emit("status", status)
