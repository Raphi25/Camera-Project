"""Native PySide6 visual prototype for the Raphas camera application.

All values and actions are simulated. No device or filesystem operations are
performed by this prototype.
"""

from __future__ import annotations

import datetime as dt
import os
import sys
from pathlib import Path
from typing import Callable

from PySide6.QtCore import QObject, QRunnable, Qt, QSize, QThreadPool, QTimer, Signal, Slot
from PySide6.QtGui import QFont, QPixmap
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSpinBox,
    QStackedWidget,
    QTabWidget,
    QTextEdit,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

try:
    import serial
    from serial.tools import list_ports
except Exception:
    serial = None
    list_ports = None

from command_service import CommandExecutor
from auth import PasswordAuth
from device_actions import DeviceActionService
from device_queries import DeviceQueryService
from device_transport import SerialTransport
from image_catalog_service import ImageCatalogEntry, build_image_catalog
from media_transfer_service import MediaTransferSession
from protocol import protocol_has_capability
from settings_service import (
    GuiSettings,
    ScheduleValidationError,
    SettingsStore,
    build_program_start,
    build_schedule_spec,
    schedule_preview,
)


BLUE = "#2563eb"
GREEN = "#16a34a"
RED = "#dc2626"
PURPLE = "#7c3aed"
DEFAULT_BAUD = 921600
SETTINGS_FILE = Path(__file__).with_name("gui_settings_store.json")
PASSWORD_STORE_FILE = Path(__file__).with_name("gui_password_store.json")
PASSWORD_ENV_NAME = "RTC_GUI_PASSWORD"
DEFAULT_PASSWORD = "ChangeMe123!"
PASSWORD_MAX_ATTEMPTS = 3
MEDIA_ROOT = Path.home() / "Pictures" / "SD_Card_Media"
IMAGE_DIRECTORY = MEDIA_ROOT / "Images"
BURST_DIRECTORY = MEDIA_ROOT / "Burst Images"
SUMMARY_DIRECTORY = MEDIA_ROOT / "Summaries"
LEGACY_DIRECTORY = MEDIA_ROOT / "Legacy_Videos"


LIGHT_STYLE = """
QWidget { color:#172033; font-family:'Segoe UI'; font-size:14px; }
QMainWindow, QWidget#page { background:#f4f7fb; }
QFrame#topbar, QFrame[card="true"] { background:white; border:1px solid #dfe5ee; border-radius:14px; }
QFrame#topbar { border-radius:0; border-width:0 0 1px 0; }
QLabel[muted="true"] { color:#697386; }
QLabel[eyebrow="true"] { color:#697386; font-size:11px; font-weight:700; }
QLabel[metric="true"] { font-size:16px; font-weight:700; }
QPushButton { min-height:38px; padding:0 16px; border-radius:9px; border:1px solid #ccd5e2; background:white; font-weight:600; }
QPushButton:hover { background:#f3f6fa; border-color:#aebacd; }
QPushButton[primary="true"] { color:white; background:#2563eb; border-color:#2563eb; }
QPushButton[primary="true"]:hover { background:#1d4ed8; }
QPushButton[danger="true"] { color:#dc2626; border-color:#f1a5a5; }
QPushButton:disabled { color:#9ca3af; background:#eef1f5; border-color:#dfe3e8; }
QToolButton { border:0; border-radius:18px; padding:7px; font-size:18px; }
QToolButton:hover { background:#edf1f7; }
QTabWidget::pane { border:0; background:transparent; }
QTabBar::tab { padding:11px 18px; margin-right:4px; color:#697386; font-weight:600; }
QTabBar::tab:selected { color:#2563eb; border-bottom:3px solid #2563eb; }
QLineEdit, QComboBox, QSpinBox { min-height:38px; border:1px solid #ccd5e2; border-radius:8px; padding:0 10px; background:white; }
QListWidget, QTextEdit { border:1px solid #dfe5ee; border-radius:10px; background:white; padding:6px; }
QProgressBar { border:0; border-radius:4px; background:#e5eaf1; height:8px; text-align:center; }
QProgressBar::chunk { border-radius:4px; background:#2563eb; }
QScrollArea { border:0; background:transparent; }
"""

DARK_STYLE = LIGHT_STYLE + """
QWidget { color:#e5e7eb; }
QMainWindow, QWidget#page { background:#0b1220; }
QFrame#topbar, QFrame[card="true"] { background:#111a2c; border-color:#263249; }
QLabel[muted="true"], QLabel[eyebrow="true"] { color:#94a3b8; }
QPushButton { color:#e5e7eb; background:#172238; border-color:#35435c; }
QPushButton:hover, QToolButton:hover { background:#22304a; }
QPushButton:disabled { color:#64748b; background:#121b2c; border-color:#27334a; }
QLineEdit, QComboBox, QSpinBox, QListWidget, QTextEdit { color:#e5e7eb; background:#10192a; border-color:#35435c; }
QTabBar::tab { color:#94a3b8; }
QTabBar::tab:selected { color:#60a5fa; border-bottom-color:#60a5fa; }
QProgressBar { background:#263249; }
"""


