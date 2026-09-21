"""Inventory, resume, validate, and atomically commit SD media downloads."""

import hashlib
import os
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from collections.abc import Callable

import media_protocol
from command_service import clean_protocol_line
from media_utils import (
    capture_date_folder,
    discard_local_temp,
    extract_clean_mjpeg_frames,
    is_jpeg_file_name,
    is_mjpeg_file_name,
    local_media_matches,
    promote_sharpest_burst_images,
)


MAX_TRANSFER_ATTEMPTS = 3
MAX_TOLERATED_MJPEG_BAD_CHUNKS = 8
IMAGE_PAUSE_TIMEOUT_MS = 8000
BINARY_TRANSFER_TIMEOUT_MS = 10000


@dataclass(frozen=True)
class MediaTransferItem:
    name: str
    size: int | None
    category: str
    output_path: Path
    is_image: bool
    is_burst: bool
    is_video: bool

    @property
    def already_downloaded(self) -> bool:
        return local_media_matches(self.output_path, self.size)


@dataclass(frozen=True)
class MediaTransferPlan:
    """Immutable work inventory grouped into summaries, stills, and videos."""
    summaries: tuple[str, ...]
    items: tuple[MediaTransferItem, ...]
    image_count: int
    burst_count: int
    legacy_count: int

    @property
    def total_items(self) -> int:
        return len(self.summaries) + len(self.items)

    @property
    def inventory_text(self) -> str:
        text = (
            f"SD card: {self.image_count} image(s), "
            f"{self.burst_count} burst image(s), "
            f"{len(self.summaries)} summary file(s)"
        )
        if self.legacy_count:
            text += f", {self.legacy_count} legacy media file(s)"
        return text


@dataclass(frozen=True)
class MediaDownloadResult:
    success: bool
    skipped: bool = False
    rebooted: bool = False
    transferred_bytes: int = 0
    mjpeg_frame_count: int | None = None


@dataclass(frozen=True)
class MediaSessionResult:
    completed: int
    total: int
    failed: int
    image_count: int
    burst_count: int
    summary_count: int
    legacy_count: int
    skipped_count: int
    transferred_bytes: int
    elapsed_seconds: float
    videos_to_convert: tuple[tuple[Path, int | None], ...] = ()
    error: str | None = None
    rebooted: bool = False

    @property
    def status_text(self) -> str:
        if self.error:
            return f"Transfer failed: {self.error}"
        if self.rebooted:
            return "Transfer aborted: device rebooted"
        speed = self.transferred_bytes / 1024.0 / max(self.elapsed_seconds, 0.001)
        if self.failed:
            text = (
                f"Transfer incomplete: {self.completed}/{self.total} saved; "
                f"{self.failed} file(s) failed"
            )
        else:
            text = (
                f"Transfer complete: {self.image_count} image(s), "
                f"{self.burst_count} burst image(s), "
                f"{self.summary_count} summary file(s)"
            )
            if self.legacy_count:
                text += f", {self.legacy_count} legacy media file(s)"
            if self.skipped_count:
                text += f"; {self.skipped_count} already present"
        if self.transferred_bytes:
            text += f"; {self.elapsed_seconds:.1f}s, {speed:.1f} KiB/s"
        return text


def parse_summary_names(
    lines: list[str], clean_line: Callable[[str], str]
) -> tuple[list[str], list[str]]:
    names: set[str] = set()
    errors: list[str] = []
    for line in lines:
        clean = clean_line(line)
        if clean.startswith("SUMMARY "):
            parts = clean.split()
            if len(parts) >= 2:
                names.add(Path(parts[1]).name)
        elif clean.startswith("ERR "):
            errors.append(clean)
    return sorted(names), errors


