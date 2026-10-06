"""Atomic GUI settings persistence and capture-schedule validation."""

import datetime as dt
import json
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass(frozen=True)
class GuiSettings:
    baud: str = "921600"
    transport: str = "Serial"
    bt_target: str = "RaphasCam"
    schedule_enabled: bool = False
    schedule_start: str = "08:00"
    schedule_stop: str = "22:00"
    schedule_days: str = "1"
    capture_interval_s: str = "30"
    burst_capture_enabled: bool = False


class SettingsStore:
    """Load and atomically persist preferences independently of UI widgets."""
    def __init__(self, path: Path) -> None:
        self._path = path

    def load(self) -> GuiSettings | None:
        if not self._path.exists():
            return None
        try:
            payload = json.loads(self._path.read_text(encoding="utf-8"))
        except (OSError, ValueError, TypeError):
            return None
        defaults = GuiSettings()
        transport = payload.get("transport", defaults.transport)
        if transport not in ("Serial", "BT"):
            transport = defaults.transport
        burst_value = payload.get(
            "burst_capture_enabled",
            payload.get("video_capture_enabled", defaults.burst_capture_enabled),
        )
        burst = burst_value if isinstance(burst_value, bool) else defaults.burst_capture_enabled
        return GuiSettings(
            baud=payload.get("baud") if isinstance(payload.get("baud"), str) else defaults.baud,
            transport=transport,
            bt_target=payload.get("bt_target") if isinstance(payload.get("bt_target"), str) else defaults.bt_target,
            schedule_enabled=payload.get("schedule_enabled") if isinstance(payload.get("schedule_enabled"), bool) else defaults.schedule_enabled,
            schedule_start=payload.get("schedule_start") if isinstance(payload.get("schedule_start"), str) else defaults.schedule_start,
            schedule_stop=payload.get("schedule_stop") if isinstance(payload.get("schedule_stop"), str) else defaults.schedule_stop,
            schedule_days=payload.get("schedule_days") if isinstance(payload.get("schedule_days"), str) else defaults.schedule_days,
            capture_interval_s=payload.get("capture_interval_s") if isinstance(payload.get("capture_interval_s"), str) else defaults.capture_interval_s,
            burst_capture_enabled=burst,
        )

    def save(self, settings: GuiSettings) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        temp_path = self._path.with_suffix(self._path.suffix + ".tmp")
        try:
            temp_path.write_text(json.dumps(asdict(settings), indent=2), encoding="utf-8")
            temp_path.replace(self._path)
        finally:
            try:
                temp_path.unlink(missing_ok=True)
            except OSError:
                pass


class ScheduleValidationError(ValueError):
    pass


@dataclass(frozen=True)
class ScheduleSpec:
    start: str
    stop: str
    days: int
    interval_seconds: int
    burst_enabled: int

    @property
    def command_fields(self) -> str:
        return (
            f"{self.start} {self.stop} {self.days} "
            f"{self.interval_seconds} {self.burst_enabled}"
        )


@dataclass(frozen=True)
class ProgramStartRequest:
    command: str
    messages: tuple[str, str, str]


def validate_interval(interval_text: str) -> int:
    try:
        interval = int(interval_text.strip())
    except ValueError as exc:
        raise ScheduleValidationError(
            "Capture interval must be a whole number of seconds."
        ) from exc
    if interval < 5 or interval > 3600:
        raise ScheduleValidationError(
            "Capture interval must be between 5 and 3600 seconds."
        )
    return interval


def build_schedule_spec(
    start_text: str,
    stop_text: str,
    days_text: str,
    interval_text: str,
    burst_enabled: bool,
) -> ScheduleSpec:
    start = start_text.strip()
    stop = stop_text.strip()
    try:
        dt.datetime.strptime(start, "%H:%M")
        dt.datetime.strptime(stop, "%H:%M")
    except ValueError as exc:
        raise ScheduleValidationError(
            "Start and stop time must use HH:MM, for example 08:00."
        ) from exc
    try:
        days = int(days_text.strip())
    except ValueError as exc:
        raise ScheduleValidationError(
            "Days to run must be a number. Use 0 for unlimited."
        ) from exc
    if days < 0 or days > 3650:
        raise ScheduleValidationError(
            "Days to run must be between 0 and 3650."
        )
    return ScheduleSpec(
        start,
        stop,
        days,
        validate_interval(interval_text),
        int(burst_enabled),
    )


def schedule_preview(
    enabled: bool,
    start: str,
    stop: str,
    days_text: str,
    interval_text: str,
    burst_enabled: bool,
) -> str:
    """Describe a schedule without mutating settings or contacting the device."""
    media_mode = "three back-to-back burst images" if burst_enabled else "one image"
    if not enabled:
        return (
            "Schedule preview: daily run window disabled; Start Program captures "
            f"{media_mode} immediately."
        )
    try:
        days = int(days_text.strip() or "0")
    except ValueError:
        days = -1
    days_label = (
        "every day until stopped"
        if days == 0
        else f"for {days} day(s)"
        if days > 0
        else "with an invalid day count"
    )
    return (
        f"Schedule preview: capture {media_mode} every "
        f"{interval_text.strip() or '?'}s from {start.strip() or '--:--'} "
        f"to {stop.strip() or '--:--'}, {days_label}; sleep outside that window."
    )


def build_program_start(
    schedule_enabled: bool,
    interval_text: str,
    burst_enabled: bool,
    now: dt.datetime,
    schedule: ScheduleSpec | None = None,
) -> ProgramStartRequest:
    burst_text = "enabled" if burst_enabled else "disabled"
    if not schedule_enabled:
        interval = validate_interval(interval_text)
        return ProgramStartRequest(
            f"START_PROGRAM {interval} {int(burst_enabled)}",
            (
                f"Program started, burst {burst_text}",
                f"From {now:%Y-%m-%d %H:%M:%S} until manually stopped",
                "Program in progress...",
            ),
        )
    if schedule is None:
        raise ScheduleValidationError("Schedule settings are required.")

    start_clock = dt.datetime.strptime(schedule.start, "%H:%M").time()
    stop_clock = dt.datetime.strptime(schedule.stop, "%H:%M").time()
    first_start = dt.datetime.combine(now.date(), start_clock)
    first_stop = dt.datetime.combine(now.date(), stop_clock)
    if first_stop <= first_start:
        first_stop += dt.timedelta(days=1)
    if now >= first_stop:
        first_start += dt.timedelta(days=1)
        first_stop += dt.timedelta(days=1)
    range_text = (
        f"From {first_start:%Y-%m-%d %H:%M} until manually stopped"
        if schedule.days == 0
        else (
            f"From {first_start:%Y-%m-%d %H:%M} "
            f"to {first_stop + dt.timedelta(days=schedule.days - 1):%Y-%m-%d %H:%M}"
        )
    )
    return ProgramStartRequest(
        f"START_SCHEDULE {schedule.command_fields}",
        (
            f"Program started, burst {burst_text}",
            range_text,
            "Program in progress...",
        ),
    )
