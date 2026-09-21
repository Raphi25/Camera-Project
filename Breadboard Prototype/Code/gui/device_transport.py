"""Reusable USB-serial and BLE transports for the desktop clients."""

import asyncio
import datetime as dt
import threading
from collections.abc import Callable

from media_protocol import BinaryDataFrame

from protocol import (
    PROTOCOL_CLIENT_MAX_VERSION,
    PROTOCOL_LEGACY_VERSION,
    parse_protocol_response,
)


LinePredicate = Callable[[str], bool]
LogCallback = Callable[[str], None]


class SerialTransport:
    """Own a reusable serial connection and the device protocol session."""

    def __init__(self, serial_module, log: LogCallback) -> None:
        self._serial = serial_module
        self._log = log
        self._connection = None
        self._port: str | None = None
        self._baud: int | None = None
        self._protocol: dict | None = None
        self.lock = threading.Lock()

    @property
    def protocol(self) -> dict | None:
        return self._protocol

    @property
    def port(self) -> str | None:
        return self._port

    def close(self) -> None:
        with self.lock:
            if self._connection is not None:
                try:
                    self._connection.close()
                except Exception:
                    pass
            self._connection = None
            self._port = None
            self._baud = None
            self._protocol = None

    def open(self, port: str, baud: int):
        """Open or reuse a connection; negotiation is deferred until a request."""
        with self.lock:
            if (
                self._connection is not None
                and self._connection.is_open
                and self._port == port
                and self._baud == baud
            ):
                return self._connection

            if self._connection is not None:
                try:
                    self._connection.close()
                except Exception:
                    pass

            connection = self._serial.Serial()
            connection.port = port
            connection.baudrate = baud
            connection.timeout = 0.2
            connection.write_timeout = None
            connection.xonxoff = False
            connection.rtscts = False
            connection.dsrdtr = False
            try:
                connection.dtr = False
                connection.rts = False
            except Exception:
                pass

            connection.open()
            try:
                connection.setDTR(False)
                connection.setRTS(False)
            except Exception:
                pass
            connection.reset_input_buffer()
            connection.reset_output_buffer()

            self._connection = connection
            self._port = port
            self._baud = baud
            self._protocol = None
            self._log(f"Connected {port} @ {baud}")
            return connection

    @staticmethod
    def read_lines_until(connection, deadline: dt.datetime,
                         is_done: LinePredicate | None = None,
                         on_line: Callable[[str], None] | None = None,
                         collect_lines: bool = True) -> list[str]:
        lines: list[str] = []
        pending = bytearray()

        while dt.datetime.now() < deadline:
            read_count = connection.in_waiting
            chunk = connection.read(read_count if read_count > 0 else 1)
            if not chunk:
                continue
            pending.extend(chunk)

            while True:
                newline_index = pending.find(b"\n")
                if newline_index < 0:
                    break
                raw_line = pending[:newline_index]
                del pending[:newline_index + 1]
                decoded = raw_line.decode(errors="ignore").strip()
                if not decoded:
                    continue
                if collect_lines:
                    lines.append(decoded)
                if on_line is not None:
                    on_line(decoded)
                if is_done and is_done(decoded):
                    return lines

        trailing = pending.decode(errors="ignore").strip()
        if trailing and collect_lines:
            lines.append(trailing)
        if trailing and on_line is not None:
            on_line(trailing)
        return lines

    def _negotiate_locked(self, connection) -> None:
        """Negotiate once while the caller holds the transport session lock."""
        if self._protocol is not None:
            return

        connection.reset_input_buffer()
        connection.write(f"PROTOCOL {PROTOCOL_CLIENT_MAX_VERSION}\n".encode("ascii"))
        connection.flush()
        deadline = dt.datetime.now() + dt.timedelta(milliseconds=700)
        lines = self.read_lines_until(
            connection,
            deadline,
            is_done=lambda line: (
                line.startswith("PROTOCOL ")
                or line.startswith("ERR PROTOCOL")
                or "Unknown command" in line
            ),
        )
        protocol = parse_protocol_response(lines)
        if protocol is None:
            protocol = {
                "selected": PROTOCOL_LEGACY_VERSION,
                "current": PROTOCOL_LEGACY_VERSION,
                "minimum": PROTOCOL_LEGACY_VERSION,
                "frame": 1,
                "capabilities": frozenset(),
                "legacy": True,
            }
        self._protocol = protocol
        self._log(
            f"Protocol v{protocol['selected']} negotiated"
            + (" (legacy fallback)" if protocol["legacy"] else "")
        )

    def request_until(self, port: str, baud: int, command: str,
                      is_done: LinePredicate | None, timeout_ms: int,
                      log_send: bool = True) -> list[str]:
        payload = (command + "\n").encode("ascii", errors="ignore")
        connection = self.open(port, baud)
        try:
            with self.lock:
                self._negotiate_locked(connection)
                if log_send:
                    self._log(f"Sending: {command}")
                connection.reset_input_buffer()
                connection.write(payload)
                connection.flush()
                deadline = dt.datetime.now() + dt.timedelta(milliseconds=timeout_ms)
                return self.read_lines_until(connection, deadline, is_done)
        except Exception:
            # USB Serial/JTAG disappears during ESP32 deep sleep. PySerial may
            # leave is_open=True on that dead Windows handle, so explicitly
            # discard it and let the next dashboard poll reopen the new COM5
            # device instance and renegotiate the protocol.
            self.close()
            raise

    def request_until_stream(self, port: str, baud: int, command: str,
                             is_done: LinePredicate | None, timeout_ms: int,
                             on_line: Callable[[str], None],
                             log_send: bool = True) -> None:
        """Deliver response lines incrementally without retaining the response."""
        payload = (command + "\n").encode("ascii", errors="ignore")
        connection = self.open(port, baud)
        try:
            with self.lock:
                self._negotiate_locked(connection)
                if log_send:
                    self._log(f"Sending: {command}")
                connection.reset_input_buffer()
                connection.write(payload)
                connection.flush()
                deadline = dt.datetime.now() + dt.timedelta(milliseconds=timeout_ms)
                self.read_lines_until(
                    connection,
                    deadline,
                    is_done,
                    on_line=on_line,
                    collect_lines=False,
                )
        except Exception:
            self.close()
            raise

    @staticmethod
    def _read_exact(connection, size: int, deadline: dt.datetime) -> bytes:
        data = bytearray()
        while len(data) < size and dt.datetime.now() < deadline:
            chunk = connection.read(size - len(data))
            if chunk:
                data.extend(chunk)
        if len(data) != size:
            raise TimeoutError(f"binary media frame ended at {len(data)}/{size} bytes")
        return bytes(data)

    @staticmethod
    def _seek_binary_magic(connection, deadline: dt.datetime) -> bytes:
        """Find the next RIMG header, tolerating bounded stray console output."""
        window = bytearray()
        skipped = bytearray()
        while dt.datetime.now() < deadline and len(skipped) <= 4096:
            byte = connection.read(1)
            if not byte:
                continue
            window.extend(byte)
            if len(window) > 4:
                skipped.append(window.pop(0))
            if window == b"RIMG":
                return bytes(skipped)
        preview = bytes(skipped + window)[:64].hex(" ")
        raise RuntimeError(
            "binary media frame magic not found"
            + (f"; received: {preview}" if preview else "")
        )

    def request_until_binary_stream(
        self, port: str, baud: int, command: str, timeout_ms: int,
        on_line: Callable[[str | BinaryDataFrame], None], log_send: bool = True,
    ) -> None:
        """Parse raw RIMG frames without buffering or text-encoding payloads."""
        connection = self.open(port, baud)
        with self.lock:
            self._negotiate_locked(connection)
            if log_send:
                self._log(f"Sending: {command}")
            connection.reset_input_buffer()
            connection.write((command + "\n").encode("ascii"))
            connection.flush()
            deadline = dt.datetime.now() + dt.timedelta(milliseconds=timeout_ms)

            begin = ""
            while dt.datetime.now() < deadline and not begin:
                raw = connection.readline()
                if raw:
                    begin = raw.decode("ascii", errors="replace").strip()
            if not begin:
                raise TimeoutError("binary media transfer did not send BEGIN")
            on_line(begin)
            if begin.startswith("ERR IMG_GET"):
                return
            parts = begin.split()
            if len(parts) < 4 or parts[0] != "BEGIN":
                raise RuntimeError(f"invalid binary media BEGIN: {begin}")
            total = int(parts[2])
            offset = int(parts[3])
            remaining = total - offset
            received = 0

            # BEGIN and END remain line-oriented recovery points, but bytes
            # between them are framed binary. Console logs can be interleaved,
            # so every iteration resynchronizes on the bounded RIMG magic scan.
            while received < remaining:
                skipped = self._seek_binary_magic(connection, deadline)
                if skipped:
                    preview = skipped[:80].decode("ascii", errors="replace").strip()
                    self._log(
                        f"Binary media: ignored {len(skipped)} stray byte(s)"
                        + (f": {preview}" if preview else ".")
                    )
                header = b"RIMG" + self._read_exact(connection, 8, deadline)
                sequence = int.from_bytes(header[4:8], "little")
                length = int.from_bytes(header[8:10], "little")
                crc = int.from_bytes(header[10:12], "little")
                if length <= 0 or received + length > remaining:
                    raise RuntimeError("binary media frame length is invalid")
                payload = self._read_exact(connection, length, deadline)
                # CRC validation and file hashing live in MediaDownloader; the
                # transport owns only framing and exact byte delivery.
                on_line(BinaryDataFrame(sequence, payload, crc))
                received += length

            end = ""
            while dt.datetime.now() < deadline and not end:
                raw = connection.readline()
                if raw:
                    end = raw.decode("ascii", errors="replace").strip()
            if not end:
                raise TimeoutError("binary media transfer did not send END")
            on_line(end)