def create_transfer_plan(
    media_lines: list[str],
    summary_names: list[str],
    image_directory: Path,
    burst_directory: Path,
    legacy_directory: Path,
) -> MediaTransferPlan:
    parsed: dict[str, int | None] = {}
    for line in media_lines:
        if not line.startswith("IMG "):
            continue
        parts = line.split(maxsplit=2)
        if len(parts) < 3:
            continue
        try:
            size = int(parts[2])
            if size <= 0:
                continue
        except ValueError:
            size = None
        parsed[parts[1]] = size

    normal: list[MediaTransferItem] = []
    burst: list[MediaTransferItem] = []
    legacy: list[MediaTransferItem] = []
    for name, size in parsed.items():
        is_image = is_jpeg_file_name(name)
        is_burst = is_image and "_burst_" in name.lower()
        is_video = is_mjpeg_file_name(name)
        if is_burst:
            directory = burst_directory
            category = "burst images"
        elif is_image:
            directory = image_directory
            category = "images"
        else:
            directory = legacy_directory
            category = "legacy media"
        if is_image:
            capture_date = capture_date_folder(name)
            if capture_date:
                directory = directory / capture_date
        item = MediaTransferItem(
            name=name,
            size=size,
            category=category,
            output_path=directory / Path(name).name,
            is_image=is_image,
            is_burst=is_burst,
            is_video=is_video,
        )
        (burst if is_burst else normal if is_image else legacy).append(item)

    sort_key = lambda item: item.name
    normal.sort(key=sort_key)
    burst.sort(key=sort_key)
    legacy.sort(key=sort_key)
    return MediaTransferPlan(
        summaries=tuple(sorted(set(summary_names))),
        items=tuple(normal + burst + legacy),
        image_count=len(normal),
        burst_count=len(burst),
        legacy_count=len(legacy),
    )


