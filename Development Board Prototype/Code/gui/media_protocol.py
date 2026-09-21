"""Parsing and integrity helpers for text and binary media-transfer frames."""

import base64
import binascii
from collections.abc import Callable
from dataclasses import dataclass


@dataclass(frozen=True)
class BinaryDataFrame:
    """One sequence-numbered, CRC-protected binary payload frame."""
    sequence: int
    payload: bytes
    crc: int


REBOOT_LINE_PREFIXES = (
    "ESP-ROM:",
    "rst:",
    "entry ",
    "load:",
    "invalid header:",
)
HEX_CHARS = frozenset("0123456789abcdefABCDEF")
TRANSFER_TIMEOUT_MIN_MS = 180000
TRANSFER_TIMEOUT_MAX_MS = 900000
TRANSFER_TIMEOUT_FIXED_OVERHEAD_MS = 30000
TRANSFER_SAFE_BYTES_PER_S = 8000


def is_reboot_line(line: str) -> bool:
    return line.startswith(REBOOT_LINE_PREFIXES)


def is_expected_list_line(line: str) -> bool:
    return (
        line.startswith("IMG ")
        or line.startswith("OK LIST")
        or line.startswith("ERR IMG_LIST")
    )


def is_expected_fetch_line(line: str) -> bool:
    return line.startswith(("BEGIN ", "DATA ", "DATA64 ", "END ", "ERR IMG_GET"))


def is_expected_delete_all_line(line: str) -> bool:
    return line.startswith(("OK IMG_DELETE_ALL", "ERR IMG_DELETE_ALL"))


def parse_begin_line(
    line: str,
) -> tuple[str | None, int | None, int, str | None]:
    """Parse BEGIN name total [offset] [sha256], including pre-resume frames."""
    parts = line.split()
    if len(parts) < 3 or parts[0] != "BEGIN":
        return None, None, 0, None
    try:
        total = int(parts[2])
    except ValueError:
        return parts[1], None, 0, None
    offset = 0
    digest = None
    if len(parts) >= 4:
        if len(parts[3]) == 64:
            digest = parts[3].lower()
        else:
            try:
                offset = int(parts[3])
            except ValueError:
                return parts[1], total, 0, None
    if len(parts) >= 5 and len(parts[4]) == 64:
        digest = parts[4].lower()
    return parts[1], total, offset, digest


def split_fetch_protocol_line(line: str) -> list[str]:
    markers = ("BEGIN ", "DATA64 ", "DATA ", "END ", "ERR IMG_GET")
    pending = line.strip()
    segments: list[str] = []
    while pending:
        next_index = min(
            (index for marker in markers if (index := pending.find(marker, 1)) > 0),
            default=-1,
        )
        if next_index < 0:
            segments.append(pending)
            break
        segments.append(pending[:next_index].strip())
        pending = pending[next_index:].strip()
    return [segment for segment in segments if segment]


def normalize_fetch_lines(lines: list[str]) -> list[str]:
    """Split protocol records concatenated by a noisy serial transport."""
    normalized: list[str] = []
    for line in lines:
        if is_expected_fetch_line(line):
            normalized.extend(split_fetch_protocol_line(line))
        else:
            normalized.append(line)
    return normalized


def is_hex_fragment_line(line: str) -> bool:
    compact = "".join(character for character in line if not character.isspace())
    return len(compact) >= 8 and all(character in HEX_CHARS for character in compact)


def parse_data_line(
    line: str,
) -> tuple[str | None, int | None, int | None, int | None]:
    if line.startswith("DATA64 "):
        prefix_length = 7
    elif line.startswith("DATA "):
        prefix_length = 5
    else:
        return None, None, None, None

    parts = line.split(maxsplit=4)
    if len(parts) >= 5:
        try:
            return parts[4], int(parts[1]), int(parts[2]), int(parts[3], 16)
        except ValueError:
            pass
    if len(parts) >= 4:
        try:
            return parts[3], int(parts[1]), int(parts[2]), None
        except ValueError:
            pass
    return line[prefix_length:].strip(), None, None, None


