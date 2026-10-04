"""Desktop application for configuration, diagnostics, and media retrieval."""

import asyncio
import datetime as dt
import hmac
import importlib
import os
import subprocess
import sys
import threading 
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, simpledialog, ttk

from auth import PasswordAuth
from protocol import (
    protocol_has_capability as _protocol_has_capability,
)
from media_utils import find_ffmpeg_executable as _find_ffmpeg_executable
from device_transport import BleTransport, SerialTransport
from command_service import CommandExecutor
from media_transfer_service import MediaTransferSession
from usb_mass_storage_service import (
    delete_all_via_otg_if_connected,
    eject_otg_session,
    retrieve_if_otg_connected,
)
from presentation_services import parse_summary_delete_result
from settings_service import (
    GuiSettings,
    ScheduleValidationError,
    SettingsStore,
    build_program_start,
    build_schedule_spec,
    schedule_preview,
)
from image_catalog_service import ImageCatalog, build_image_catalog
from views import (
    MainPageActions,
    MainPageVariables,
    SettingsPageActions,
    SettingsPageVariables,
    build_main_page,
    build_settings_page,
)
from device_actions import DeviceActionService
from device_queries import DeviceQueryService

PILImage = None
ImageOps = None
ImageTk = None
pil_import_error = None
try:
    from PIL import Image as PILImage
    from PIL import ImageOps, ImageTk
except Exception as exc:
    pil_import_error = f"Image preview unavailable: {exc}. Install it with: pip install Pillow"

serial = None
list_ports = None
serial_import_error = None

def _load_pyserial():
    """Load pyserial lazily so packaging errors can be shown inside the GUI."""
    global serial, list_ports, serial_import_error

    try:
        mod = importlib.import_module("serial")
        ports_mod = importlib.import_module("serial.tools.list_ports")
    except Exception as exc:
        serial_import_error = f"Failed to import pyserial: {exc}"
        return

    if not hasattr(mod, "Serial"):
        serial_path = getattr(mod, "__file__", "unknown location")
        serial_import_error = (
            "Imported 'serial' is not pyserial. "
            f"Loaded from: {serial_path}. "
            "Uninstall conflicting package and run: pip install pyserial"
        )
        return

    serial = mod
    list_ports = ports_mod
    serial_import_error = None

_load_pyserial()

BleakClient = None
BleakScanner = None
bleak_import_error = None

def _load_bleak():
    """Load Bleak lazily and retain a user-facing dependency error."""
    global BleakClient, BleakScanner, bleak_import_error

    try:
        bleak_mod = importlib.import_module("bleak")
    except Exception as exc:
        bleak_import_error = f"Bleak not available: {exc}. Install it with: pip install bleak"
        return

    BleakClient = getattr(bleak_mod, "BleakClient", None)
    BleakScanner = getattr(bleak_mod, "BleakScanner", None)
    if BleakClient is None or BleakScanner is None:
        bleak_import_error = "Installed bleak module does not expose BleakClient/BleakScanner."
        return

    bleak_import_error = None

_load_bleak()

DEFAULT_BAUD = 921600
SERIAL_RETRY_COUNT = 3
SERIAL_RESPONSE_WINDOW_MS = 700
HOSTED_BLE_TARGET_DEFAULT = "RaphasCam"
BLE_CONTROL_COMMAND_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"
BLE_CONTROL_RESPONSE_UUID = "0000fff2-0000-1000-8000-00805f9b34fb"
BLE_SCAN_TIMEOUT_S = 12.0
BLE_RESPONSE_WINDOW_S = 1.5
DASHBOARD_REFRESH_MS = 500
GUI_PASSWORD_ENV_NAME = "RTC_GUI_PASSWORD"
GUI_DEFAULT_PASSWORD = "ChangeMe123!"
GUI_PASSWORD_MAX_ATTEMPTS = 10
if getattr(sys, "frozen", False):
    APP_DIR = Path(sys.executable).resolve().parent
    RESOURCE_DIR = Path(getattr(sys, "_MEIPASS", APP_DIR))
else:
    APP_DIR = Path(__file__).resolve().parent
    RESOURCE_DIR = APP_DIR

GUI_PASSWORD_STORE_FILE = APP_DIR / "gui_password_store.json"
GUI_SETTINGS_STORE_FILE = APP_DIR / "gui_settings_store.json"
GUI_LOGO_FILE = RESOURCE_DIR / "Camera.png"
MEDIA_PROTOCOL_DOWNLOAD_DIR = Path.home() / "Pictures" / "SD_Card_Media"
LEGACY_VIDEO_DOWNLOAD_DIR = MEDIA_PROTOCOL_DOWNLOAD_DIR / "Legacy_Videos"
IMAGE_PROTOCOL_DOWNLOAD_DIR = MEDIA_PROTOCOL_DOWNLOAD_DIR / "Images"
BURST_IMAGE_DOWNLOAD_DIR = MEDIA_PROTOCOL_DOWNLOAD_DIR / "Burst Images"
SUMMARY_PROTOCOL_DOWNLOAD_DIR = MEDIA_PROTOCOL_DOWNLOAD_DIR / "Summaries"
MJPEG_TARGET_DURATION_SECONDS = 3.0


MJPEG_DEFAULT_CAPTURE_FPS = 5.0


def _user_log_text(text: str) -> str | None:
    """Remove transport chatter and shorten low-level Windows serial errors."""
    message = text.strip()
    if not message:
        return None

    hidden_prefixes = (
        "Protocol v",
        "BT protocol v",
        "Device uses legacy media transfer",
        "Using negotiated binary",
        "Using negotiated Base64",
        "SD card:",
        "Sending:",
        "BT sending:",
        "Failed to resume auto capture:",
        "RX: I (",
    )
    if message.startswith(hidden_prefixes):
        return None
    if message in ("RX: I", "RX: W", "RX: E"):
        return None
    if "i2c.common: GPIO 7 is not usable" in message:
        return None
    if "i2c.common: GPIO 8 is not usable" in message:
        return None
    if "unexpected serial line(s)" in message:
        return None

    serial_link_errors = (
        "WriteFile failed",
        "ClearCommError failed",
        "The device does not recognize the command",
    )
    if any(fragment in message for fragment in serial_link_errors):
        if message.startswith("Transfer failed:"):
            return "Transfer interrupted because the serial connection was lost; reconnect and retry."
        return None

    return message