class MediaDownloader:
    """Download one item with validation, optional resume, and atomic commit."""

    def __init__(
        self,
        request: Callable[[str, Callable[[str], bool], int], list[str]],
        log: Callable[[str], None],
        use_base64: bool,
        stream_request: Callable[
            [
                str,
                Callable[[str], bool],
                int,
                Callable[[str | media_protocol.BinaryDataFrame], None],
            ],
            None,
        ] | None = None,
        require_hash: bool = False,
        allow_resume: bool = False,
        use_binary: bool = False,
    ) -> None:
        self._request = request
        self._log = log
        self._use_base64 = use_base64
        self._stream_request = stream_request
        self._require_hash = require_hash
        self._allow_resume = allow_resume
        self._use_binary = use_binary

    def download(self, item: MediaTransferItem) -> MediaDownloadResult:
        """Fetch with bounded retries without exposing an incomplete file."""
        if item.already_downloaded:
            return MediaDownloadResult(success=True, skipped=True)

        timeout_ms = media_protocol.transfer_timeout_ms(item.size)
        if self._use_binary:
            timeout_ms = BINARY_TRANSFER_TIMEOUT_MS
        for attempt in range(1, MAX_TRANSFER_ATTEMPTS + 1):
            if attempt > 1:
                self._log(
                    f"Retrying {item.name} "
                    f"(attempt {attempt}/{MAX_TRANSFER_ATTEMPTS})..."
                )
            try:
                result = self._download_attempt(item, timeout_ms)
            except (OSError, RuntimeError, TimeoutError):
                result = MediaDownloadResult(success=False)
                self._log(f"Transfer interrupted for {item.name}; retrying this file.")
                # If the same resume point fails twice, make the final attempt
                # from byte zero instead of repeatedly requesting a bad offset.
                if attempt >= 2 and self._allow_resume:
                    discard_local_temp(
                        item.output_path.parent / f".{item.output_path.name}.part"
                    )
            if result.success or result.rebooted:
                return result

        self._log(
            f"Skipping {item.name}: transfer remained unstable after "
            f"{MAX_TRANSFER_ATTEMPTS} attempt(s)."
        )
        return MediaDownloadResult(success=False)

    def _download_attempt(
        self, item: MediaTransferItem, timeout_ms: int
    ) -> MediaDownloadResult:
        if self._stream_request is not None:
            return self._download_stream_attempt(item, timeout_ms)

        fetch_command = (
            "IMG_GETBIN" if self._use_binary
            else "IMG_GET64" if self._use_base64
            else "IMG_GET"
        )
        raw_lines = self._request(
            f"{fetch_command} {item.name}",
            lambda line: (
                line.startswith(f"END {item.name}")
                or line.startswith("ERR IMG_GET")
            ),
            timeout_ms,
        )
        raw_lines = media_protocol.normalize_fetch_lines(raw_lines)
        raw_lines, repaired_split_lines = media_protocol.repair_split_data_lines(raw_lines)
        response_lines, ignored_lines, rebooted = media_protocol.extract_relevant_lines(
            raw_lines, media_protocol.is_expected_fetch_line
        )
        if ignored_lines:
            self._log(
                f"{item.name}: ignored {len(ignored_lines)} unexpected serial line(s)."
            )
        if repaired_split_lines:
            self._log(
                f"Repaired {repaired_split_lines} split DATA tail fragment(s) "
                f"during {item.name} transfer."
            )
        if rebooted:
            self._log(f"Stopping retrieval: device rebooted during transfer of {item.name}.")
            return MediaDownloadResult(success=False, rebooted=True)

        item.output_path.parent.mkdir(parents=True, exist_ok=True)
        temp_path: Path | None = None
        payload_size = 0
        payload_header = bytearray()
        expected_size = None
        expected_hash: str | None = None
        end_hash: str | None = None
        payload_hash = hashlib.sha256()
        got_begin = False
        got_end = False
        invalid_chunks = 0
        repaired_chunks = 0
        sequence_errors = 0
        length_errors = 0
        crc_errors = 0
        expected_sequence = 0
        transfer_error = None

        try:
            # Never write directly to the final name. Validation occurs while
            # the payload is still a sibling .part file, and os.replace below
            # makes the completed file visible in one atomic filesystem step.
            with tempfile.NamedTemporaryFile(
                mode="wb",
                delete=False,
                dir=item.output_path.parent,
                prefix=f".{item.output_path.name}.",
                suffix=".part",
            ) as temp_file:
                temp_path = Path(temp_file.name)
                for line in response_lines:
                    if line.startswith("BEGIN "):
                        got_begin = True
                        _name, expected_size, begin_offset, expected_hash = (
                            media_protocol.parse_begin_line(line)
                        )
                        if begin_offset != 0:
                            sequence_errors += 1
                    elif line.startswith(("DATA ", "DATA64 ")):
                        encoded, sequence, declared_length, declared_crc = (
                            media_protocol.parse_data_line(line)
                        )
                        if encoded is None:
                            invalid_chunks += 1
                            continue
                        if sequence is not None:
                            if sequence != expected_sequence:
                                sequence_errors += 1
                            expected_sequence = sequence + 1
                        else:
                            expected_sequence += 1

                        if line.startswith("DATA64 "):
                            chunk = media_protocol.decode_base64_chunk(
                                encoded, declared_length
                            )
                            repaired_nonhex = repaired_odd = False
                        else:
                            chunk, repaired_nonhex, repaired_odd = (
                                media_protocol.decode_hex_chunk(encoded, declared_length)
                            )
                        if chunk == b"":
                            continue
                        if repaired_nonhex or repaired_odd:
                            repaired_chunks += 1
                        if chunk is None:
                            if declared_length is None:
                                invalid_chunks += 1
                            else:
                                length_errors += 1
                            continue
                        if (
                            declared_crc is not None
                            and media_protocol.crc16_ccitt_false(chunk) != declared_crc
                        ):
                            crc_errors += 1
                            continue
                        if len(payload_header) < 3:
                            payload_header.extend(chunk[: 3 - len(payload_header)])
                        temp_file.write(chunk)
                        payload_hash.update(chunk)
                        payload_size += len(chunk)
                    elif line.startswith("END "):
                        got_end = True
                        parts = line.split(maxsplit=2)
                        if len(parts) >= 3 and len(parts[2]) == 64:
                            end_hash = parts[2].lower()
                    elif line.startswith("ERR "):
                        transfer_error = line
                        break
                temp_file.flush()
                os.fsync(temp_file.fileno())

            if transfer_error:
                if "invalid_offset" in transfer_error:
                    discard_partial = True
                self._log(f"Transfer error for {item.name}: {transfer_error}")
                return MediaDownloadResult(success=False)

            bad_chunks = invalid_chunks + sequence_errors + length_errors + crc_errors
            tolerate_damage = (
                item.is_video
                and 0 < bad_chunks <= MAX_TOLERATED_MJPEG_BAD_CHUNKS
            )
            problems = (
                (invalid_chunks, "malformed DATA chunk(s)"),
                (sequence_errors, "DATA sequence error(s)"),
                (length_errors, "DATA length mismatch chunk(s)"),
                (crc_errors, "DATA CRC mismatch chunk(s)"),
            )
            if not tolerate_damage:
                for count, description in problems:
                    if count:
                        self._log(
                            f"Retry needed for {item.name}: {count} {description}."
                        )
                        return MediaDownloadResult(success=False)
            elif bad_chunks:
                self._log(
                    f"Warning: salvaging {item.name} despite {bad_chunks} damaged "
                    "DATA chunk(s); one frame may be corrupted or missing."
                )
            if repaired_chunks:
                self._log(
                    f"Note: repaired {repaired_chunks} noisy DATA chunk(s) "
                    f"during {item.name} transfer."
                )
            if not got_begin or not got_end or payload_size == 0:
                self._log(
                    f"Retry needed for {item.name}: incomplete transfer "
                    f"(timeout was {timeout_ms / 1000.0:.0f}s for "
                    f"{item.size or 'unknown'} bytes)."
                )
                return MediaDownloadResult(success=False)

            # Per-chunk CRC/sequence checks above detect localized corruption;
            # the whole-file digest below detects omissions and ordering errors
            # that could still produce individually valid chunks.
            if item.is_image and not payload_header.startswith(b"\xFF\xD8\xFF"):
                self._log(
                    f"Retry needed for {item.name}: payload is not a valid JPEG header."
                )
                return MediaDownloadResult(success=False)

            frame_count = None
            if item.is_video:
                clean_payload, frame_count, dropped_bytes = extract_clean_mjpeg_frames(
                    temp_path.read_bytes()
                )
                if frame_count == 0:
                    self._log(
                        f"Retry needed for {item.name}: no complete MJPEG frames were found."
                    )
                    return MediaDownloadResult(success=False)
                if dropped_bytes:
                    self._log(
                        f"{item.name}: removed {dropped_bytes} padding/junk byte(s) "
                        "between MJPEG frames."
                    )
                with temp_path.open("wb") as clean_file:
                    clean_file.write(clean_payload)
                    clean_file.flush()
                    os.fsync(clean_file.fileno())
                self._log(
                    f"{item.name}: received {frame_count} complete MJPEG frame(s)."
                )

            if expected_size is not None and payload_size != expected_size:
                if item.is_video and tolerate_damage:
                    self._log(
                        "Warning: saving recovered MJPEG payload with size mismatch "
                        f"{payload_size} vs expected {expected_size}."
                    )
                else:
                    self._log(
                        f"Retry needed for {item.name}: size mismatch "
                        f"{payload_size} vs expected {expected_size}."
                    )
                    return MediaDownloadResult(success=False)
            actual_hash = payload_hash.hexdigest()
            if self._require_hash and (expected_hash is None or end_hash is None):
                discard_partial = True
                self._log(f"Retry needed for {item.name}: SHA-256 metadata missing.")
                return MediaDownloadResult(success=False)
            if expected_hash is not None and actual_hash != expected_hash:
                self._log(
                    f"Retry needed for {item.name}: SHA-256 mismatch."
                )
                return MediaDownloadResult(success=False)
            if end_hash is not None and end_hash != actual_hash:
                self._log(
                    f"Retry needed for {item.name}: END SHA-256 mismatch."
                )
                return MediaDownloadResult(success=False)
            if expected_hash is not None and end_hash is not None and expected_hash != end_hash:
                self._log(
                    f"Retry needed for {item.name}: transfer hashes disagree."
                )
                return MediaDownloadResult(success=False)

            os.replace(temp_path, item.output_path)
            temp_path = None
            return MediaDownloadResult(
                success=True,
                transferred_bytes=item.output_path.stat().st_size,
                mjpeg_frame_count=frame_count,
            )
        finally:
            discard_local_temp(temp_path)

    def _download_stream_attempt(
        self, item: MediaTransferItem, timeout_ms: int
    ) -> MediaDownloadResult:
        """Validate streamed chunks and append them to a resumable partial file."""
        fetch_command = (
            "IMG_GETBIN" if self._use_binary
            else "IMG_GET64" if self._use_base64
            else "IMG_GET"
        )
        item.output_path.parent.mkdir(parents=True, exist_ok=True)
        temp_path: Path | None = None
        resume_path = item.output_path.parent / f".{item.output_path.name}.part"
        resume_offset = 0
        if self._allow_resume and resume_path.exists():
            try:
                resume_offset = resume_path.stat().st_size
                if item.size is not None and resume_offset > item.size:
                    resume_path.unlink()
                    resume_offset = 0
            except OSError:
                resume_offset = 0
        payload_size = resume_offset
        payload_header = bytearray()
        expected_size: int | None = None
        expected_hash: str | None = None
        end_hash: str | None = None
        payload_hash = hashlib.sha256()
        if resume_offset:
            try:
                with resume_path.open("rb") as existing:
                    while chunk := existing.read(64 * 1024):
                        if len(payload_header) < 3:
                            payload_header.extend(chunk[: 3 - len(payload_header)])
                        payload_hash.update(chunk)
            except OSError:
                discard_local_temp(resume_path)
                resume_offset = payload_size = 0
                payload_header.clear()
                payload_hash = hashlib.sha256()
        expected_sequence = 0
        got_begin = got_end = rebooted = False
        invalid_chunks = sequence_errors = length_errors = crc_errors = 0
        ignored_lines = 0
        transfer_error: str | None = None
        discard_partial = False

        try:
            if self._allow_resume:
                temp_path = resume_path
                temp_file_context = temp_path.open("ab")
            else:
                temp_file_context = tempfile.NamedTemporaryFile(
                    mode="wb",
                    delete=False,
                    dir=item.output_path.parent,
                    prefix=f".{item.output_path.name}.",
                    suffix=".part",
                )
            with temp_file_context as temp_file:
                temp_path = Path(temp_file.name)

                def consume_chunk(
                    sequence: int | None,
                    chunk: bytes | None,
                    declared_length: int | None,
                    declared_crc: int | None,
                ) -> None:
                    nonlocal payload_size, expected_sequence
                    nonlocal invalid_chunks, sequence_errors, length_errors, crc_errors

                    if sequence is None:
                        invalid_chunks += 1
                        return
                    if sequence != expected_sequence:
                        sequence_errors += 1
                    expected_sequence = sequence + 1
                    if chunk is None:
                        if declared_length is None:
                            invalid_chunks += 1
                        else:
                            length_errors += 1
                        return
                    if declared_length is not None and len(chunk) != declared_length:
                        length_errors += 1
                        return
                    if (
                        declared_crc is not None
                        and media_protocol.crc16_ccitt_false(chunk) != declared_crc
                    ):
                        crc_errors += 1
                        return
                    if len(payload_header) < 3:
                        payload_header.extend(chunk[: 3 - len(payload_header)])
                    temp_file.write(chunk)
                    payload_hash.update(chunk)
                    payload_size += len(chunk)

                def consume(raw_line: str | media_protocol.BinaryDataFrame) -> None:
                    nonlocal payload_size, expected_size, expected_hash, end_hash
                    nonlocal expected_sequence
                    nonlocal got_begin, got_end, rebooted, invalid_chunks
                    nonlocal sequence_errors, length_errors, crc_errors
                    nonlocal ignored_lines, transfer_error

                    if isinstance(raw_line, media_protocol.BinaryDataFrame):
                        consume_chunk(
                            raw_line.sequence,
                            raw_line.payload,
                            len(raw_line.payload),
                            raw_line.crc,
                        )
                        return

                    for line in media_protocol.split_fetch_protocol_line(raw_line):
                        if media_protocol.is_reboot_line(line):
                            rebooted = True
                            continue
                        if not media_protocol.is_expected_fetch_line(line):
                            ignored_lines += 1
                            continue
                        if line.startswith("BEGIN "):
                            got_begin = True
                            _name, expected_size, accepted_offset, expected_hash = (
                                media_protocol.parse_begin_line(line)
                            )
                            if accepted_offset != resume_offset:
                                sequence_errors += 1
                            continue
                        if line.startswith("END "):
                            got_end = True
                            parts = line.split(maxsplit=2)
                            if len(parts) >= 3 and len(parts[2]) == 64:
                                end_hash = parts[2].lower()
                            continue
                        if line.startswith("ERR "):
                            transfer_error = line
                            continue

                        encoded, sequence, declared_length, declared_crc = (
                            media_protocol.parse_data_line(line)
                        )
                        if encoded is None:
                            invalid_chunks += 1
                            continue
                        if line.startswith("DATA64 "):
                            chunk = media_protocol.decode_base64_chunk(
                                encoded, declared_length
                            )
                        else:
                            chunk, _repaired_nonhex, _repaired_odd = (
                                media_protocol.decode_hex_chunk(encoded, declared_length)
                            )
                        consume_chunk(sequence, chunk, declared_length, declared_crc)

                assert self._stream_request is not None
                command = f"{fetch_command} {item.name}"
                if self._allow_resume:
                    command += f" {resume_offset}"
                self._stream_request(
                    command,
                    lambda line: (
                        line.startswith(f"END {item.name}")
                        or line.startswith("ERR IMG_GET")
                    ),
                    timeout_ms,
                    consume,
                )
                temp_file.flush()
                os.fsync(temp_file.fileno())

            if ignored_lines:
                self._log(
                    f"{item.name}: ignored {ignored_lines} unexpected serial line(s)."
                )
            if rebooted:
                self._log(f"Stopping retrieval: device rebooted during transfer of {item.name}.")
                return MediaDownloadResult(success=False, rebooted=True)
            if transfer_error:
                self._log(f"Transfer error for {item.name}: {transfer_error}")
                return MediaDownloadResult(success=False)

            problems = (
                (invalid_chunks, "malformed DATA chunk(s)"),
                (sequence_errors, "DATA sequence error(s)"),
                (length_errors, "DATA length mismatch chunk(s)"),
                (crc_errors, "DATA CRC mismatch chunk(s)"),
            )
            for count, description in problems:
                if count:
                    if self._allow_resume and temp_path is not None:
                        with temp_path.open("r+b") as partial:
                            partial.truncate(resume_offset)
                    self._log(f"Retry needed for {item.name}: {count} {description}.")
                    return MediaDownloadResult(success=False)
            if not got_begin or not got_end or payload_size == 0:
                self._log(f"Retry needed for {item.name}: incomplete transfer.")
                return MediaDownloadResult(success=False)
            if expected_size is not None and payload_size != expected_size:
                self._log(
                    f"Retry needed for {item.name}: size mismatch "
                    f"{payload_size} vs expected {expected_size}."
                )
                return MediaDownloadResult(success=False)
            actual_hash = payload_hash.hexdigest()
            if self._require_hash and (expected_hash is None or end_hash is None):
                self._log(f"Retry needed for {item.name}: SHA-256 metadata missing.")
                return MediaDownloadResult(success=False)
            if expected_hash is not None and actual_hash != expected_hash:
                discard_partial = True
                self._log(f"Retry needed for {item.name}: SHA-256 mismatch.")
                return MediaDownloadResult(success=False)
            if end_hash is not None and end_hash != actual_hash:
                discard_partial = True
                self._log(f"Retry needed for {item.name}: END SHA-256 mismatch.")
                return MediaDownloadResult(success=False)
            if expected_hash is not None and end_hash is not None and expected_hash != end_hash:
                discard_partial = True
                self._log(f"Retry needed for {item.name}: transfer hashes disagree.")
                return MediaDownloadResult(success=False)
            if item.is_image and not payload_header.startswith(b"\xFF\xD8\xFF"):
                discard_partial = True
                self._log(f"Retry needed for {item.name}: payload is not a valid JPEG header.")
                return MediaDownloadResult(success=False)

            frame_count = None
            if item.is_video:
                clean_payload, frame_count, dropped_bytes = extract_clean_mjpeg_frames(
                    temp_path.read_bytes()
                )
                if frame_count == 0:
                    self._log(f"Retry needed for {item.name}: no complete MJPEG frames were found.")
                    return MediaDownloadResult(success=False)
                if dropped_bytes:
                    self._log(
                        f"{item.name}: removed {dropped_bytes} padding/junk byte(s) "
                        "between MJPEG frames."
                    )
                temp_path.write_bytes(clean_payload)

            os.replace(temp_path, item.output_path)
            temp_path = None
            return MediaDownloadResult(
                success=True,
                transferred_bytes=item.output_path.stat().st_size,
                mjpeg_frame_count=frame_count,
            )
        finally:
            if temp_path is not None and (not self._allow_resume or discard_partial):
                discard_local_temp(temp_path)