class BleTransport:
    """Own BLE discovery, notification framing, and protocol negotiation."""

    def __init__(
        self,
        client_type,
        scanner_type,
        command_uuid: str,
        response_uuid: str,
        scan_timeout_s: float,
        response_window_s: float,
        log: LogCallback,
    ) -> None:
        self._client_type = client_type
        self._scanner_type = scanner_type
        self._command_uuid = command_uuid
        self._response_uuid = response_uuid
        self._scan_timeout_s = scan_timeout_s
        self._response_window_s = response_window_s
        self._log = log
        self._protocol_by_address: dict[str, dict] = {}

    async def discover(self, target: str):
        """Find a BLE peripheral by exact address or partial advertised name."""
        target_lower = target.lower()
        devices = await self._scanner_type.discover(timeout=self._scan_timeout_s)
        for device in devices:
            name = device.name or ""
            address = device.address or ""
            if target_lower == address.lower() or target_lower in name.lower():
                return device
        return None

    async def request(
        self,
        target: str,
        command: str,
        log_send: bool = True,
        response_window_s: float | None = None,
    ) -> list[str]:
        """Send one command and assemble newline-delimited BLE notifications."""
        device = await self.discover(target)
        address = device.address if device is not None else target
        lines: list[str] = []
        receive_buffer = ""

        def on_notify(_sender, data: bytearray) -> None:
            nonlocal receive_buffer
            receive_buffer += bytes(data).decode(errors="ignore").replace("\r", "")
            while "\n" in receive_buffer:
                part, receive_buffer = receive_buffer.split("\n", 1)
                part = part.strip()
                if part:
                    lines.append(part)

        if log_send:
            self._log(f"BT connecting: {address}")
        async with self._client_type(address) as client:
            if not client.is_connected:
                raise RuntimeError("BT connection failed")

            await client.start_notify(self._response_uuid, on_notify)
            if address not in self._protocol_by_address:
                await client.write_gatt_char(
                    self._command_uuid,
                    f"PROTOCOL {PROTOCOL_CLIENT_MAX_VERSION}\n".encode("utf-8"),
                    response=True,
                )
                await asyncio.sleep(0.5)
                protocol = parse_protocol_response(lines)
                if protocol is None:
                    protocol = {
                        "selected": PROTOCOL_LEGACY_VERSION,
                        "current": PROTOCOL_LEGACY_VERSION,
                        "minimum": PROTOCOL_LEGACY_VERSION,
                        "frame": 1,
                        "capabilities": frozenset(),
                        "legacy": True,
                    }
                self._protocol_by_address[address] = protocol
                self._log(
                    f"BT protocol v{protocol['selected']} negotiated"
                    + (" (legacy fallback)" if protocol["legacy"] else "")
                )
                lines.clear()
                receive_buffer = ""

            if log_send:
                self._log(f"BT sending: {command}")
            await client.write_gatt_char(
                self._command_uuid,
                (command + "\n").encode("utf-8"),
                response=True,
            )
            await asyncio.sleep(
                self._response_window_s
                if response_window_s is None
                else response_window_s
            )
            await client.stop_notify(self._response_uuid)

        trailing = receive_buffer.strip()
        if trailing:
            lines.append(trailing)
        return lines