class RTCSetterApp:
    """Main GUI application for communicating with ESP32-P4 firmware.

    Provides a graphical interface to:
    - Connect to the device via serial USB
    - Enable or deep-sleep the autonomous camera capture process
    - Set RTC (real-time clock) time
    - Retrieve images and other media from the SD card
    - Delete all SD-card media (password-protected)
    - Change GUI password

    Serial communication is handled in background threads to keep the GUI responsive.
    """

    def __init__(self, root: tk.Tk, auth: PasswordAuth) -> None:
        self.root = root
        self.auth = auth
        self.root.title("GUI")
        self.root.geometry("980x760")

        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value=str(DEFAULT_BAUD))
        self.transport_var = tk.StringVar(value="Serial")
        self.ble_target_var = tk.StringVar(value=HOSTED_BLE_TARGET_DEFAULT)
        self.schedule_enabled_var = tk.BooleanVar(value=False)
        self.schedule_start_var = tk.StringVar(value="08:00")
        self.schedule_stop_var = tk.StringVar(value="22:00")
        self.schedule_days_var = tk.StringVar(value="1")
        self.capture_interval_var = tk.StringVar(value="30")
        self.burst_capture_enabled_var = tk.BooleanVar(value=False)
        self._settings_store = SettingsStore(GUI_SETTINGS_STORE_FILE)
        self._load_gui_settings()
        self.status_var = tk.StringVar(value="Device: unknown")
        self.sd_status_var = tk.StringVar(value="SD: unknown")
        self.otg_status_var = tk.StringVar(value="OTG: waiting for firmware status...")
        self.schedule_preview_var = tk.StringVar(value="")
        self.media_transfer_status_var = tk.StringVar(value="SD media transfer: idle")
        self.media_transfer_progress_var = tk.DoubleVar(value=0.0)
        self.summary_files: list[str] = []
        self.image_files: list[Path] = []
        self._image_catalog = ImageCatalog((), 0, 0)
        self._image_preview_photo = None
        self._ble_address = None
        self._serial_transport = SerialTransport(serial, self._safe_log)
        self._ble_transport = BleTransport(
            BleakClient,
            BleakScanner,
            BLE_CONTROL_COMMAND_UUID,
            BLE_CONTROL_RESPONSE_UUID,
            BLE_SCAN_TIMEOUT_S,
            BLE_RESPONSE_WINDOW_S,
            self._safe_log,
        )
        self._command_executor = CommandExecutor(
            self._serial_transport,
            self._ble_transport,
            serial,
            SERIAL_RETRY_COUNT,
            SERIAL_RESPONSE_WINDOW_MS,
        )
        self._closing = False
        self._dashboard_refresh_in_progress = False
        self._dashboard_refresh_after_id = None
        self._foreground_command_active = False
        self._auto_time_sync_keys: set[str] = set()
        self._auto_time_sync_lock = threading.Lock()
        self._last_log_text: str | None = None
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        self._build_ui()
        for var in (
            self.schedule_enabled_var,
            self.schedule_start_var,
            self.schedule_stop_var,
            self.schedule_days_var,
            self.capture_interval_var,
            self.burst_capture_enabled_var,
        ):
            var.trace_add("write", lambda *_args: self._update_schedule_preview())
        self._update_schedule_preview()
        self.refresh_ports()
        self._schedule_dashboard_auto_refresh()

    def _on_close(self) -> None:
        """Stop background refresh and release transports before Tk exits."""
        self._closing = True
        if self._dashboard_refresh_after_id is not None:
            try:
                self.root.after_cancel(self._dashboard_refresh_after_id)
            except Exception:
                pass
            self._dashboard_refresh_after_id = None
        self._save_gui_settings()
        self._close_serial_connection()
        self.root.destroy()

    def _load_gui_settings(self) -> None:
        """Restore persisted UI settings while retaining safe defaults."""
        settings = self._settings_store.load()
        if settings is None:
            return
        self.baud_var.set(settings.baud)
        self.transport_var.set(settings.transport)
        self.ble_target_var.set(settings.bt_target)
        self.schedule_enabled_var.set(settings.schedule_enabled)
        self.schedule_start_var.set(settings.schedule_start)
        self.schedule_stop_var.set(settings.schedule_stop)
        self.schedule_days_var.set(settings.schedule_days)
        self.capture_interval_var.set(settings.capture_interval_s)
        self.burst_capture_enabled_var.set(settings.burst_capture_enabled)

    def _save_gui_settings(self) -> None:
        """Persist non-secret GUI preferences so the app reopens as left."""
        try:
            self._settings_store.save(
                GuiSettings(
                    baud=self.baud_var.get().strip(),
                    transport=self.transport_var.get(),
                    bt_target=self.ble_target_var.get().strip(),
                    schedule_enabled=bool(self.schedule_enabled_var.get()),
                    schedule_start=self.schedule_start_var.get().strip(),
                    schedule_stop=self.schedule_stop_var.get().strip(),
                    schedule_days=self.schedule_days_var.get().strip(),
                    capture_interval_s=self.capture_interval_var.get().strip(),
                    burst_capture_enabled=bool(
                        self.burst_capture_enabled_var.get()
                    ),
                )
            )
        except Exception as exc:
            self._safe_log(f"Warning: failed to save GUI settings: {exc}")

    def _close_serial_connection(self) -> None:
        self._serial_transport.close()

    def _build_ui(self) -> None:
        self.main_frame = ttk.Frame(self.root, padding=12)
        self.settings_frame = ttk.Frame(self.root, padding=12)

        main_widgets = build_main_page(
            self.main_frame,
            MainPageVariables(
                self.port_var,
                self.media_transfer_status_var,
                self.media_transfer_progress_var,
                self.status_var,
                self.sd_status_var,
                self.otg_status_var,
                self.schedule_preview_var,
            ),
            MainPageActions(
                self.show_settings_page,
                self.refresh_ports,
                self.start_program,
                self.stop_program,
                self.retrieve_sd_images,
                self.eject_otg,
                self.delete_all_sd_images,
                self.refresh_summary_list,
                self.open_selected_summary,
                self.delete_all_summaries,
                self.refresh_image_list,
                self.open_selected_image,
                self._on_image_selected,
            ),
        )
        self.port_combo = main_widgets.port_combo
        self.media_transfer_progress = main_widgets.media_transfer_progress
        self.summary_listbox = main_widgets.summary_listbox
        self.summary_text = main_widgets.summary_text
        self.image_listbox = main_widgets.image_listbox
        self.image_preview = main_widgets.image_preview
        self.log = main_widgets.log

        build_settings_page(
            self.settings_frame,
            SettingsPageVariables(
                self.baud_var,
                self.transport_var,
                self.ble_target_var,
                self.schedule_enabled_var,
                self.schedule_start_var,
                self.schedule_stop_var,
                self.schedule_days_var,
                self.capture_interval_var,
                self.burst_capture_enabled_var,
            ),
            SettingsPageActions(
                self.show_main_page,
                self.find_ble_device,
                self.apply_schedule_settings,
                self.change_password,
            ),
        )
        self.show_main_page()
        self.refresh_image_list()

        self._log("Tip: close idf.py monitor before sending commands from this GUI.")
        if serial_import_error:
            self._log(serial_import_error)
        if bleak_import_error:
            self._log(bleak_import_error)
        if pil_import_error:
            self._log(pil_import_error)


    def _update_schedule_preview(self) -> None:
        self.schedule_preview_var.set(
            schedule_preview(
                bool(self.schedule_enabled_var.get()),
                self.schedule_start_var.get(),
                self.schedule_stop_var.get(),
                self.schedule_days_var.get(),
                self.capture_interval_var.get(),
                bool(self.burst_capture_enabled_var.get()),
            )
        )

    def _set_text_widget(self, widget: tk.Text, text: str) -> None:
        widget.configure(state=tk.NORMAL)
        widget.delete("1.0", tk.END)
        widget.insert(tk.END, text)
        widget.configure(state=tk.DISABLED)

    def _request_command_lines(self, command: str, is_done, timeout_ms: int = 2500, log_send: bool = True) -> list[str]:
        if self.transport_var.get() in ("BT", "BLE"):
            target = self._ble_address or self.ble_target_var.get().strip() or HOSTED_BLE_TARGET_DEFAULT
            return asyncio.run(self._send_command_ble_async(target, command, log_send=log_send))

        target = self._get_serial_target()
        if target is None:
            return []
        port, baud = target
        return self._serial_request_until(port, baud, command, is_done=is_done, timeout_ms=timeout_ms, log_send=log_send)

    def _device_query_service(self) -> DeviceQueryService:
        return DeviceQueryService(
            request=lambda command, is_done, timeout_ms, log_send: (
                self._request_command_lines(
                    command,
                    is_done=is_done,
                    timeout_ms=timeout_ms,
                    log_send=log_send,
                )
            ),
            should_cancel=lambda: self._foreground_command_active,
        )

    def _schedule_dashboard_auto_refresh(self) -> None:
        if self._closing:
            return
        if self._dashboard_refresh_after_id is not None:
            return
        self._dashboard_refresh_after_id = self.root.after(DASHBOARD_REFRESH_MS, self._dashboard_auto_refresh_tick)

    def _dashboard_auto_refresh_tick(self) -> None:
        self._dashboard_refresh_after_id = None
        if self._closing:
            return
        if not self._foreground_command_active:
            self.refresh_dashboard(log_errors=False)
        self._schedule_dashboard_auto_refresh()

    def refresh_dashboard(self, log_errors: bool = True) -> None:
        if self._foreground_command_active:
            return
        if self._dashboard_refresh_in_progress:
            return
        self._dashboard_refresh_in_progress = True
        threading.Thread(target=self._refresh_dashboard_worker, args=(log_errors,), daemon=True).start()

    def _refresh_dashboard_worker(self, log_errors: bool) -> None:
        try:
            result = self._device_query_service().dashboard(log_errors)
            if result.cancelled:
                return
            if result.error:
                if log_errors:
                    self._safe_log(f"Status refresh failed: {result.error}")
                return
            self.root.after(0, lambda: self._apply_dashboard_view(result.view))
        finally:
            if not self._closing:
                self.root.after(0, self._mark_dashboard_refresh_done)

    def _mark_dashboard_refresh_done(self) -> None:
        self._dashboard_refresh_in_progress = False

    def _pause_dashboard_for_foreground_command(self) -> None:
        """Temporarily stop background polling while a button command is in flight."""
        self._foreground_command_active = True
        if self._dashboard_refresh_after_id is not None:
            try:
                self.root.after_cancel(self._dashboard_refresh_after_id)
            except Exception:
                pass
            self._dashboard_refresh_after_id = None

    def _resume_dashboard_after_foreground_command(self) -> None:
        """Resume dashboard polling after a foreground command finishes."""
        if self._closing:
            return
        self._foreground_command_active = False
        self._schedule_dashboard_auto_refresh()

    def _apply_dashboard_view(self, view) -> None:
        if view.device_text is not None:
            self.status_var.set(view.device_text)
        if view.sd_text is not None:
            self.sd_status_var.set(view.sd_text)
        if view.otg_text is not None:
            self.otg_status_var.set(view.otg_text)
        if not view.device_status_received:
            return
        if self.transport_var.get() == "Serial":
            self._auto_sync_pc_time_once(
                f"serial:{self.port_var.get().strip()}:{self.baud_var.get().strip()}"
            )
        else:
            bt_key = (
                self._ble_address
                or self.ble_target_var.get().strip()
                or HOSTED_BLE_TARGET_DEFAULT
            )
            self._auto_sync_pc_time_once(f"bt:{bt_key}", bt_address=self._ble_address)


    def _auto_sync_pc_time_once(self, device_key: str, bt_address: str | None = None) -> None:
        if not device_key:
            return

        with self._auto_time_sync_lock:
            if device_key in self._auto_time_sync_keys:
                return
            self._auto_time_sync_keys.add(device_key)

        payload = dt.datetime.now().replace(microsecond=0).strftime("%Y-%m-%d %H:%M:%S")
        threading.Thread(
            target=self._send_settime_worker,
            args=(payload, bt_address),
            daemon=True,
        ).start()

    def refresh_summary_list(self) -> None:
        threading.Thread(target=self._refresh_summary_list_worker, daemon=True).start()

    def _refresh_summary_list_worker(self) -> None:
        result = self._device_query_service().summary_list()
        if result.error:
            self._safe_log(f"Summary refresh failed: {result.error}")
            return
        self.root.after(0, lambda: self._apply_summary_view(result.view))

    def _apply_summary_view(self, view) -> None:
        for error in view.errors:
            self._log(error)
        self.summary_files = list(view.names)
        self.summary_listbox.delete(0, tk.END)
        for name in view.names:
            self.summary_listbox.insert(tk.END, name)
        self._log(view.status_text)


    def open_selected_summary(self) -> None:
        selection = self.summary_listbox.curselection()
        if not selection:
            messagebox.showinfo("Daily Summary", "Select a summary file first.", parent=self.root)
            return
        name = self.summary_listbox.get(selection[0])
        threading.Thread(target=self._open_summary_worker, args=(name,), daemon=True).start()

    def refresh_image_list(self) -> None:
        """Refresh normal and burst JPEGs already transferred to this PC."""
        self._image_catalog = build_image_catalog(
            IMAGE_PROTOCOL_DOWNLOAD_DIR,
            BURST_IMAGE_DOWNLOAD_DIR,
        )
        self.image_files = [entry.path for entry in self._image_catalog.entries]
        self.image_listbox.delete(0, tk.END)
        for entry in self._image_catalog.entries:
            self.image_listbox.insert(tk.END, entry.display_text)

        self._image_preview_photo = None
        self.image_preview.configure(
            image="",
            text=self._image_catalog.preview_status,
        )


    def _selected_image_path(self) -> Path | None:
        selection = self.image_listbox.curselection()
        if not selection:
            return None
        return self._image_catalog.path_at(int(selection[0]))

    def _on_image_selected(self, _event=None) -> None:
        path = self._selected_image_path()
        if path is None:
            return
        if PILImage is None or ImageTk is None or ImageOps is None:
            self.image_preview.configure(
                image="",
                text=f"{path.name}\n\nPreview requires Pillow.\nUse Open Selected to view the image.",
            )
            return

        try:
            with PILImage.open(path) as source:
                preview = ImageOps.exif_transpose(source).copy()
            preview.thumbnail((560, 340), PILImage.Resampling.LANCZOS)
            self._image_preview_photo = ImageTk.PhotoImage(preview)
            self.image_preview.configure(image=self._image_preview_photo, text="")
        except Exception as exc:
            self._image_preview_photo = None
            self.image_preview.configure(
                image="",
                text=f"Could not preview {path.name}\n{exc}",
            )

    def open_selected_image(self) -> None:
        path = self._selected_image_path()
        if path is None:
            messagebox.showinfo("Image Viewer", "Select an image first.", parent=self.root)
            return
        try:
            os.startfile(path)
        except Exception as exc:
            messagebox.showerror(
                "Image Viewer",
                f"Could not open {path.name}:\n{exc}",
                parent=self.root,
            )

    def _open_summary_worker(self, name: str) -> None:
        result = self._device_query_service().summary_text(name)
        if result.error:
            self._safe_log(f"Summary open failed: {result.error}")
            return
        self.root.after(0, lambda: self._apply_summary_content(name, result.text))

    def _apply_summary_content(self, name: str, text: str) -> None:
        self._set_text_widget(self.summary_text, text)
        self._log(f"Opened summary {name}.")


    def delete_all_summaries(self) -> None:
        if not self._prompt_verified_password(
            "Delete Summaries",
            "Enter your app password to allow deleting daily summary files:",
        ):
            return
        if not messagebox.askyesno(
            "Delete Summaries",
            "Delete all daily summary .txt files from the SD card?\n\nImages and encrypted logs will not be deleted.",
            parent=self.root,
        ):
            return
        threading.Thread(target=self._delete_all_summaries_worker, daemon=True).start()

    def _delete_all_summaries_worker(self) -> None:
        try:
            lines = self._device_action_service().delete_summaries()
            self.root.after(0, lambda: self._apply_summary_delete(lines))
        except Exception as exc:
            self._safe_log(f"Summary delete failed: {exc}")

    def _apply_summary_delete(self, lines: list[str]) -> None:
        result = parse_summary_delete_result(lines)
        if result:
            self._log(result)
        if result.startswith("OK SUMMARY_DELETE_ALL"):
            self.summary_files = []
            self.summary_listbox.delete(0, tk.END)
            self._set_text_widget(self.summary_text, "")
            self.refresh_dashboard()
        elif not result:
            self._log("Summary delete finished without a protocol response.")


    def show_main_page(self) -> None:
        self._save_gui_settings()
        self._update_schedule_preview()
        self.settings_frame.pack_forget()
        self.main_frame.pack(fill=tk.BOTH, expand=True)

    def show_settings_page(self) -> None:
        self.main_frame.pack_forget()
        self.settings_frame.pack(fill=tk.BOTH, expand=True)

    def _log(self, text: str) -> None:
        text = _user_log_text(text)
        if text is None or text == self._last_log_text:
            return
        self._last_log_text = text
        timestamp = dt.datetime.now().strftime("%H:%M:%S")
        self.log.insert(tk.END, f"[{timestamp}] {text}\n")
        self.log.see(tk.END)

    def refresh_ports(self) -> None:
        if list_ports is None:
            self.port_combo["values"] = []
            if serial_import_error:
                self._log(serial_import_error)
            else:
                self._log("pyserial not installed. Install with: pip install pyserial")
            return

        detected = list(list_ports.comports())
        # Intel AMT/SOL is a motherboard-management UART, not the camera. It is
        # commonly the only COM port left when the ESP32-P4 control cable is
        # unplugged, so selecting it produced a misleading Windows error 87.
        def is_camera_port(port_info) -> bool:
            description = (port_info.description or "").lower()
            hardware_id = (port_info.hwid or "").upper()
            if "active management technology" in description:
                return False
            if hardware_id.startswith("PCI\\VEN_8086"):
                return False
            return True

        camera_ports = [p for p in detected if is_camera_port(p)]
        ports = [p.device for p in camera_ports]
        self.port_combo["values"] = ports

        if self._serial_transport.port and self._serial_transport.port not in ports:
            self._close_serial_connection()

        if ports and self.port_var.get() not in ports:
            self.port_var.set(ports[0])

        if not ports:
            self.port_var.set("")
            if detected:
                ignored = ", ".join(f"{p.device} ({p.description})" for p in detected)
                self._log(
                    "No camera control port found. Connect the regular USB/serial cable. "
                    f"Ignored non-camera port: {ignored}"
                )
            else:
                self._log("No serial ports found.")
        else:
            self._log(f"Detected ports: {', '.join(ports)}")
            if self.status_var.get() == "Device: unknown":
                self.status_var.set(f"Device: {self.port_var.get().strip()} detected; waiting for firmware status...")
            if self.sd_status_var.get() == "SD: unknown":
                self.sd_status_var.set("SD: waiting for firmware status...")

    def _schedule_command_payload(self) -> tuple[str, str, int, int, int] | None:
        try:
            spec = build_schedule_spec(
                self.schedule_start_var.get(),
                self.schedule_stop_var.get(),
                self.schedule_days_var.get(),
                self.capture_interval_var.get(),
                bool(self.burst_capture_enabled_var.get()),
            )
        except ScheduleValidationError as exc:
            messagebox.showerror("Invalid schedule", str(exc))
            return None
        return (
            spec.start,
            spec.stop,
            spec.days,
            spec.interval_seconds,
            spec.burst_enabled,
        )

    def apply_schedule_settings(self) -> None:
        """Send daily program-window settings to the firmware."""
        self._save_gui_settings()
        if not self.schedule_enabled_var.get():
            self.send_command("DISABLE_SCHEDULE")
            return

        payload = self._schedule_command_payload()
        if payload is None:
            return

        start_text, stop_text, days, interval_s, burst_enabled = payload
        self.send_command(f"SET_SCHEDULE {start_text} {stop_text} {days} {interval_s} {burst_enabled}")

    def start_program(self) -> None:
        """Start the firmware's autonomous camera capture schedule."""
        schedule_enabled = bool(self.schedule_enabled_var.get())
        burst_enabled = bool(self.burst_capture_enabled_var.get())
        try:
            schedule = (
                build_schedule_spec(
                    self.schedule_start_var.get(),
                    self.schedule_stop_var.get(),
                    self.schedule_days_var.get(),
                    self.capture_interval_var.get(),
                    burst_enabled,
                )
                if schedule_enabled
                else None
            )
            request = build_program_start(
                schedule_enabled,
                self.capture_interval_var.get(),
                burst_enabled,
                dt.datetime.now(),
                schedule,
            )
        except ScheduleValidationError as exc:
            messagebox.showerror(
                "Invalid schedule" if schedule_enabled else "Invalid interval",
                str(exc),
            )
            return

        self._save_gui_settings()
        self.send_command(request.command, list(request.messages))


    def stop_program(self) -> None:
        """Stop autonomous captures while keeping the device awake on serial."""
        self.send_command("STOP_PROGRAM")

    def _device_action_service(
        self, port: str | None = None, baud: int | None = None
    ) -> DeviceActionService:
        def serial_request(command, is_done, timeout_ms, log_send):
            if port is not None and baud is not None:
                return self._serial_request_until(
                    port,
                    baud,
                    command,
                    is_done=is_done,
                    timeout_ms=timeout_ms,
                    log_send=log_send,
                )
            return self._request_command_lines(
                command,
                is_done=is_done,
                timeout_ms=timeout_ms,
                log_send=log_send,
            )

        def ble_request(target, command, log_send, response_window_s):
            return asyncio.run(
                self._send_command_ble_async(
                    target,
                    command,
                    log_send=log_send,
                    response_window_s=response_window_s,
                )
            )

        return DeviceActionService(serial_request, ble_request)

    def record_burst(self) -> None:
        """Ask the firmware to capture encrypted stills for three seconds."""
        self._pause_dashboard_for_foreground_command()
        if self.transport_var.get() in ("BT", "BLE"):
            if not self._ble_address:
                self._resume_dashboard_after_foreground_command()
                messagebox.showinfo("BT device needed", "Click Find BT first, then try recording again.", parent=self.root)
                return
            threading.Thread(target=self._record_burst_worker, args=(None, None, self._ble_address), daemon=True).start()
            return

        target = self._get_serial_target()
        if target is None:
            self._resume_dashboard_after_foreground_command()
            return
        port, baud = target
        threading.Thread(target=self._record_burst_worker, args=(port, baud, None), daemon=True).start()

    def _record_burst_worker(self, port: str | None, baud: int | None, bt_address: str | None) -> None:
        try:
            result = self._device_action_service(port, baud).capture_burst(bt_address)
            for message in result.messages:
                self._safe_log(message)
        finally:
            self._resume_dashboard_after_foreground_command()

    def _send_settime_worker(self, payload: str, bt_address: str | None = None) -> None:
        result = self._device_action_service().sync_clock(payload, bt_address)
        for message in result.messages:
            self._safe_log(message)

    def _get_serial_target(self) -> tuple[str, int] | None:
        if serial is None:
            details = serial_import_error or "Install pyserial: pip install pyserial"
            messagebox.showerror("Serial module issue", details)
            return None

        port = self.port_var.get().strip()
        if not port:
            self._log("No COM port selected. Connect the camera or choose a detected port.")
            self.status_var.set("Device: no COM port selected")
            return None

        try:
            baud = int(self.baud_var.get().strip())
        except ValueError:
            messagebox.showerror("Invalid baud", "Baud must be an integer.")
            return None

        return port, baud

    def _prompt_verified_password(self, title: str, prompt: str) -> bool:
        auth_password = simpledialog.askstring(
            title,
            prompt,
            show="*",
            parent=self.root,
        )
        if auth_password is None:
            return False
        if not self.auth.verify(auth_password):
            messagebox.showerror("Access denied", "Password is incorrect.", parent=self.root)
            return False
        return True

    def send_command(self, command: str, success_messages: list[str] | None = None) -> None:
        self._pause_dashboard_for_foreground_command()
        if self.transport_var.get() in ("BT", "BLE"):
            self.send_command_ble(command, success_messages)
            return

        target = self._get_serial_target()
        if target is None:
            self._resume_dashboard_after_foreground_command()
            return
        port, baud = target

        worker = threading.Thread(
            target=self._send_command_worker,
            args=(port, baud, command, success_messages),
            daemon=True,
        )
        worker.start()

    def find_ble_device(self) -> None:
        if bleak_import_error:
            self._log(bleak_import_error)
            messagebox.showerror("BT module issue", bleak_import_error, parent=self.root)
            return

        target = self.ble_target_var.get().strip() or HOSTED_BLE_TARGET_DEFAULT
        self._log(f"BT: scanning for '{target}'...")
        threading.Thread(target=self._find_ble_worker, args=(target,), daemon=True).start()

    def _find_ble_worker(self, target: str) -> None:
        try:
            device = asyncio.run(self._find_ble_device_async(target))
            if device is None:
                self._safe_log(f"BT: no device found matching '{target}'")
                return
            self._ble_address = device.address
            name = device.name or "(no name)"
            self._safe_log(f"BT: found {name} at {device.address}")
            self._auto_sync_pc_time_once(f"bt:{device.address}", bt_address=device.address)
        except Exception as exc:
            self._safe_log(f"BT scan error: {exc}")

    async def _find_ble_device_async(self, target: str):
        return await self._ble_transport.discover(target)

    def send_command_ble(self, command: str, success_messages: list[str] | None = None) -> None:
        if bleak_import_error:
            self._log(bleak_import_error)
            messagebox.showerror("BT module issue", bleak_import_error, parent=self.root)
            self._resume_dashboard_after_foreground_command()
            return

        target = self._ble_address or self.ble_target_var.get().strip() or HOSTED_BLE_TARGET_DEFAULT
        threading.Thread(
            target=self._send_command_ble_worker,
            args=(target, command, success_messages),
            daemon=True,
        ).start()

    def _send_command_ble_worker(
        self,
        target: str,
        command: str,
        success_messages: list[str] | None = None,
    ) -> None:
        try:
            result = self._command_executor.execute_ble(target, command)
            if result.error:
                self._safe_log(result.error)
            elif success_messages and any(line.startswith("OK ") for line in result.lines):
                for message in success_messages:
                    self._safe_log(message)
            elif result.lines:
                for line in result.lines:
                    self._safe_log(f"BT RX: {line}")
            else:
                self._safe_log("BT: no response received")
        finally:
            if not self._closing:
                self.root.after(0, self._resume_dashboard_after_foreground_command)

    async def _send_command_ble_async(
        self,
        target: str,
        command: str,
        log_send: bool = True,
        response_window_s: float = BLE_RESPONSE_WINDOW_S,
    ) -> list[str]:
        return await self._ble_transport.request(
            target,
            command,
            log_send=log_send,
            response_window_s=response_window_s,
        )

    def _send_command_worker(
        self,
        port: str,
        baud: int,
        command: str,
        success_messages: list[str] | None = None,
    ) -> None:
        try:
            if not success_messages:
                self._safe_log(f"Sending: {command}")
            result = self._command_executor.execute_serial(port, baud, command)
            if result.error:
                self._safe_log(result.error)
            elif success_messages and any(line.startswith("OK ") for line in result.lines):
                for message in success_messages:
                    self._safe_log(message)
            elif result.lines:
                for line in result.lines:
                    self._safe_log(f"RX: {line}")
            else:
                self._safe_log(
                    "No immediate response. If device is busy printing, this can still be normal."
                )
            if result.connection_closed:
                self._safe_log("Serial connection closed after deep-sleep command.")
            if result.refresh_ports:
                self.root.after(0, self.refresh_ports)
        finally:
            if not self._closing:
                self.root.after(0, self._resume_dashboard_after_foreground_command)

    def _safe_log(self, text: str) -> None:
        self.root.after(0, lambda: self._log(text))

    def change_password(self) -> None:
        """Validate confirmation and delegate atomic storage to PasswordAuth."""
        if not self.auth.can_change():
            messagebox.showerror("Password Change Disabled", self.auth.cannot_change_reason(), parent=self.root)
            return

        old_password = simpledialog.askstring(
            "Change Password",
            "Enter current password:",
            show="*",
            parent=self.root,
        )
        if old_password is None:
            return

        new_password = simpledialog.askstring(
            "Change Password",
            "Enter new password:",
            show="*",
            parent=self.root,
        )
        if new_password is None:
            return

        confirm_password = simpledialog.askstring(
            "Change Password",
            "Re-enter new password:",
            show="*",
            parent=self.root,
        )
        if confirm_password is None:
            return

        if not hmac.compare_digest(new_password, confirm_password):
            messagebox.showerror("Mismatch", "New passwords do not match.", parent=self.root)
            return

        ok, message = self.auth.change_password(old_password, new_password)
        if not ok:
            messagebox.showerror("Password Change Failed", message, parent=self.root)
            return

        messagebox.showinfo("Password Updated", message, parent=self.root)
        self._log("Password was changed successfully.")

    def retrieve_sd_images(self) -> None:
        """Retrieve SD media through the serial line protocol."""
        target = self._get_serial_target()
        if target is None:
            return
        port, baud = target

        if not self._prompt_verified_password(
            "Media Access",
            "Enter your app password to authorize SD media retrieval:",
        ):
            return

        self._pause_dashboard_for_foreground_command()
        worker = threading.Thread(
            target=self._retrieve_sd_images_worker,
            args=(port, baud),
            daemon=True,
        )
        worker.start()

    def eject_otg(self) -> None:
        """Explicitly end Mass Storage mode and return SD ownership to the camera."""
        target = self._get_serial_target()
        if target is None:
            return
        port, baud = target
        self._pause_dashboard_for_foreground_command()
        threading.Thread(
            target=self._eject_otg_worker,
            args=(port, baud),
            daemon=True,
        ).start()

    def _eject_otg_worker(self, port: str, baud: int) -> None:
        try:
            eject_otg_session(
                request=lambda command, is_done, timeout_ms: self._serial_request_until(
                    port, baud, command, is_done=is_done,
                    timeout_ms=timeout_ms, log_send=False
                ),
                log=self._safe_log,
                progress=self._queue_media_transfer_progress,
            )
        except Exception as exc:
            self._safe_log(f"OTG eject failed: {exc}")
            self._queue_media_transfer_progress(0, 1, "OTG eject failed")
        finally:
            if not self._closing:
                self.root.after(0, self._resume_dashboard_after_foreground_command)

    def _set_media_transfer_progress(self, completed: int, total: int, status: str) -> None:
        """Update aggregate transfer progress from the Tk main thread."""
        percent = 0.0 if total <= 0 else min(100.0, (completed * 100.0) / total)
        self.media_transfer_progress_var.set(percent)
        self.media_transfer_status_var.set(status)

    def _queue_media_transfer_progress(self, completed: int, total: int, status: str) -> None:
        if not self._closing:
            self.root.after(
                0,
                lambda c=completed, t=total, s=status: self._set_media_transfer_progress(c, t, s),
            )

    def delete_all_sd_images(self) -> None:
        """Password-gated command to delete all media files stored on device SD."""
        target = self._get_serial_target()
        if target is None:
            return
        port, baud = target

        if not self._prompt_verified_password(
            "Delete All SD Media",
            "Enter your app password to allow deleting all device SD media:",
        ):
            return

        confirmed = messagebox.askyesno(
            "Confirm Delete",
            "Are you sure you want to delete all Media? "
            "Can't be undone.",
            parent=self.root,
        )
        if not confirmed:
            return

        self._pause_dashboard_for_foreground_command()
        self._set_media_transfer_progress(0, 1, "Deleting all SD media; please wait...")
        worker = threading.Thread(
            target=self._delete_all_sd_images_worker,
            args=(port, baud),
            daemon=True,
        )
        worker.start()

    def _serial_request_until(
        self,
        port: str,
        baud: int,
        command: str,
        is_done,
        timeout_ms: int,
        log_send: bool = True,
    ) -> list[str]:
        return self._serial_transport.request_until(
            port,
            baud,
            command,
            is_done,
            timeout_ms,
            log_send,
        )

    def _serial_media_stream_request(
        self, port: str, baud: int, command: str, is_done, timeout_ms: int, on_line
    ) -> None:
        """Run one streamed request and drop a desynchronized serial session."""
        try:
            if command.startswith("IMG_GETBIN "):
                self._serial_transport.request_until_binary_stream(
                    port, baud, command, timeout_ms, on_line, log_send=False
                )
            else:
                self._serial_transport.request_until_stream(
                    port, baud, command, is_done, timeout_ms, on_line, log_send=False
                )
        except Exception:
            self._serial_transport.close()
            raise

    def _convert_mjpeg_to_mp4_if_possible(self, mjpeg_path: Path, frame_count: int | None = None) -> None:
        """Convert raw MJPEG stream to a normal MP4 when ffmpeg is available."""
        ffmpeg = _find_ffmpeg_executable()
        output_path = mjpeg_path.with_suffix(".mp4")
        playback_fps = MJPEG_DEFAULT_CAPTURE_FPS
        if frame_count and frame_count > 0:
            playback_fps = max(0.5, frame_count / MJPEG_TARGET_DURATION_SECONDS)
        playback_fps_text = f"{playback_fps:.3f}".rstrip("0").rstrip(".")
        if not ffmpeg:
            self._safe_log(
                "ffmpeg not found, so the raw MJPEG was saved without MP4 conversion. "
                f"Install ffmpeg, then run: ffmpeg -framerate {playback_fps_text} -f mjpeg "
                f"-i \"{mjpeg_path}\" -t 3 -c:v libx264 -pix_fmt yuv420p \"{output_path}\""
            )
            return

        try:
            result = subprocess.run(
                [
                    ffmpeg,
                    "-y",
                    "-framerate",
                    playback_fps_text,
                    "-f",
                    "mjpeg",
                    "-i",
                    str(mjpeg_path),
                    "-t",
                    "3",
                    "-c:v",
                    "libx264",
                    "-pix_fmt",
                    "yuv420p",
                    "-movflags",
                    "+faststart",
                    str(output_path),
                ],
                capture_output=True,
                text=True,
                timeout=180,
                check=False,
            )
        except Exception as exc:
            self._safe_log(f"MP4 conversion failed for {mjpeg_path.name}: {exc}")
            return

        if result.returncode == 0 and output_path.exists():
            try:
                mjpeg_path.unlink()
                self._safe_log(
                    f"Converted {mjpeg_path.name} to playable {MJPEG_TARGET_DURATION_SECONDS:.0f}s MP4 "
                    f"at {playback_fps_text} fps: {output_path.name}; removed raw MJPEG."
                )
            except OSError as exc:
                self._safe_log(
                    f"Converted {mjpeg_path.name} to playable MP4: {output_path.name}; "
                    f"could not remove raw MJPEG: {exc}"
                )
        else:
            details = (result.stderr or result.stdout or "").strip().splitlines()
            tail = details[-1] if details else f"ffmpeg exit code {result.returncode}"
            self._safe_log(f"MP4 conversion failed for {mjpeg_path.name}: {tail}")

    def _retrieve_sd_images_worker(self, port: str, baud: int) -> None:
        try:
            usb_result = retrieve_if_otg_connected(
                request=lambda command, is_done, timeout_ms: self._serial_request_until(
                    port, baud, command, is_done=is_done,
                    timeout_ms=timeout_ms, log_send=False
                ),
                media_root=MEDIA_PROTOCOL_DOWNLOAD_DIR,
                log=self._safe_log,
                progress=self._queue_media_transfer_progress,
            )
            if usb_result.used:
                total = usb_result.copied + usb_result.skipped
                self._queue_media_transfer_progress(total, total or 1,
                    f"USB transfer complete: {usb_result.copied} copied, "
                    f"{usb_result.skipped} already present; OTG auto-ejected")
                if not self._closing:
                    self.root.after(0, self.refresh_image_list)
                    self.root.after(0, self._resume_dashboard_after_foreground_command)
                return
        except Exception as exc:
            self._safe_log(
                f"USB Mass Storage transfer interrupted: {exc}"
            )
            self._queue_media_transfer_progress(
                0, 1, "USB transfer interrupted; retry or click Eject OTG"
            )
            if not self._closing:
                self.root.after(0, self._resume_dashboard_after_foreground_command)
            return

        session = MediaTransferSession(
            request=lambda command, is_done, timeout_ms: self._serial_request_until(
                port, baud, command, is_done=is_done, timeout_ms=timeout_ms, log_send=False
            ),
            log=self._safe_log,
            progress=self._queue_media_transfer_progress,
            use_base64=lambda: _protocol_has_capability(
                self._serial_transport.protocol, "base64_v2"
            ),
            image_directory=IMAGE_PROTOCOL_DOWNLOAD_DIR,
            burst_directory=BURST_IMAGE_DOWNLOAD_DIR,
            summary_directory=SUMMARY_PROTOCOL_DOWNLOAD_DIR,
            legacy_directory=LEGACY_VIDEO_DOWNLOAD_DIR,
            stream_request=lambda command, is_done, timeout_ms, on_line: (
                self._serial_media_stream_request(
                    port, baud, command, is_done, timeout_ms, on_line
                )
            ),
            require_hash=lambda: _protocol_has_capability(
                self._serial_transport.protocol, "media_sha256"
            ),
            allow_resume=lambda: _protocol_has_capability(
                self._serial_transport.protocol, "media_resume"
            ),
            use_binary=lambda: _protocol_has_capability(
                self._serial_transport.protocol, "binary_media_v3"
            ),
        )
        try:
            result = session.run()
            for video_path, frame_count in result.videos_to_convert:
                self._convert_mjpeg_to_mp4_if_possible(video_path, frame_count)

            status = result.status_text
            if result.error:
                self._queue_media_transfer_progress(0, 1, status)
            elif result.rebooted or result.failed:
                self._queue_media_transfer_progress(
                    result.completed, result.total or 1, status
                )
            else:
                complete_total = result.total or 1
                self._queue_media_transfer_progress(
                    complete_total, complete_total, status
                )
            self._safe_log(
                status + (
                    ". Check the errors above and retry." if result.failed else "."
                )
            )
        finally:
            if not self._closing:
                self.root.after(0, self.refresh_image_list)
                self.root.after(0, self._resume_dashboard_after_foreground_command)
    def _delete_all_sd_images_worker(self, port: str, baud: int) -> None:
        try:
            otg_result = delete_all_via_otg_if_connected(
                request=lambda command, is_done, timeout_ms: self._serial_request_until(
                    port, baud, command, is_done=is_done,
                    timeout_ms=timeout_ms, log_send=False
                ),
                log=self._safe_log,
                progress=self._queue_media_transfer_progress,
            )
            if otg_result.used:
                if otg_result.failed:
                    self._queue_media_transfer_progress(
                        otg_result.deleted,
                        otg_result.deleted + otg_result.failed,
                        f"OTG deletion incomplete: {otg_result.failed} failed",
                    )
                else:
                    self._queue_media_transfer_progress(
                        otg_result.deleted,
                        otg_result.deleted or 1,
                        f"All SD media deleted over OTG ({otg_result.deleted} files)",
                    )
                return

            result = self._device_action_service(port, baud).delete_all_media()
            for message in result.messages:
                self._safe_log(message)
            if result.success:
                self._queue_media_transfer_progress(1, 1, "All SD media deleted")
            else:
                self._queue_media_transfer_progress(0, 1, "SD media deletion failed; check log")
        finally:
            if not self._closing:
                self.root.after(0, self._resume_dashboard_after_foreground_command)