def label(text: str, *, muted: bool = False, size: int | None = None, bold: bool = False) -> QLabel:
    widget = QLabel(text)
    widget.setProperty("muted", muted)
    font = widget.font()
    if size:
        font.setPointSize(size)
    font.setBold(bold)
    widget.setFont(font)
    return widget


def button(text: str, *, primary: bool = False, danger: bool = False) -> QPushButton:
    widget = QPushButton(text)
    widget.setProperty("primary", primary)
    widget.setProperty("danger", danger)
    widget.setCursor(Qt.CursorShape.PointingHandCursor)
    return widget


def card() -> tuple[QFrame, QVBoxLayout]:
    frame = QFrame()
    frame.setProperty("card", True)
    layout = QVBoxLayout(frame)
    layout.setContentsMargins(20, 18, 20, 18)
    layout.setSpacing(10)
    return frame, layout


class WorkerSignals(QObject):
    result = Signal(object)
    error = Signal(str)
    finished = Signal()


class Worker(QRunnable):
    def __init__(self, operation: Callable[[], object]) -> None:
        super().__init__()
        self.operation = operation
        self.signals = WorkerSignals()

    @Slot()
    def run(self) -> None:
        try:
            self.signals.result.emit(self.operation())
        except Exception as exc:
            self.signals.error.emit(str(exc))
        finally:
            self.signals.finished.emit()


class DeviceBackend(QObject):
    """Qt-facing adapter around the GUI-independent serial services."""

    log = Signal(str)
    dashboard = Signal(object)
    command_done = Signal(object)
    busy_changed = Signal(bool)
    transfer_progress = Signal(int, int, str)
    transfer_done = Signal(object)
    delete_done = Signal(object)

    def __init__(self) -> None:
        super().__init__()
        self.pool = QThreadPool.globalInstance()
        self.workers: set[Worker] = set()
        self.transport = SerialTransport(serial, self.log.emit)
        self.executor = CommandExecutor(self.transport, None, serial, 3, 700)
        self.port = ""
        self.baud = DEFAULT_BAUD
        self.busy = False
        self.polling = False

    def configure(self, port: str, baud: int) -> None:
        if port != self.port or baud != self.baud:
            self.transport.close()
        self.port = port
        self.baud = baud

    def close(self) -> None:
        self.transport.close()

    def _request(self, command, is_done, timeout_ms, log_send=True):
        if serial is None:
            raise RuntimeError("pyserial is unavailable")
        if not self.port:
            raise RuntimeError("Select a COM port first")
        return self.transport.request_until(
            self.port, self.baud, command, is_done, timeout_ms, log_send
        )

    def _start_worker(self, worker: Worker) -> None:
        """Keep Qt worker signals alive until queued callbacks are delivered."""
        self.workers.add(worker)
        worker.signals.finished.connect(lambda worker=worker: self.workers.discard(worker))
        self.pool.start(worker)

    def refresh_dashboard(self) -> None:
        if self.busy or self.polling or not self.port:
            return
        self.polling = True
        worker = Worker(lambda: DeviceQueryService(self._request).dashboard(False))
        worker.signals.result.connect(self.dashboard.emit)
        worker.signals.error.connect(lambda error: self.log.emit(f"Status refresh failed: {error}"))
        worker.signals.finished.connect(self._poll_finished)
        self._start_worker(worker)

    def _poll_finished(self) -> None:
        self.polling = False

    def send(self, command: str) -> None:
        if self.busy:
            return
        self.busy = True
        self.busy_changed.emit(True)
        worker = Worker(lambda: self.executor.execute_serial(self.port, self.baud, command))
        worker.signals.result.connect(self.command_done.emit)
        worker.signals.error.connect(lambda error: self.log.emit(f"Command failed: {error}"))
        worker.signals.finished.connect(self._command_finished)
        self._start_worker(worker)

    def _command_finished(self) -> None:
        self.busy = False
        self.busy_changed.emit(False)

    def retrieve_media(self) -> None:
        if self.busy or not self.port:
            if not self.port:
                self.log.emit("Select a COM port before retrieving media.")
            return
        self.busy = True
        self.busy_changed.emit(True)

        def transfer():
            session = MediaTransferSession(
                request=lambda command, is_done, timeout_ms: self._request(
                    command, is_done, timeout_ms, False
                ),
                log=self.log.emit,
                progress=self.transfer_progress.emit,
                use_base64=lambda: protocol_has_capability(
                    self.transport.protocol, "base64_v2"
                ),
                image_directory=IMAGE_DIRECTORY,
                burst_directory=BURST_DIRECTORY,
                summary_directory=SUMMARY_DIRECTORY,
                legacy_directory=LEGACY_DIRECTORY,
                stream_request=lambda command, is_done, timeout_ms, on_line: (
                    self.transport.request_until_binary_stream(
                        self.port, self.baud, command, timeout_ms, on_line,
                        log_send=False,
                    )
                    if command.startswith("IMG_GETBIN ")
                    else self.transport.request_until_stream(
                        self.port, self.baud, command, is_done, timeout_ms,
                        on_line, log_send=False,
                    )
                ),
                require_hash=lambda: protocol_has_capability(
                    self.transport.protocol, "media_sha256"
                ),
                allow_resume=lambda: protocol_has_capability(
                    self.transport.protocol, "media_resume"
                ),
                use_binary=lambda: protocol_has_capability(
                    self.transport.protocol, "binary_media_v3"
                ),
            )
            return session.run()

        worker = Worker(transfer)
        worker.signals.result.connect(self.transfer_done.emit)
        worker.signals.error.connect(lambda error: self.log.emit(f"Media transfer failed: {error}"))
        worker.signals.finished.connect(self._command_finished)
        self._start_worker(worker)

    def delete_media(self) -> None:
        if self.busy or not self.port:
            if not self.port:
                self.log.emit("Select a COM port before deleting media.")
            return
        self.busy = True
        self.busy_changed.emit(True)
        service = DeviceActionService(serial_request=self._request)
        worker = Worker(service.delete_all_media)
        worker.signals.result.connect(self.delete_done.emit)
        worker.signals.error.connect(lambda error: self.log.emit(f"Delete-all failed: {error}"))
        worker.signals.finished.connect(self._command_finished)
        self._start_worker(worker)