class MediaTransferSession:
    """Coordinate device pause, SD synchronization, and guaranteed resume."""

    def __init__(
        self,
        request: Callable[[str, Callable[[str], bool], int], list[str]],
        log: Callable[[str], None],
        progress: Callable[[int, int, str], None],
        use_base64: Callable[[], bool],
        image_directory: Path,
        burst_directory: Path,
        summary_directory: Path,
        legacy_directory: Path,
        stream_request: Callable[
            [
                str,
                Callable[[str], bool],
                int,
                Callable[[str | media_protocol.BinaryDataFrame], None],
            ],
            None,
        ] | None = None,
        require_hash: Callable[[], bool] | None = None,
        allow_resume: Callable[[], bool] | None = None,
        use_binary: Callable[[], bool] | None = None,
    ) -> None:
        self._request = request
        self._log = log
        self._progress = progress
        self._use_base64 = use_base64
        self._image_directory = image_directory
        self._burst_directory = burst_directory
        self._summary_directory = summary_directory
        self._legacy_directory = legacy_directory
        self._stream_request = stream_request
        self._require_hash = require_hash or (lambda: False)
        self._allow_resume = allow_resume or (lambda: False)
        self._use_binary = use_binary or (lambda: False)

    def _request_command(
        self, command: str, is_done: Callable[[str], bool], timeout_ms: int
    ) -> list[str]:
        return self._request(command, is_done, timeout_ms)

    def _summary_inventory(self) -> list[str]:
        lines = self._request_command(
            "SUMMARY_LIST",
            lambda line: (
                line.startswith("OK SUMMARY_LIST")
                or line.startswith("ERR SUMMARY_LIST")
            ),
            5000,
        )
        names, errors = parse_summary_names(
            lines, lambda line: clean_protocol_line(line, "SUMMARY_LIST")
        )
        for error in errors:
            self._log(f"Summary list error: {error}")
        return names

    def _download_summary(self, name: str) -> bool:
        lines = self._request_command(
            f"SUMMARY_GET {name}",
            lambda line: (
                line.startswith("END_SUMMARY ")
                or line.startswith("ERR SUMMARY_GET")
            ),
            10000,
        )
        body: list[str] = []
        failed = False
        for line in lines:
            clean = clean_protocol_line(line, "SUMMARY_GET")
            if clean.startswith("TEXT "):
                body.append(clean[5:])
            elif clean.startswith("ERR "):
                failed = True
                self._log(f"Summary get error for {name}: {clean}")
        if failed or not body:
            self._log(f"Skipping summary {name}: no text content received.")
            return False

        self._summary_directory.mkdir(parents=True, exist_ok=True)
        destination = self._summary_directory / Path(name).name
        temp_path: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                delete=False,
                dir=destination.parent,
                prefix=f".{destination.name}.",
                suffix=".part",
                newline="\n",
            ) as temp_file:
                temp_path = Path(temp_file.name)
                temp_file.write("\n".join(body) + "\n")
                temp_file.flush()
                os.fsync(temp_file.fileno())
            os.replace(temp_path, destination)
            temp_path = None
            return True
        finally:
            discard_local_temp(temp_path)

    def run(self) -> MediaSessionResult:
        """Synchronize all media and resume capture even after partial failure."""
        started = time.perf_counter()
        paused = False
        completed = failed = 0
        saved_images = saved_bursts = saved_summaries = saved_legacy = 0
        skipped = transferred_bytes = 0
        videos: list[tuple[Path, int | None]] = []
        total = 0

        try:
            for directory in (
                self._legacy_directory,
                self._image_directory,
                self._burst_directory,
                self._summary_directory,
            ):
                directory.mkdir(parents=True, exist_ok=True)
            self._progress(0, 0, "SD media transfer: reading card inventory...")

            pause_lines = self._request_command(
                "IMG_PAUSE",
                lambda line: (
                    line.startswith("OK IMG_PAUSE")
                    or line.startswith("ERR IMG_PAUSE")
                ),
                IMAGE_PAUSE_TIMEOUT_MS,
            )
            paused = any(line.startswith("OK IMG_PAUSE") for line in pause_lines)
            summaries = self._summary_inventory()

            inventory_lines = self._request_command(
                "IMG_LIST",
                lambda line: (
                    line.startswith("OK LIST") or line.startswith("ERR IMG_LIST")
                ),
                25000,
            )
            media_lines, _ignored, rebooted = media_protocol.extract_relevant_lines(
                inventory_lines, media_protocol.is_expected_list_line
            )
            if rebooted:
                self._log("SD media retrieval aborted: device rebooted during inventory.")
                return MediaSessionResult(
                    0, 0, 0, 0, 0, 0, 0, 0, 0,
                    max(time.perf_counter() - started, 0.001),
                    rebooted=True,
                )

            plan = create_transfer_plan(
                media_lines,
                summaries,
                self._image_directory,
                self._burst_directory,
                self._legacy_directory,
            )
            total = plan.total_items
            self._log(plan.inventory_text + ".")
            self._progress(0, total, plan.inventory_text)

            def record(success: bool, category: str) -> None:
                nonlocal completed, failed
                if success:
                    completed += 1
                else:
                    failed += 1
                self._progress(
                    completed,
                    total,
                    f"Transferring {category}: {completed}/{total} saved",
                )

            for name in plan.summaries:
                success = self._download_summary(name)
                if success:
                    saved_summaries += 1
                record(success, "summaries")

            use_base64 = self._use_base64()
            use_binary = self._use_binary()
            if plan.items:
                self._log(
                    "Using negotiated binary v3 media transfer."
                    if use_binary
                    else "Using negotiated Base64 v2 media transfer."
                    if use_base64
                    else "Device uses legacy media transfer; compatibility mode enabled."
                )
            downloader = MediaDownloader(
                self._request_command,
                self._log,
                use_base64,
                self._stream_request,
                self._require_hash(),
                self._allow_resume(),
                use_binary,
            )
            for item in plan.items:
                self._progress(
                    completed,
                    total,
                    f"Transferring {item.category}: {completed}/{total} saved - {item.name}",
                )
                result = downloader.download(item)
                if result.rebooted:
                    return MediaSessionResult(
                        completed, total, failed, saved_images, saved_bursts,
                        saved_summaries, saved_legacy, skipped, transferred_bytes,
                        max(time.perf_counter() - started, 0.001),
                        tuple(videos), rebooted=True,
                    )
                if result.success:
                    skipped += int(result.skipped)
                    transferred_bytes += result.transferred_bytes
                    if item.is_image:
                        if item.is_burst:
                            saved_bursts += 1
                        else:
                            saved_images += 1
                    elif item.is_video:
                        saved_legacy += 1
                        if not result.skipped:
                            videos.append((item.output_path, result.mjpeg_frame_count))
                record(result.success, item.category)

            for source, destination, score in promote_sharpest_burst_images(
                self._burst_directory, self._image_directory
            ):
                self._log(
                    f"Selected least-blurred burst image {source.name} "
                    f"(sharpness {score:.1f}); copied to {destination}."
                )

            elapsed = max(time.perf_counter() - started, 0.001)
            return MediaSessionResult(
                completed, total, failed, saved_images, saved_bursts,
                saved_summaries, saved_legacy, skipped, transferred_bytes,
                elapsed, tuple(videos),
            )
        except Exception as exc:
            return MediaSessionResult(
                completed, total, failed, saved_images, saved_bursts,
                saved_summaries, saved_legacy, skipped, transferred_bytes,
                max(time.perf_counter() - started, 0.001),
                tuple(videos), error=str(exc),
            )
        finally:
            if paused:
                try:
                    self._request_command(
                        "IMG_RESUME",
                        lambda line: (
                            line.startswith("OK IMG_RESUME")
                            or line.startswith("ERR IMG_RESUME")
                        ),
                        2000,
                    )
                except Exception as exc:
                    self._log(f"Failed to resume auto capture: {exc}")
