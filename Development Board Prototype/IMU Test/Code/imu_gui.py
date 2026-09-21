"""Quick BMI323 logger transfer and analysis GUI for Windows."""

from __future__ import annotations

import queue
import subprocess
import sys
import threading
import tkinter as tk
import time
import traceback
import zlib
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None

DEFAULT_OUTPUT = Path(__file__).resolve().parent / "Data"
ERROR_LOG = Path(__file__).resolve().parent / "imu_gui_error.log"


class ImuGui(tk.Tk):
    """Tk front end for logger control, verified transfer, and offline analysis."""

    def __init__(self) -> None:
        super().__init__()
        self.title("BMI323 IMU Logger")
        self.geometry("780x450")
        self.minsize(740, 400)
        self.serial_connection = None
        self.serial_lock = threading.Lock()
        self.ui_queue: queue.Queue[tuple[str, object]] = queue.Queue()
        self.port = tk.StringVar(value="COM4")
        self.baud = tk.StringVar(value="115200")
        self.source = tk.StringVar()
        self.destination = tk.StringVar(value=str(DEFAULT_OUTPUT))
        self.status = tk.StringVar(value="Ready")
        self._build_ui()
        self.refresh_ports()
        self.after(50, self._drain_ui_queue)
        self.protocol("WM_DELETE_WINDOW", self.close)

    def _build_ui(self) -> None:
        frame = ttk.Frame(self, padding=16)
        frame.pack(fill="both", expand=True)
        ttk.Label(frame, text="BMI323 IMU Logger", font=("Segoe UI", 18, "bold")).pack(anchor="w")
        ttk.Label(
            frame,
            text="Download IMU logs and derive chest-motion profiles for camera blur testing.",
        ).pack(anchor="w", pady=(0, 16))

        connection = ttk.LabelFrame(frame, text="Device")
        connection.pack(fill="x", pady=6)
        ttk.Label(connection, text="Port").grid(row=0, column=0, padx=8, pady=8)
        self.port_box = ttk.Combobox(connection, textvariable=self.port, width=12, state="readonly")
        self.port_box.grid(row=0, column=1, padx=8, pady=8)
        ttk.Label(connection, text="Baud").grid(row=0, column=2, padx=8, pady=8)
        ttk.Entry(connection, textvariable=self.baud, width=10).grid(row=0, column=3, padx=8, pady=8)
        ttk.Button(connection, text="Refresh", command=self.refresh_ports).grid(row=0, column=4, padx=8, pady=8)
        self.start_button = ttk.Button(connection, text="Start Logging", command=self.start_logging)
        self.start_button.grid(row=0, column=5, padx=8, pady=8)
        self.stop_button = ttk.Button(connection, text="Stop Logging", command=self.stop_logging)
        self.stop_button.grid(row=0, column=6, padx=8, pady=8)
        self.status_button = ttk.Button(connection, text="Status", command=self.request_status)
        self.status_button.grid(row=0, column=7, padx=8, pady=8)

        transfer = ttk.LabelFrame(frame, text="Transfer")
        transfer.pack(fill="x", pady=6)
        ttk.Label(transfer, text="PC log").grid(row=0, column=0, padx=8, pady=8, sticky="w")
        ttk.Entry(transfer, textvariable=self.source, width=58).grid(row=0, column=1, padx=8, pady=8)
        self.download_button = ttk.Button(transfer, text="Download All", command=self.download_all)
        self.download_button.grid(row=0, column=2, padx=8, pady=8)
        ttk.Label(transfer, text="Save to").grid(row=1, column=0, padx=8, pady=8, sticky="w")
        ttk.Entry(transfer, textvariable=self.destination, width=58).grid(row=1, column=1, padx=8, pady=8)
        ttk.Button(transfer, text="Browse", command=self.choose_destination).grid(row=1, column=2, padx=8, pady=8)
        self.analysis_button = ttk.Button(
            transfer, text="Analyze Body Motion", command=self.analyze_log
        )
        self.analysis_button.grid(row=2, column=1, padx=8, pady=8, sticky="w")
        self.delete_button = ttk.Button(transfer, text="Delete All Logs", command=self.delete_all_logs)
        self.delete_button.grid(row=2, column=2, padx=8, pady=8)

        self.output = tk.Text(frame, height=10, state="disabled", wrap="word")
        self.output.pack(fill="both", expand=True, pady=(12, 4))
        ttk.Label(frame, textvariable=self.status, relief="sunken", anchor="w").pack(fill="x")

    def log(self, text: str) -> None:
        self.output.configure(state="normal")
        self.output.insert("end", f"{datetime.now():%H:%M:%S}  {text}\n")
        self.output.see("end")
        self.output.configure(state="disabled")
        self.status.set(text)

    def refresh_ports(self) -> None:
        if list_ports is None:
            self.port_box["values"] = [self.port.get()]
            self.log("Install pyserial with: python -m pip install pyserial")
            return
        port_info = list(list_ports.comports())
        ports = [item.device for item in port_info]
        self.port_box["values"] = ports or [self.port.get()]
        if ports and self.port.get() not in ports:
            self.port.set(ports[0])
        details = ", ".join(f"{item.device} ({item.description})" for item in port_info)
        self.log(f"Ports found: {details or 'none'}")

    def _selected_connection(self) -> tuple[str, int] | None:
        """Validate GUI connection fields before launching a worker thread."""
        if serial is None:
            messagebox.showerror("Missing dependency", "Install pyserial first.")
            return None
        try:
            return self.port.get(), int(self.baud.get())
        except ValueError:
            self.log("Invalid baud rate.")
            return None

    def stop_logging(self) -> None:
        settings = self._selected_connection()
        if settings is None:
            return
        port, baud = settings
        self.stop_button.configure(state="disabled")
        self.log("Stopping logger and flushing the SD-card buffer...")
        threading.Thread(
            target=self._stop_logging_worker, args=(port, baud), daemon=True
        ).start()

    def start_logging(self) -> None:
        settings = self._selected_connection()
        if settings is None:
            return
        port, baud = settings
        self.start_button.configure(state="disabled")
        self.log("Synchronizing the RTC and starting a new IMU log...")
        pc_time = datetime.now().replace(microsecond=0).strftime("%Y-%m-%d %H:%M:%S")
        threading.Thread(
            target=self._start_logging_worker,
            args=(port, baud, pc_time),
            daemon=True,
        ).start()

    def _start_logging_worker(self, port: str, baud: int, pc_time: str) -> None:
        try:
            with self.serial_lock:
                connection = self._open_serial_locked(port, baud)
                self._request_locked(
                    connection,
                    "IMU_LOG_STOP",
                    ("OK IMU_LOG_STOP", "ERR IMU_LOG_STOP"),
                )
                rtc_response = self._request_locked(
                    connection,
                    f"RTC_SET {pc_time}",
                    ("OK RTC_SET", "ERR RTC_SET"),
                )
                response = self._request_locked(
                    connection,
                    "IMU_LOG_START",
                    ("OK IMU_LOG_START", "ERR IMU_LOG_START"),
                )
            self.ui_queue.put(("start_done", f"{rtc_response}; {response}"))
        except Exception as exc:
            with self.serial_lock:
                self._close_serial_connection()
            self.ui_queue.put(("start_error", str(exc)))

    def _stop_logging_worker(self, port: str, baud: int) -> None:
        response, error = self._send_command_io(
            "IMU_LOG_STOP", "OK IMU_LOG_STOP", port, baud
        )
        if error:
            self.ui_queue.put(("stop_error", error))
        else:
            self.ui_queue.put(("stop_done", response or "No response from the firmware"))

    def _drain_ui_queue(self) -> None:
        """Apply worker results on Tk's main thread; workers never touch widgets."""
        try:
            while True:
                action, message = self.ui_queue.get_nowait()
                if action in ("stop_done", "stop_error"):
                    self.stop_button.configure(state="normal")
                    prefix = "Serial error: " if action == "stop_error" else "Device: "
                    self.log(prefix + message)
                elif action in ("start_done", "start_error"):
                    self.start_button.configure(state="normal")
                    prefix = "Start error: " if action == "start_error" else "Device: "
                    self.log(prefix + str(message))
                elif action in ("status_done", "status_error"):
                    self.status_button.configure(state="normal")
                    prefix = "Serial error: " if action == "status_error" else "Device: "
                    self.log(prefix + message)
                elif action == "transfer_progress":
                    self.log(str(message))
                elif action == "transfer_done":
                    self.download_button.configure(state="normal")
                    paths = [Path(item) for item in message]
                    newest = paths[-1]
                    self.source.set(str(newest))
                    self.log(
                        f"Downloaded and verified {len(paths)} file(s) to: "
                        f"{newest.parent}"
                    )
                elif action == "transfer_error":
                    self.download_button.configure(state="normal")
                    self.log(f"Transfer error: {message}")
                elif action in ("delete_done", "delete_error"):
                    self.delete_button.configure(state="normal")
                    prefix = "Delete error: " if action == "delete_error" else "Device: "
                    self.log(prefix + str(message))
                elif action in ("analysis_done", "analysis_error"):
                    self.analysis_button.configure(state="normal")
                    if action == "analysis_error":
                        self.log(f"Analysis error: {message}")
                        messagebox.showerror("Analysis failed", str(message))
                    else:
                        self.log(str(message))
        except queue.Empty:
            pass
        self.after(50, self._drain_ui_queue)

    def request_status(self) -> None:
        settings = self._selected_connection()
        if settings is None:
            return
        port, baud = settings
        self.status_button.configure(state="disabled")
        self.log("Requesting device status...")
        threading.Thread(
            target=self._status_worker, args=(port, baud), daemon=True
        ).start()

    def _status_worker(self, port: str, baud: int) -> None:
        response, error = self._send_command_io("STATUS", "STATUS ", port, baud)
        if error:
            self.ui_queue.put(("status_error", error))
        else:
            self.ui_queue.put(("status_done", response or "No response from the firmware"))

    def _send_command_io(
        self,
        command: str,
        expected_prefix: str | tuple[str, ...],
        port: str,
        baud: int,
    ) -> tuple[str, str]:
        if serial is None:
            return "", "pyserial is not installed"
        try:
            with self.serial_lock:
                return self._send_command_locked(
                    command, expected_prefix, port, baud
                ), ""
        except Exception as exc:
            with self.serial_lock:
                self._close_serial_connection()
            return "", str(exc)

    def _send_command_locked(
        self,
        command: str,
        expected_prefix: str | tuple[str, ...],
        port: str,
        baud: int,
    ) -> str:
        """Perform serial I/O while the caller holds serial_lock."""
        connection = self._open_serial_locked(port, baud)
        connection.write(f"{command}\n".encode())
        connection.flush()
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            line = connection.readline().decode(errors="replace").strip()
            if line.startswith(expected_prefix):
                return line
        return ""

    def _open_serial_locked(self, port: str, baud: int):
        """Reuse one non-resetting USB serial session; caller holds serial_lock."""
        if self.serial_connection is not None and self.serial_connection.is_open:
            return self.serial_connection
        connection = serial.Serial()
        connection.port = port
        connection.baudrate = baud
        connection.timeout = 0.2
        connection.write_timeout = 2
        connection.xonxoff = False
        connection.rtscts = False
        connection.dsrdtr = False
        connection.dtr = False
        connection.rts = False
        connection.open()
        connection.setDTR(False)
        connection.setRTS(False)
        connection.reset_input_buffer()
        connection.reset_output_buffer()
        self.serial_connection = connection
        return connection

    def _close_serial_connection(self) -> None:
        """Close and forget the cached connection after shutdown or I/O failure."""
        connection = self.serial_connection
        self.serial_connection = None
        if connection is not None:
            try:
                connection.close()
            except Exception:
                pass

    def report_callback_exception(self, exc_type, exc_value, exc_traceback) -> None:
        """Keep callback failures visible and save their traceback for diagnosis."""
        details = "".join(traceback.format_exception(exc_type, exc_value, exc_traceback))
        try:
            ERROR_LOG.write_text(details, encoding="utf-8")
        except OSError:
            pass
        self.log(f"GUI error: {exc_value}. Details saved to {ERROR_LOG.name}")
        messagebox.showerror(
            "BMI323 Logger error",
            f"{exc_value}\n\nDetails were saved to:\n{ERROR_LOG}",
        )

    def close(self) -> None:
        with self.serial_lock:
            self._close_serial_connection()
        self.destroy()

    def download_all(self) -> None:
        settings = self._selected_connection()
        if settings is None:
            return
        port, baud = settings
        destination = Path(self.destination.get())
        self.download_button.configure(state="disabled")
        self.log("Stopping logging and requesting all SD-card CSV files...")
        threading.Thread(
            target=self._download_all_worker,
            args=(port, baud, destination),
            daemon=True,
        ).start()

    def delete_all_logs(self) -> None:
        settings = self._selected_connection()
        if settings is None:
            return
        if not messagebox.askyesno(
            "Delete all SD logs?",
            "Permanently delete every IMU run CSV from the device SD card?\n\n"
            "Files already downloaded to this PC will not be deleted.",
            icon="warning",
        ):
            return
        port, baud = settings
        self.delete_button.configure(state="disabled")
        self.log("Stopping logging and deleting all IMU run CSVs from the SD card...")
        threading.Thread(
            target=self._delete_all_logs_worker,
            args=(port, baud),
            daemon=True,
        ).start()

    def _delete_all_logs_worker(self, port: str, baud: int) -> None:
        try:
            with self.serial_lock:
                connection = self._open_serial_locked(port, baud)
                self._request_locked(
                    connection,
                    "IMU_LOG_STOP",
                    ("OK IMU_LOG_STOP", "ERR IMU_LOG_STOP"),
                )
                response = self._request_locked(
                    connection,
                    "LOG_DELETE_ALL",
                    ("OK LOG_DELETE_ALL", "ERR LOG_DELETE_ALL"),
                    timeout=10.0,
                )
            self.ui_queue.put(("delete_done", response))
        except Exception as exc:
            with self.serial_lock:
                self._close_serial_connection()
            self.ui_queue.put(("delete_error", str(exc)))

    def _read_until_locked(self, connection, done, timeout: float) -> list[str]:
        """Collect firmware lines until a caller-supplied completion marker arrives."""
        lines: list[str] = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = connection.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip("\r\n")
            lines.append(line)
            if done(line):
                return lines
        raise TimeoutError("Device response timed out")

    def _write_command_locked(self, connection, command: str) -> None:
        connection.reset_input_buffer()
        connection.write((command + "\n").encode("ascii"))
        connection.flush()

    def _request_locked(
        self,
        connection,
        command: str,
        response_prefixes: tuple[str, ...],
        timeout: float = 5.0,
    ) -> str:
        """Send a simple command and return its terminal OK response."""
        self._write_command_locked(connection, command)
        lines = self._read_until_locked(
            connection,
            lambda line: line.startswith(response_prefixes),
            timeout,
        )
        response = next(line for line in lines if line.startswith(response_prefixes))
        if response.startswith("ERR "):
            raise RuntimeError(response)
        return response

    def _receive_log_locked(
        self,
        connection,
        name: str,
        expected_size: int,
        destination: Path,
        file_number: int,
        file_count: int,
    ) -> Path:
        """Receive one framed CSV into a temporary file and verify size and CRC."""
        target = destination / name
        temp_path = target.with_suffix(target.suffix + ".part")
        self.ui_queue.put(
            (
                "transfer_progress",
                f"Downloading file {file_number}/{file_count}: {name} "
                f"({expected_size:,} bytes)...",
            )
        )

        try:
            self._write_command_locked(connection, f"LOG_GET {name}")
            started_at = time.monotonic()
            # Windows may briefly pause a USB serial reader. The firmware
            # retries transmit back-pressure, so allow the same pause here.
            idle_deadline = started_at + 45.0
            hard_deadline = started_at + max(120.0, expected_size / 250.0 + 60.0)
            next_progress = 65536
            received = 0
            crc = 0
            began = False
            completed = False
            with temp_path.open("wb") as output:
                while time.monotonic() < hard_deadline:
                    raw = connection.readline()
                    if not raw:
                        if time.monotonic() >= idle_deadline:
                            raise TimeoutError(
                                f"CSV transfer stalled after {received:,} / "
                                f"{expected_size:,} bytes for {name}"
                            )
                        continue
                    idle_deadline = time.monotonic() + 45.0
                    line = raw.decode("utf-8", errors="strict").strip("\r\n")
                    if line.startswith("BEGIN_LOG "):
                        parts = line.split()
                        if (
                            len(parts) != 3
                            or parts[1] != name
                            or int(parts[2]) != expected_size
                        ):
                            raise RuntimeError(f"Invalid start-of-transfer marker for {name}")
                        began = True
                        continue
                    if line.startswith("CSV ") and began:
                        payload = line[4:].encode("utf-8") + b"\n"
                        output.write(payload)
                        received += len(payload)
                        crc = zlib.crc32(payload, crc)
                        if received >= next_progress:
                            percent = min(100.0, received * 100.0 / expected_size)
                            self.ui_queue.put(
                                (
                                    "transfer_progress",
                                    f"File {file_number}/{file_count}: "
                                    f"{received:,} / {expected_size:,} bytes "
                                    f"({percent:.0f}%)",
                                )
                            )
                            next_progress += 65536
                        continue
                    if line.startswith("ERR LOG_GET"):
                        raise RuntimeError(f"{name}: {line}")
                    if line.startswith("END_LOG "):
                        parts = line.split()
                        if len(parts) != 4 or parts[1] != name:
                            raise RuntimeError(f"Invalid end-of-transfer marker for {name}")
                        device_bytes = int(parts[2])
                        device_crc = int(parts[3], 16)
                        if received != expected_size or received != device_bytes:
                            raise RuntimeError(
                                f"{name}: size mismatch: received {received}, inventory "
                                f"{expected_size}, device sent {device_bytes}"
                            )
                        if crc != device_crc:
                            raise RuntimeError(
                                f"{name}: CRC mismatch: PC {crc:08x}, "
                                f"device {device_crc:08x}"
                            )
                        completed = True
                        break
                if not completed:
                    raise TimeoutError(
                        f"{name}: transfer exceeded its safety limit after "
                        f"{received:,} / {expected_size:,} bytes"
                    )
            temp_path.replace(target)
            return target
        except Exception:
            temp_path.unlink(missing_ok=True)
            raise

    def _download_all_worker(self, port: str, baud: int, destination: Path) -> None:
        """Stop logging, inventory the card, and download every completed run."""
        try:
            with self.serial_lock:
                connection = self._open_serial_locked(port, baud)
                self._request_locked(
                    connection,
                    "IMU_LOG_STOP",
                    ("OK IMU_LOG_STOP", "ERR IMU_LOG_STOP"),
                )

                self._write_command_locked(connection, "LOG_LIST")
                list_lines = self._read_until_locked(
                    connection,
                    lambda line: line.startswith(("OK LOG_LIST", "ERR LOG_LIST")),
                    5.0,
                )
                error = next((line for line in list_lines if line.startswith("ERR LOG_LIST")), None)
                if error:
                    raise RuntimeError(error)
                logs: list[tuple[str, int]] = []
                for line in list_lines:
                    parts = line.split()
                    if len(parts) == 3 and parts[0] == "LOG":
                        try:
                            logs.append((parts[1], int(parts[2])))
                        except ValueError:
                            continue
                if not logs:
                    raise RuntimeError("No completed CSV logs were found on the SD card")
                destination.mkdir(parents=True, exist_ok=True)
                downloaded: list[Path] = []
                ordered_logs = sorted(logs, key=lambda item: item[0])
                for number, (name, expected_size) in enumerate(ordered_logs, start=1):
                    downloaded.append(
                        self._receive_log_locked(
                            connection,
                            name,
                            expected_size,
                            destination,
                            number,
                            len(ordered_logs),
                        )
                    )
            self.ui_queue.put(
                ("transfer_done", tuple(str(path) for path in downloaded))
            )
        except Exception as exc:
            with self.serial_lock:
                self._close_serial_connection()
            self.ui_queue.put(("transfer_error", str(exc)))

    def choose_destination(self) -> None:
        selected = filedialog.askdirectory(initialdir=self.destination.get())
        if selected:
            self.destination.set(selected)

    def analyze_log(self) -> None:
        target = Path(self.source.get())
        if not target.is_file():
            messagebox.showerror("Log not found", "Download a CSV log first.")
            return
        self.analysis_button.configure(state="disabled")
        self.log("Analyzing chest-worn horizontal body motion...")
        threading.Thread(
            target=self._analyze_log_worker,
            args=(target,),
            daemon=True,
        ).start()

    def _analyze_log_worker(self, target: Path) -> None:
        script = Path(__file__).with_name("analyze_imu.py")
        result = subprocess.run(
            [sys.executable, str(script), str(target), "--body-motion"],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            self.ui_queue.put(("analysis_done", result.stdout.strip()))
        else:
            self.ui_queue.put(
                ("analysis_error", result.stderr.strip() or result.stdout.strip() or "Analyzer failed")
            )


if __name__ == "__main__":
    ImuGui().mainloop()