class StatusCard(QFrame):
    def __init__(self, title: str, value: str, detail: str, symbol: str, color: str) -> None:
        super().__init__()
        self.setProperty("card", True)
        self.setMinimumHeight(132)
        outer = QHBoxLayout(self)
        outer.setContentsMargins(20, 17, 20, 17)
        text = QVBoxLayout()
        title_label = label(title.upper())
        title_label.setProperty("eyebrow", True)
        self.value_label = label(value)
        self.value_label.setProperty("metric", True)
        self.value_label.setWordWrap(True)
        self.value_label.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        self.detail_label = label(detail, muted=True)
        self.detail_label.setWordWrap(True)
        text.addWidget(title_label)
        text.addWidget(self.value_label)
        text.addWidget(self.detail_label)
        outer.addLayout(text, 1)
        icon = QLabel(symbol)
        icon.setAlignment(Qt.AlignmentFlag.AlignCenter)
        icon.setFixedSize(46, 46)
        icon.setStyleSheet(f"background:{color}22;color:{color};border-radius:12px;font-size:22px;font-weight:700")
        outer.addWidget(icon)

    def set_content(self, value: str, detail: str) -> None:
        self.value_label.setText(value)
        self.detail_label.setText(detail)


class MediaCard(QFrame):
    def __init__(self, entry: ImageCatalogEntry, open_preview) -> None:
        super().__init__()
        self.setProperty("card", True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.entry = entry
        self.open_preview = open_preview
        self.setMinimumWidth(0)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 14)
        layout.setSpacing(4)
        image = QLabel()
        pixmap = QPixmap(str(entry.path))
        image.setPixmap(pixmap.scaled(480, 170, Qt.AspectRatioMode.KeepAspectRatioByExpanding, Qt.TransformationMode.SmoothTransformation))
        image.setFixedHeight(170)
        image.setMinimumWidth(0)
        image.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
        image.setScaledContents(True)
        layout.addWidget(image)
        title = label(entry.path.name, bold=True)
        title.setContentsMargins(14, 8, 14, 0)
        try:
            modified = dt.datetime.fromtimestamp(entry.path.stat().st_mtime).strftime("%Y-%m-%d %H:%M")
        except OSError:
            modified = "Downloaded image"
        captured = label(f"{entry.category} · {modified}", muted=True)
        captured.setContentsMargins(14, 0, 14, 0)
        layout.addWidget(title)
        layout.addWidget(captured)

    def sizeHint(self) -> QSize:
        return QSize(240, 230)

    def minimumSizeHint(self) -> QSize:
        return QSize(160, 220)

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.open_preview(self.entry)
        super().mousePressEvent(event)


