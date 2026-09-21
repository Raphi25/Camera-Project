"""Transport-independent execution and cleanup of ordinary commands."""

import asyncio
import datetime as dt
import time
from dataclasses import dataclass


@dataclass(frozen=True)
class CommandResult:
    lines: tuple[str, ...] = ()
    error: str | None = None
    refresh_ports: bool = False
    connection_closed: bool = False


def clean_protocol_line(line: str, command: str) -> str:
    """Recover protocol replies attached to command echoes or ESP-IDF logs."""
    upper_command = command.strip().upper()
    markers = ["ERR ", "OK "]
    if upper_command.startswith("GETTIME"):
        markers.insert(0, "TIME ")
    elif upper_command.startswith("IMG_COUNT"):
        markers.insert(0, "COUNT ")
    elif upper_command.startswith("STATUS"):
        markers.insert(0, "STATUS ")
    elif upper_command.startswith("SD_STATUS"):
        markers.insert(0, "SD_STATUS ")
    elif upper_command.startswith("SUMMARY_LIST"):
        markers.insert(0, "SUMMARY ")
    elif upper_command.startswith("SUMMARY_GET"):
        markers[:0] = ["END_SUMMARY ", "TEXT ", "BEGIN_SUMMARY "]
    elif upper_command.startswith("SUMMARY_DELETE_ALL"):
        markers[:0] = ["ERR SUMMARY_DELETE_ALL", "OK SUMMARY_DELETE_ALL"]

    for marker in markers:
        index = line.find(marker)
        if index > 0:
            return line[index:]
    return line


class CommandExecutor:
    """Execute serial or BLE commands behind one UI-independent contract."""

    def __init__(
        self,
        serial_transport,
        ble_transport,
        serial_module,
        serial_retry_count: int,
        serial_response_window_ms: int,
    ) -> None:
        self._serial_transport = serial_transport
        self._ble_transport = ble_transport
        self._serial_module = serial_module
        self._serial_retry_count = serial_retry_count
        self._serial_response_window_ms = serial_response_window_ms

    def execute_ble(self, target: str, command: str) -> CommandResult:
        try:
            lines = asyncio.run(self._ble_transport.request(target, command))
            return CommandResult(lines=tuple(lines))
        except Exception as exc:
            return CommandResult(error=f"BT command error: {exc}")

    def execute_serial(self, port: str, baud: int, command: str) -> CommandResult:
        """Send one command, retry write timeouts, and normalize the response."""
        payload = (command + "\n").encode("ascii", errors="ignore")
        upper_command = command.strip().upper()
        is_sleep_command = upper_command in {"DEEP_SLEEP", "DEVICE_OFF"}

        for attempt in range(1, self._serial_retry_count + 1):
            try:
                connection = self._serial_transport.open(port, baud)
                with self._serial_transport.lock:
                    connection.write(payload)
                    connection.flush()
                    deadline = dt.datetime.now() + dt.timedelta(
                        milliseconds=self._serial_response_window_ms
                    )
                    lines = self._serial_transport.read_lines_until(connection, deadline)
                cleaned = tuple(clean_protocol_line(line, command) for line in lines)

                if is_sleep_command:
                    self._serial_transport.close()
                    return CommandResult(lines=cleaned, connection_closed=True)
                return CommandResult(lines=cleaned)
            except PermissionError:
                return CommandResult(
                    error="Port is busy. Close idf.py monitor (or any serial app) then retry."
                )
            except Exception as exc:
                timeout_type = getattr(self._serial_module, "SerialTimeoutException", ())
                if timeout_type and isinstance(exc, timeout_type):
                    self._serial_transport.close()
                    if attempt < self._serial_retry_count:
                        time.sleep(0.08)
                        continue
                    return CommandResult(
                        error="Serial error: Write timeout (device not accepting writes)."
                    )

                self._serial_transport.close()
                error_text = str(exc)
                disconnected_while_sleeping = is_sleep_command and any(
                    marker in error_text
                    for marker in (
                        "WriteFile failed",
                        "could not open port",
                        "PermissionError",
                        "FileNotFoundError",
                    )
                )
                if disconnected_while_sleeping:
                    return CommandResult(
                        error=(
                            "Serial disconnected while sending the deep-sleep command. "
                            "The device may already be asleep or rebooting; refresh ports "
                            "after wake/reset."
                        ),
                        refresh_ports=True,
                    )
                return CommandResult(error=f"Serial error: {exc}")

        return CommandResult(error="Serial command failed.")