def main() -> None:
    """Application entry point.

    Startup sequence:
    1. Create hidden Tkinter root window
    2. Initialize PasswordAuth (loads stored password or uses default)
    3. Prompt for password (up to GUI_PASSWORD_MAX_ATTEMPTS = 3 tries)
    4. On success: show main window and start RTCSetterApp
    5. On failure: exit after too many attempts

    The main window is initially hidden during authentication to prevent
    users from interacting while the password dialog is open.
    """
    root = tk.Tk()
    root.withdraw()

    # Keep a reference for Tk's lifetime; otherwise the window icon can vanish.
    try:
        root._app_icon = tk.PhotoImage(file=str(GUI_LOGO_FILE))
        root.iconphoto(True, root._app_icon)
    except (tk.TclError, OSError):
        root._app_icon = None

    auth = PasswordAuth(
        env_password=os.getenv(GUI_PASSWORD_ENV_NAME),
        default_password=GUI_DEFAULT_PASSWORD,
        store_path=GUI_PASSWORD_STORE_FILE,
    )
    authenticated = False

    for _ in range(GUI_PASSWORD_MAX_ATTEMPTS):
        entered_password = simpledialog.askstring(
            "Authentication Required",
            "Enter GUI password:",
            show="*",
            parent=root,
        )

        if entered_password is None:
            root.destroy()
            return

        if auth.verify(entered_password):
            authenticated = True
            break

        messagebox.showerror("Access denied", "Incorrect password.", parent=root)

    if not authenticated:
        messagebox.showerror("Access denied", "Too many failed attempts.", parent=root)
        root.destroy()
        return

    root.deiconify()
    app = RTCSetterApp(root, auth)
    root.mainloop()


if __name__ == "__main__":
    main()