class SettingsDialog(QDialog):
    def __init__(self, settings: GuiSettings, ports: list[str], parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Camera GUI settings")
        self.resize(600, 480)
        outer = QVBoxLayout(self)
        heading = label("Settings", size=18, bold=True)
        outer.addWidget(heading)
        tabs = QTabWidget()
        outer.addWidget(tabs, 1)

        connection = QWidget()
        form = QFormLayout(connection)
        self.transport = QComboBox(); self.transport.addItems(["Serial", "BT"]); self.transport.setCurrentText(settings.transport)
        self.port = QComboBox(); self.port.addItems(ports)
        self.baud = QComboBox(); self.baud.setEditable(True); self.baud.addItems(["921600", "460800", "115200"]); self.baud.setCurrentText(settings.baud)
        self.bt_target = QLineEdit(settings.bt_target)
        form.addRow("Transport", self.transport)
        form.addRow("Serial port", self.port)
        form.addRow("Baud rate", self.baud)
        form.addRow("BT name/address", self.bt_target)
        tabs.addTab(connection, "Connection")

        schedule = QWidget()
        schedule_form = QFormLayout(schedule)
        self.schedule_enabled = QCheckBox("Use daily run window"); self.schedule_enabled.setChecked(settings.schedule_enabled)
        self.interval = QSpinBox(); self.interval.setRange(5, 3600); self.interval.setValue(int(settings.capture_interval_s)); self.interval.setSuffix(" seconds")
        self.schedule_start = QLineEdit(settings.schedule_start)
        self.schedule_stop = QLineEdit(settings.schedule_stop)
        self.schedule_days = QSpinBox(); self.schedule_days.setRange(0, 3650); self.schedule_days.setValue(int(settings.schedule_days))
        self.burst = QCheckBox("Take five back-to-back burst images each cycle"); self.burst.setChecked(settings.burst_capture_enabled)
        schedule_form.addRow(self.schedule_enabled)
        schedule_form.addRow("Start", self.schedule_start)
        schedule_form.addRow("Stop", self.schedule_stop)
        schedule_form.addRow("Days (0 = unlimited)", self.schedule_days)
        schedule_form.addRow("Capture interval", self.interval)
        schedule_form.addRow(self.burst)
        tabs.addTab(schedule, "Schedule")

        security = QWidget()
        security_layout = QVBoxLayout(security)
        security_layout.addWidget(label("Protect destructive device operations with a password.", muted=True))
        security_layout.addWidget(button("Change password"), 0, Qt.AlignmentFlag.AlignLeft)
        security_layout.addStretch()
        tabs.addTab(security, "Security")

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        outer.addWidget(buttons)

    def values(self) -> GuiSettings:
        return GuiSettings(
            baud=self.baud.currentText().strip(),
            transport=self.transport.currentText(),
            bt_target=self.bt_target.text().strip(),
            schedule_enabled=self.schedule_enabled.isChecked(),
            schedule_start=self.schedule_start.text().strip(),
            schedule_stop=self.schedule_stop.text().strip(),
            schedule_days=str(self.schedule_days.value()),
            capture_interval_s=str(self.interval.value()),
            burst_capture_enabled=self.burst.isChecked(),
        )


class MainWindow(QMainWindow):
    def __init__(self, auth: PasswordAuth) -> None:
        super().__init__()
        self.auth = auth
        self.dark = False
        self.running = False
        self.pending_running_state: bool | None = None
        self.settings_store = SettingsStore(SETTINGS_FILE)
        self.settings = self.settings_store.load() or GuiSettings()
        self.backend = DeviceBackend()
        self.backend.log.connect(self.append_log)
        self.backend.dashboard.connect(self.apply_dashboard)
        self.backend.command_done.connect(self.command_finished)
        self.backend.busy_changed.connect(self.set_busy)
        self.backend.transfer_progress.connect(self.update_transfer_progress)
        self.backend.transfer_done.connect(self.transfer_finished)
        self.backend.delete_done.connect(self.delete_finished)
        self.ports: list[str] = []
        self.last_port_signature: tuple[tuple[str, str, str], ...] = ()
        self.setWindowTitle("Camera GUI")
        self.resize(1180, 820)
        self.setMinimumSize(700, 560)
        self.build_ui()
        self.apply_theme()
        self.refresh_media()
        self.refresh_ports()
        self.poll_timer = QTimer(self)
        self.poll_timer.timeout.connect(self.backend.refresh_dashboard)
        self.poll_timer.start(900)
        self.port_timer = QTimer(self)
        self.port_timer.timeout.connect(self.refresh_ports)
        self.port_timer.start(1500)

    def build_ui(self) -> None:
        page = QWidget(objectName="page")
        self.setCentralWidget(page)
        root = QVBoxLayout(page)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        topbar = QFrame(objectName="topbar")
        topbar.setFixedHeight(72)
        top = QHBoxLayout(topbar)
        top.setContentsMargins(26, 10, 26, 10)
        logo = QLabel("●")
        logo.setAlignment(Qt.AlignmentFlag.AlignCenter)
        logo.setFixedSize(40, 40)
        logo.setStyleSheet(f"background:{BLUE};color:white;border-radius:11px;font-size:20px")
        top.addWidget(logo)
        names = QVBoxLayout()
        names.setSpacing(0)
        names.addWidget(label("Camera GUI", size=14, bold=True))
        names.addWidget(label("Device control center", muted=True))
        top.addLayout(names)
        top.addStretch()
        self.connection_label = QLabel("●  Looking for device")
        self.connection_label.setStyleSheet(f"color:{GREEN};font-weight:700;padding:7px 12px;background:{GREEN}18;border-radius:12px")
        top.addWidget(self.connection_label)
        theme = QToolButton(); theme.setText("☾"); theme.setToolTip("Toggle appearance"); theme.clicked.connect(self.toggle_theme)
        settings = QToolButton(); settings.setText("⚙"); settings.setToolTip("Settings"); settings.clicked.connect(self.open_settings)
        top.addWidget(theme); top.addWidget(settings)
        root.addWidget(topbar)

        scroll = QScrollArea(); scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        content = QWidget(objectName="page")
        content.setMinimumWidth(0)
        content.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        scroll.setWidget(content)
        body = QVBoxLayout(content)
        body.setContentsMargins(28, 24, 28, 28)
        body.setSpacing(18)
        welcome = QHBoxLayout()
        intro = QVBoxLayout(); intro.setSpacing(3)
        intro.addWidget(label("Good morning", size=22, bold=True))
        intro.addWidget(label("Your camera is online and following its schedule.", muted=True))
        welcome.addLayout(intro); welcome.addStretch()
        refresh = button("↻  Refresh"); refresh.clicked.connect(self.manual_refresh)
        welcome.addWidget(refresh)
        body.addLayout(welcome)

        statuses = QGridLayout(); statuses.setSpacing(14)
        self.device_card = StatusCard("Device", "Waiting", "Select or connect a device", "▶", GREEN)
        self.sd_card = StatusCard("SD card", "Unknown", "Waiting for firmware status", "▣", BLUE)
        self.schedule_card = StatusCard("Schedule", "Configured", schedule_preview(self.settings.schedule_enabled, self.settings.schedule_start, self.settings.schedule_stop, self.settings.schedule_days, self.settings.capture_interval_s, self.settings.burst_capture_enabled).removeprefix("Schedule preview: "), "◷", PURPLE)
        statuses.addWidget(self.device_card, 0, 0)
        statuses.addWidget(self.sd_card, 0, 1)
        statuses.addWidget(self.schedule_card, 0, 2)
        statuses.setColumnStretch(0, 1); statuses.setColumnStretch(1, 1); statuses.setColumnStretch(2, 1)
        body.addLayout(statuses)

        controls, controls_layout = card()
        control_row = QHBoxLayout()
        control_text = QVBoxLayout(); control_text.setSpacing(2)
        control_text.addWidget(label("Capture controls", size=13, bold=True))
        self.run_label = label("Program is stopped", muted=True)
        control_text.addWidget(self.run_label)
        control_row.addLayout(control_text); control_row.addStretch()
        controls_layout.addLayout(control_row)
        action_row = QHBoxLayout()
        action_row.setSpacing(8)
        self.start = button("▶  Start capture", primary=True); self.start.clicked.connect(lambda: self.set_running(True))
        self.stop = button("■  Stop", danger=True); self.stop.setEnabled(False); self.stop.clicked.connect(lambda: self.set_running(False))
        self.retrieve = button("↓  Retrieve media"); self.retrieve.clicked.connect(self.retrieve_media)
        self.delete = button("Delete media", danger=True); self.delete.clicked.connect(self.confirm_delete)
        for widget in (self.start, self.stop, self.retrieve, self.delete):
            widget.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
            action_row.addWidget(widget)
        controls_layout.addLayout(action_row)
        self.progress = QProgressBar(); self.progress.setRange(0, 100); self.progress.setValue(0); self.progress.hide()
        controls_layout.addWidget(self.progress)
        body.addWidget(controls)

        tabs = QTabWidget()
        tabs.addTab(self.media_page(), "Media")
        tabs.addTab(self.summary_page(), "Daily summaries")
        tabs.addTab(self.activity_page(), "Activity")
        body.addWidget(tabs, 1)
        root.addWidget(scroll, 1)
        self.statusBar().showMessage("Ready · device commands are sent only when you use a control")

    def media_page(self) -> QWidget:
        page = QWidget(objectName="page")
        layout = QVBoxLayout(page); layout.setContentsMargins(0, 16, 0, 0)
        heading = QHBoxLayout(); heading.addWidget(label("Recent captures", size=13, bold=True)); heading.addStretch()
        self.media_search = QLineEdit(); self.media_search.setPlaceholderText("Search media"); self.media_search.setMaximumWidth(260); self.media_search.textChanged.connect(self.refresh_media); heading.addWidget(self.media_search)
        layout.addLayout(heading)
        self.media_grid = QGridLayout(); self.media_grid.setSpacing(14)
        for column in range(3): self.media_grid.setColumnStretch(column, 1)
        layout.addLayout(self.media_grid); layout.addStretch()
        return page

    def summary_page(self) -> QWidget:
        page = QWidget(objectName="page")
        layout = QVBoxLayout(page); layout.setContentsMargins(0, 16, 0, 0)
        self.summary_list = QListWidget()
        self.summary_list.itemDoubleClicked.connect(self.open_local_summary)
        layout.addWidget(self.summary_list)
        self.refresh_summaries()
        return page

    def refresh_summaries(self) -> None:
        self.summary_list.clear()
        try:
            paths = sorted(
                (path for path in SUMMARY_DIRECTORY.iterdir() if path.is_file()),
                key=lambda path: path.name,
                reverse=True,
            ) if SUMMARY_DIRECTORY.exists() else []
        except OSError:
            paths = []
        if not paths:
            item = QListWidgetItem("No transferred summaries yet. Use Retrieve media first.")
            item.setFlags(Qt.ItemFlag.NoItemFlags)
            self.summary_list.addItem(item)
            return
        for path in paths:
            item = QListWidgetItem(path.name)
            item.setData(Qt.ItemDataRole.UserRole, str(path))
            item.setSizeHint(QSize(0, 46))
            self.summary_list.addItem(item)

    def open_local_summary(self, item: QListWidgetItem) -> None:
        value = item.data(Qt.ItemDataRole.UserRole)
        if not value:
            return
        path = Path(value)
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            QMessageBox.critical(self, "Unable to open summary", str(exc))
            return
        dialog = QDialog(self); dialog.setWindowTitle(path.name); dialog.resize(760, 560)
        layout = QVBoxLayout(dialog)
        viewer = QTextEdit(); viewer.setReadOnly(True); viewer.setPlainText(text)
        layout.addWidget(viewer)
        dialog.exec()

    def activity_page(self) -> QWidget:
        page = QWidget(objectName="page")
        layout = QVBoxLayout(page); layout.setContentsMargins(0, 16, 0, 0)
        self.log_widget = QTextEdit(); self.log_widget.setReadOnly(True)
        self.log_widget.setFont(QFont("Cascadia Mono", 10))
        self.log_widget.setPlainText("Native GUI initialized.\nTip: close idf.py monitor before connecting.")
        layout.addWidget(self.log_widget)
        return page

    def set_running(self, running: bool) -> None:
        if running:
            try:
                schedule = (
                    build_schedule_spec(
                        self.settings.schedule_start,
                        self.settings.schedule_stop,
                        self.settings.schedule_days,
                        self.settings.capture_interval_s,
                        self.settings.burst_capture_enabled,
                    )
                    if self.settings.schedule_enabled
                    else None
                )
                request = build_program_start(
                    self.settings.schedule_enabled,
                    self.settings.capture_interval_s,
                    self.settings.burst_capture_enabled,
                    dt.datetime.now(),
                    schedule,
                )
            except ScheduleValidationError as exc:
                QMessageBox.critical(self, "Invalid schedule", str(exc))
                return
            self.pending_running_state = True
            self.run_label.setText("Starting program…")
            self.backend.send(request.command)
        else:
            self.pending_running_state = False
            self.run_label.setText("Stopping program…")
            self.backend.send("STOP_PROGRAM")

    def retrieve_media(self) -> None:
        if not self.backend.port:
            QMessageBox.warning(self, "No COM port", "Select a COM port in Settings before retrieving media.")
            return
        self.progress.setRange(0, 0)
        self.progress.show()
        self.statusBar().showMessage("Reading media inventory from the device…")
        self.backend.retrieve_media()

    def confirm_delete(self) -> None:
        if not self.backend.port:
            QMessageBox.warning(self, "No COM port", "Select a COM port in Settings before deleting media.")
            return
        password, accepted = QInputDialog.getText(
            self,
            "Authentication required",
            "Enter GUI password to delete all SD media:",
            QLineEdit.EchoMode.Password,
        )
        if not accepted:
            return
        if not self.auth.verify(password):
            QMessageBox.critical(self, "Access denied", "Password is incorrect.")
            return
        result = QMessageBox.warning(
            self,
            "Delete all SD media?",
            "This will permanently remove every image, video, and summary from the camera. This cannot be undone.",
            QMessageBox.StandardButton.Cancel | QMessageBox.StandardButton.Yes,
            QMessageBox.StandardButton.Cancel,
        )
        if result == QMessageBox.StandardButton.Yes:
            self.statusBar().showMessage("Deleting media from the device…")
            self.backend.delete_media()

    def open_preview(self, entry: ImageCatalogEntry) -> None:
        dialog = QDialog(self); dialog.setWindowTitle(entry.path.name); dialog.resize(920, 600)
        layout = QVBoxLayout(dialog)
        image = QLabel(); image.setAlignment(Qt.AlignmentFlag.AlignCenter)
        image.setPixmap(QPixmap(str(entry.path)).scaled(900, 520, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation))
        layout.addWidget(image, 1); layout.addWidget(label(f"{entry.category} · {entry.path.name}", bold=True))
        dialog.exec()

    @Slot()
    def refresh_media(self, *_args) -> None:
        while self.media_grid.count():
            item = self.media_grid.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        catalog = build_image_catalog(IMAGE_DIRECTORY, BURST_DIRECTORY)
        query = self.media_search.text().strip().lower()
        entries = [entry for entry in catalog.entries if not query or query in entry.display_text.lower()]
        if not entries:
            message = "No matching images." if query else "No transferred images yet. Click Retrieve media to download them from the device."
            empty = label(message, muted=True)
            empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
            empty.setMinimumHeight(180)
            self.media_grid.addWidget(empty, 0, 0, 1, 3)
            return
        for index, entry in enumerate(entries):
            self.media_grid.addWidget(MediaCard(entry, self.open_preview), index // 3, index % 3)

    @Slot(int, int, str)
    def update_transfer_progress(self, completed: int, total: int, status: str) -> None:
        if total > 0:
            self.progress.setRange(0, total)
            self.progress.setValue(min(completed, total))
        else:
            self.progress.setRange(0, 0)
        self.progress.show()
        self.statusBar().showMessage(status)

    @Slot(object)
    def transfer_finished(self, result) -> None:
        if result.total:
            self.progress.setRange(0, result.total)
            self.progress.setValue(result.completed)
        else:
            self.progress.hide()
        self.statusBar().showMessage(result.status_text, 8000)
        self.append_log(result.status_text)
        self.refresh_media()
        self.refresh_summaries()

    @Slot(object)
    def delete_finished(self, result) -> None:
        for message in result.messages:
            self.append_log(message)
        if result.success:
            self.statusBar().showMessage("All SD media deleted successfully", 8000)
            self.progress.hide()
            self.refresh_media()
            self.refresh_summaries()
        else:
            self.statusBar().showMessage("Delete operation was not confirmed by the device", 8000)

    def open_settings(self) -> None:
        dialog = SettingsDialog(self.settings, self.ports, self)
        if self.backend.port and dialog.port.findText(self.backend.port) >= 0:
            dialog.port.setCurrentText(self.backend.port)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            candidate = dialog.values()
            try:
                baud = int(candidate.baud)
            except ValueError:
                QMessageBox.critical(self, "Invalid baud", "Baud must be an integer.")
                return
            self.settings = candidate
            self.settings_store.save(candidate)
            selected_port = dialog.port.currentText()
            self.backend.configure(selected_port, baud)
            self.connection_label.setText(f"●  {selected_port or 'No COM port'}")
            preview = schedule_preview(candidate.schedule_enabled, candidate.schedule_start, candidate.schedule_stop, candidate.schedule_days, candidate.capture_interval_s, candidate.burst_capture_enabled)
            self.schedule_card.set_content("Configured", preview.removeprefix("Schedule preview: "))
            self.statusBar().showMessage("Settings saved", 3000)
            self.backend.refresh_dashboard()

    def refresh_ports(self) -> None:
        if list_ports is None:
            self.append_log("pyserial is unavailable")
            return
        port_entries = list(list_ports.comports())
        self.ports = [item.device for item in port_entries]
        signature = tuple(
            (item.device, item.description or "", item.hwid or "")
            for item in port_entries
        )
        ports_changed = signature != self.last_port_signature
        self.last_port_signature = signature

        def is_usb_device(item) -> bool:
            description = (item.description or "").lower()
            hwid = (item.hwid or "").lower()
            return (
                getattr(item, "vid", None) == 0x303A
                or "usb" in description
                or "usb" in hwid
            )

        usb_ports = [item.device for item in port_entries if is_usb_device(item)]
        current_is_usb = self.backend.port in usb_ports
        if current_is_usb:
            selected = self.backend.port
        elif usb_ports:
            selected = usb_ports[0]
        else:
            # Never auto-select Intel AMT/SOL or another unrelated system port.
            # Such ports remain available for explicit selection in Settings.
            selected = ""
        try:
            baud = int(self.settings.baud)
        except ValueError:
            baud = DEFAULT_BAUD
        previous = self.backend.port
        self.backend.configure(selected, baud)
        if selected:
            port_changed = selected != previous
            if port_changed:
                self.connection_label.setText(f"●  {selected} · waiting")
            if ports_changed:
                details = ", ".join(
                    f"{item.device} ({item.description})" for item in port_entries
                )
                self.append_log(f"Detected ports: {details}")
            if port_changed:
                self.append_log(f"Selected USB camera port: {selected}")
                self.backend.refresh_dashboard()
        else:
            self.connection_label.setText("●  Camera not connected")
            if self.ports and ports_changed:
                self.append_log(
                    "No USB camera port found; unrelated system ports were ignored."
                )
            elif not self.ports and ports_changed:
                self.append_log("No serial ports found.")

    def manual_refresh(self) -> None:
        self.statusBar().showMessage("Refreshing device and SD status…")
        self.refresh_ports()
        self.backend.refresh_dashboard()

    @Slot(str)
    def append_log(self, message: str) -> None:
        stamp = dt.datetime.now().strftime("%H:%M:%S")
        self.log_widget.append(f"[{stamp}] {message}")

    @Slot(object)
    def apply_dashboard(self, result) -> None:
        if result.error:
            self.append_log(f"Status refresh failed: {result.error}")
            return
        if result.view is None:
            return
        view = result.view
        if view.device_text:
            device = view.device_text.removeprefix("Device:").strip()
            if device:
                device = device[0].upper() + device[1:]
            self.device_card.set_content(device or "Online", f"Serial · {self.backend.port}")
            self.connection_label.setText(f"●  Connected · {self.backend.port}")
        if view.sd_text:
            sd = view.sd_text.removeprefix("SD:").strip()
            if sd:
                sd = sd[0].upper() + sd[1:]
            self.sd_card.set_content(sd or "Available", "Reported by device")

    @Slot(object)
    def command_finished(self, result) -> None:
        succeeded = not result.error and any(line.startswith("OK ") for line in result.lines)
        if result.error:
            self.append_log(result.error)
        elif result.lines:
            for line in result.lines:
                self.append_log(f"RX: {line}")
        else:
            self.append_log("No immediate response received.")
        if self.pending_running_state is not None:
            if succeeded:
                self.running = self.pending_running_state
                self.run_label.setText("Program is running" if self.running else "Program is stopped")
                self.statusBar().showMessage("Capture program started" if self.running else "Capture program stopped", 3000)
            else:
                self.run_label.setText("Program is running" if self.running else "Program is stopped")
                self.statusBar().showMessage("Device did not confirm the command", 4000)
            self.pending_running_state = None
        if result.refresh_ports:
            self.refresh_ports()
        self.backend.refresh_dashboard()

    @Slot(bool)
    def set_busy(self, busy: bool) -> None:
        self.start.setEnabled(not busy and not self.running)
        self.stop.setEnabled(not busy and self.running)
        self.retrieve.setEnabled(not busy)
        self.delete.setEnabled(not busy)

    def closeEvent(self, event) -> None:
        self.settings_store.save(self.settings)
        self.backend.close()
        super().closeEvent(event)

    def toggle_theme(self) -> None:
        self.dark = not self.dark
        self.apply_theme()

    def apply_theme(self) -> None:
        QApplication.instance().setStyleSheet(DARK_STYLE if self.dark else LIGHT_STYLE)


def authenticate(auth: PasswordAuth) -> bool:
    for _attempt in range(PASSWORD_MAX_ATTEMPTS):
        password, accepted = QInputDialog.getText(
            None,
            "Camera GUI · Authentication",
            "Enter GUI password:",
            QLineEdit.EchoMode.Password,
        )
        if not accepted:
            return False
        if auth.verify(password):
            return True
        QMessageBox.critical(None, "Access denied", "Password is incorrect.")
    QMessageBox.critical(None, "Access denied", "Too many failed attempts.")
    return False


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("Camera GUI")
    app.setStyle("Fusion")
    auth = PasswordAuth(
        env_password=os.getenv(PASSWORD_ENV_NAME),
        default_password=DEFAULT_PASSWORD,
        store_path=PASSWORD_STORE_FILE,
    )
    if not authenticate(auth):
        return 0
    window = MainWindow(auth)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
"""Native Qt front end built on the same services as the Tk application."""
