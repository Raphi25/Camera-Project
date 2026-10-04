"""State-changing device operations shared by both desktop front ends."""

from dataclasses import dataclass
from collections.abc import Callable

import media_protocol
from command_service import clean_protocol_line


SerialRequest = Callable[[str, Callable[[str], bool], int, bool], list[str]]
BleRequest = Callable[[str, str, bool, float], list[str]]


@dataclass(frozen=True)
class DeviceActionResult:
    messages: tuple[str, ...]
    success: bool = False
    rebooted: bool = False
    protocol_lines: tuple[str, ...] = ()


class DeviceActionService:
    """Coordinate multi-command operations and their required cleanup."""
    def __init__(
        self,
        serial_request: SerialRequest | None = None,
        ble_request: BleRequest | None = None,
    ) -> None:
        self._serial_request = serial_request
        self._ble_request = ble_request

    def capture_burst(self, bt_address: str | None = None) -> DeviceActionResult:
        try:
            if bt_address:
                if self._ble_request is None:
                    raise RuntimeError("BLE request transport is unavailable")
                lines = self._ble_request(bt_address, "BURST_3S", True, 180.0)
            else:
                if self._serial_request is None:
                    raise RuntimeError("Serial request transport is unavailable")
                lines = self._serial_request(
                    "BURST_3S",
                    lambda line: (
                        line.startswith("OK BURST_3S")
                        or line.startswith("ERR BURST_3S")
                    ),
                    180000,
                    True,
                )
            cleaned = tuple(clean_protocol_line(line, "BURST_3S") for line in lines)
            completed = any(
                line.startswith(("OK BURST_3S", "ERR BURST_3S"))
                for line in cleaned
            )
            messages = tuple(f"RX: {line}" for line in cleaned)
            if not completed:
                messages += (
                    "No BURST_3S completion reply received within 180 seconds.",
                )
            return DeviceActionResult(
                messages,
                success=any(line.startswith("OK BURST_3S") for line in cleaned),
                protocol_lines=cleaned,
            )
        except Exception as exc:
            return DeviceActionResult((f"BURST_3S error: {exc}",))

    def sync_clock(
        self, payload: str, bt_address: str | None = None
    ) -> DeviceActionResult:
        command = f"SETTIME {payload}"
        try:
            if bt_address:
                if self._ble_request is None:
                    raise RuntimeError("BLE request transport is unavailable")
                lines = self._ble_request(bt_address, command, False, 1.5)
            else:
                if self._serial_request is None:
                    raise RuntimeError("Serial request transport is unavailable")
                lines = self._serial_request(
                    command,
                    lambda line: (
                        line.startswith("OK SETTIME")
                        or line.startswith("ERR SETTIME")
                    ),
                    2000,
                    False,
                )
        except Exception as exc:
            return DeviceActionResult((f"Auto time sync failed: {exc}",))

        cleaned = tuple(clean_protocol_line(line, command) for line in lines)
        for line in cleaned:
            if line.startswith("OK SETTIME"):
                return DeviceActionResult(
                    (f"PC time auto-synced: {payload}",),
                    success=True,
                    protocol_lines=cleaned,
                )
            if line.startswith("ERR SETTIME"):
                return DeviceActionResult(
                    (f"Auto time sync failed: {line}",),
                    protocol_lines=cleaned,
                )
        return DeviceActionResult(
            ("Auto time sync: no confirmation received.",),
            protocol_lines=cleaned,
        )

    def delete_summaries(self) -> list[str]:
        if self._serial_request is None:
            raise RuntimeError("Serial request transport is unavailable")
        return self._serial_request(
            "SUMMARY_DELETE_ALL",
            lambda line: (
                line.startswith("OK SUMMARY_DELETE_ALL")
                or line.startswith("ERR SUMMARY_DELETE_ALL")
            ),
            8000,
            True,
        )

    def delete_all_media(self) -> DeviceActionResult:
        """Stop acquisition, delete SD media, and leave capture stopped."""
        if self._serial_request is None:
            return DeviceActionResult(("Delete-all error: serial transport unavailable",))

        messages = ["Starting password-authorized delete-all operation..."]
        success = False
        rebooted = False
        lines: list[str] = []
        try:
            stop_lines = self._serial_request(
                "STOP_PROGRAM",
                lambda line: (
                    line.startswith("OK STOP_PROGRAM")
                    or line.startswith("ERR STOP_PROGRAM")
                ),
                20000,
                True,
            )
            stopped = any(line.startswith("OK STOP_PROGRAM") for line in stop_lines)
            if not stopped:
                raise RuntimeError("firmware did not confirm that auto capture stopped")
            messages.append("Stopped auto capture before delete-all.")

            raw_lines = self._serial_request(
                "IMG_DELETE_ALL",
                lambda line: (
                    line.startswith("OK IMG_DELETE_ALL")
                    or line.startswith("ERR IMG_DELETE_ALL")
                ),
                120000,
                True,
            )
            lines, ignored, rebooted = media_protocol.extract_relevant_lines(
                raw_lines, media_protocol.is_expected_delete_all_line
            )
            messages.extend(f"RX: {line}" for line in lines)
            if ignored:
                messages.append(
                    f"Ignored {len(ignored)} non-protocol line(s) during IMG_DELETE_ALL."
                )
            if rebooted:
                messages.append("Delete-all aborted: device rebooted.")
            else:
                success = any(line.startswith("OK IMG_DELETE_ALL") for line in lines)
                if success:
                    summary_lines = self._serial_request(
                        "SUMMARY_DELETE_ALL",
                        lambda line: (
                            line.startswith("OK SUMMARY_DELETE_ALL")
                            or line.startswith("ERR SUMMARY_DELETE_ALL")
                        ),
                        30000,
                        True,
                    )
                    messages.extend(
                        f"RX: {line}" for line in summary_lines
                        if line.startswith(("OK SUMMARY_DELETE_ALL", "ERR SUMMARY_DELETE_ALL"))
                    )
                    summaries_deleted = any(
                        line.startswith("OK SUMMARY_DELETE_ALL") for line in summary_lines
                    )
                    success = success and summaries_deleted
                    if success:
                        messages.append("All SD images, videos, and summaries were deleted successfully.")
                    else:
                        messages.append("Images were deleted, but daily summaries could not all be deleted.")
                elif any(line.startswith("ERR IMG_DELETE_ALL") for line in lines):
                    messages.append(
                        "Delete-all reported an error. Check the response above."
                    )
                else:
                    messages.append("No delete-all response received from firmware.")
        except Exception as exc:
            messages.append(f"Delete-all error: {exc}")
        return DeviceActionResult(
            tuple(messages),
            success=success,
            rebooted=rebooted,
            protocol_lines=tuple(lines),
        )