def repair_split_data_lines(lines: list[str]) -> tuple[list[str], int]:
    repaired: list[str] = []
    repaired_count = 0
    for line in lines:
        if repaired and is_hex_fragment_line(line) and repaired[-1].startswith("DATA "):
            chunk, _sequence, declared_length, _crc = parse_data_line(repaired[-1])
            if chunk is not None and declared_length is not None:
                have_hex = sum(1 for character in chunk if character in HEX_CHARS)
                if have_hex < declared_length * 2:
                    repaired[-1] = f"{repaired[-1]} {line.strip()}"
                    repaired_count += 1
                    continue
        repaired.append(line)
    return repaired, repaired_count


def crc16_ccitt_false(payload: bytes) -> int:
    return binascii.crc_hqx(payload, 0xFFFF)


def decode_hex_chunk(
    chunk_raw: str, declared_length: int | None = None
) -> tuple[bytes | None, bool, bool]:
    """Decode a hex DATA payload and report any safe repairs applied."""
    if not chunk_raw:
        return b"", False, False

    if declared_length is not None:
        if declared_length < 0:
            return None, False, False
        if declared_length == 0:
            return b"", False, False

        target_length = declared_length * 2
        compact = chunk_raw.strip()
        if len(compact) == target_length:
            try:
                return bytes.fromhex(compact), False, False
            except ValueError:
                pass

        collected: list[str] = []
        repaired_nonhex = False
        for character in chunk_raw:
            if character in HEX_CHARS:
                collected.append(character)
                if len(collected) >= target_length:
                    break
            elif not character.isspace():
                repaired_nonhex = True
        if len(collected) < target_length:
            return None, repaired_nonhex, False
        try:
            return bytes.fromhex("".join(collected)), repaired_nonhex, False
        except ValueError:
            return None, repaired_nonhex, False

    compact = "".join(character for character in chunk_raw if not character.isspace())
    if not compact:
        return b"", False, False
    repaired_nonhex = any(character not in HEX_CHARS for character in compact)
    if repaired_nonhex:
        compact = "".join(character for character in compact if character in HEX_CHARS)
    if not compact:
        return None, repaired_nonhex, False

    repaired_odd = len(compact) % 2 != 0
    if repaired_odd:
        compact = compact[:-1]
    if not compact or len(compact) % 2 != 0:
        return None, repaired_nonhex, repaired_odd
    try:
        return bytes.fromhex(compact), repaired_nonhex, repaired_odd
    except ValueError:
        return None, repaired_nonhex, repaired_odd


def decode_base64_chunk(chunk_raw: str, declared_length: int | None) -> bytes | None:
    if declared_length is not None and declared_length < 0:
        return None
    try:
        decoded = base64.b64decode(chunk_raw.strip(), validate=True)
    except (binascii.Error, ValueError):
        return None
    if declared_length is not None and len(decoded) != declared_length:
        return None
    return decoded


def extract_relevant_lines(
    lines: list[str], predicate: Callable[[str], bool]
) -> tuple[list[str], list[str], bool]:
    """Separate transfer records from console noise and flag device reboots."""
    relevant: list[str] = []
    ignored: list[str] = []
    reboot_detected = False
    for line in lines:
        if is_reboot_line(line):
            reboot_detected = True
            ignored.append(line)
        elif predicate(line):
            relevant.append(line)
        else:
            ignored.append(line)
    return relevant, ignored, reboot_detected


def transfer_timeout_ms(size_bytes: int | None) -> int:
    """Calculate a bounded transfer deadline from the announced file size."""
    if not size_bytes or size_bytes <= 0:
        return TRANSFER_TIMEOUT_MIN_MS
    estimated_wire_bytes = int(size_bytes * 2.4)
    estimated_ms = int((estimated_wire_bytes / TRANSFER_SAFE_BYTES_PER_S) * 1000)
    timeout = TRANSFER_TIMEOUT_FIXED_OVERHEAD_MS + estimated_ms
    return max(TRANSFER_TIMEOUT_MIN_MS, min(TRANSFER_TIMEOUT_MAX_MS, timeout))
