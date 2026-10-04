"""Pure transformations from protocol lines to UI view models and text."""

from dataclasses import dataclass

from command_service import clean_protocol_line


def parse_key_value_line(line: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for part in line.split()[1:]:
        if "=" in part:
            key, value = part.split("=", 1)
            values[key] = value
    return values


def format_gibibytes(byte_text: str) -> str:
    try:
        value = int(byte_text)
    except (TypeError, ValueError):
        return "unknown"
    return f"{value / (1024 ** 3):.2f} GB"


@dataclass(frozen=True)
class DashboardViewModel:
    device_text: str | None = None
    sd_text: str | None = None
    device_status_received: bool = False
    otg_text: str | None = None


def build_dashboard_view(
    status_lines: list[str], sd_lines: list[str]
) -> DashboardViewModel:
    device_text = None
    otg_text = None
    status_received = False
    for line in status_lines:
        clean = clean_protocol_line(line, "STATUS")
        if clean.startswith("STATUS "):
            values = parse_key_value_line(clean)
            auto = values.get("auto", "?")
            paused = values.get("paused", "?")
            transfer = values.get("transfer", "?")
            schedule = values.get("schedule", "?")
            burst = values.get("burst", values.get("video", "?"))
            device_time = values.get("time", "unknown").replace("_", " ")
            interval_ms = values.get("interval_ms", "?")
            if auto == "1" and paused != "1":
                program_state = "Program running"
            elif auto == "1":
                program_state = "Program paused"
            else:
                program_state = "Program stopped"
            schedule_state = (
                "Schedule is active" if schedule == "1" else "Schedule is deactivated"
            )
            transfer_state = "Transfer active" if transfer == "1" else "No transfer"
            media_state = (
                "Image + burst"
                if burst == "1"
                else "Image only"
                if burst == "0"
                else "Media mode unknown"
            )
            try:
                interval_label = f"{int(interval_ms) // 1000}s"
            except ValueError:
                interval_label = f"{interval_ms} ms"
            device_text = (
                f"Device: time {device_time} | {program_state} | {schedule_state} | "
                f"{media_state} | {transfer_state} | interval {interval_label}"
            )
            if "scene" in values:
                scene_text = "Last captured scene: unavailable"
                if values["scene"] in ("light", "dark"):
                    try:
                        brightness = int(values.get("scene_avg_luma", ""))
                        dark_pct = int(values.get("scene_dark_pct", ""))
                        if 0 <= brightness <= 255 and 0 <= dark_pct <= 100:
                            scene_text = (
                                f"Last captured scene: {values['scene'].title()} | "
                                f"brightness {brightness}/255 | dark pixels {dark_pct}%"
                            )
                    except ValueError:
                        pass
                device_text += "\n" + scene_text
            otg = values.get("otg")
            otg_ready = values.get("otg_ready")
            if otg == "1" and transfer == "1":
                otg_text = "OTG: connected | Mass Storage transfer active"
            elif otg == "1" and otg_ready == "1":
                otg_text = "OTG: connected | Media ready for fast transfer"
            elif otg == "1":
                otg_text = "OTG: connected | Media is not ready"
            elif otg == "0":
                otg_text = "OTG: checked when Get SD Media is clicked"
            else:
                otg_text = "OTG: status unavailable; firmware update required"
            status_received = True
            break
        if clean.startswith("ERR "):
            device_text = f"Device: {clean}"
            break

    sd_text = None
    for line in sd_lines:
        clean = clean_protocol_line(line, "SD_STATUS")
        if clean.startswith("SD_STATUS "):
            values = parse_key_value_line(clean)
            sd_text = (
                f"SD: total {format_gibibytes(values.get('total', ''))} | "
                f"free {format_gibibytes(values.get('free', ''))} | "
                f"used {format_gibibytes(values.get('used', ''))} | "
                f"images={values.get('images', '?')} "
                f"summaries={values.get('summaries', '?')}"
            )
            break
        if clean.startswith("ERR "):
            sd_text = f"SD: {clean}"
            break
    return DashboardViewModel(
        device_text=device_text,
        sd_text=sd_text,
        device_status_received=status_received,
        otg_text=otg_text,
    )


@dataclass(frozen=True)
class SummaryListViewModel:
    names: tuple[str, ...]
    errors: tuple[str, ...]

    @property
    def status_text(self) -> str:
        if self.names:
            return f"Found {len(self.names)} daily summary file(s)."
        return "No daily summary files found yet."


def build_summary_list(lines: list[str]) -> SummaryListViewModel:
    names: set[str] = set()
    errors: list[str] = []
    for line in lines:
        clean = clean_protocol_line(line, "SUMMARY_LIST")
        if clean.startswith("SUMMARY "):
            parts = clean.split()
            if len(parts) >= 2:
                names.add(parts[1])
        elif clean.startswith("ERR "):
            errors.append(clean)
    return SummaryListViewModel(
        tuple(sorted(names, reverse=True)),
        tuple(errors),
    )


def build_summary_text(name: str, lines: list[str]) -> str:
    body: list[str] = []
    for line in lines:
        clean = clean_protocol_line(line, "SUMMARY_GET")
        if clean.startswith("TEXT "):
            body.append(clean[5:])
        elif clean.startswith("ERR "):
            body.append(clean)
    return "\n".join(body) if body else f"No content received for {name}."


def parse_summary_delete_result(lines: list[str]) -> str:
    for line in lines:
        clean = clean_protocol_line(line, "SUMMARY_DELETE_ALL")
        if clean.startswith(("OK SUMMARY_DELETE_ALL", "ERR SUMMARY_DELETE_ALL")):
            return clean
    return ""
