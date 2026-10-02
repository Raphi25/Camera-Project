"""Plot TC66 current recordings from CSV or Excel files."""

from __future__ import annotations

import argparse
from bisect import bisect_right
from datetime import datetime, time
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.dates as mdates
    import matplotlib.pyplot as plt
    import pandas as pd
except ImportError as exc:  # Give a useful message instead of a traceback.
    raise SystemExit(
        "Missing plotting dependencies. Install them with: "
        "python -m pip install -r requirements.txt"
    ) from exc


SUPPORTED_EXTENSIONS = {".csv", ".xlsx", ".xls", ".xlsm"}
# Set this to a filename from the "Excel files" folder, or leave it as None
# to automatically use the newest recording. A command-line input overrides it.
INPUT_FILE: str | None = "2026-08-27-28-sc.csv"
DEAD_VOLTAGE_THRESHOLD = 0.05
DEAD_CURRENT_THRESHOLD = 0.001
DEAD_SAMPLE_COUNT = 10
DEFAULT_SLEEP_START = time(23, 0)
DEFAULT_SLEEP_END = time(8, 0)


def find_newest_recording(folder: Path) -> Path:
    files = [
        path
        for path in folder.iterdir()
        if path.is_file() and path.suffix.lower() in SUPPORTED_EXTENSIONS
    ]
    if not files:
        raise FileNotFoundError(f"No CSV or Excel recordings found in: {folder}")
    return max(files, key=lambda path: path.stat().st_mtime)


def load_recording(path: Path, sheet: str | int = 0) -> pd.DataFrame:
    suffix = path.suffix.lower()
    if suffix == ".csv":
        frame = pd.read_csv(path, encoding="utf-8-sig")
    elif suffix in {".xlsx", ".xlsm"}:
        frame = pd.read_excel(path, sheet_name=sheet, engine="openpyxl")
    elif suffix == ".xls":
        frame = pd.read_excel(path, sheet_name=sheet)
    else:
        raise ValueError(f"Unsupported file type: {suffix}")

    required = ["Time", "Voltage (V)", "Current (A)"]
    missing = [column for column in required if column not in frame.columns]
    if missing:
        raise ValueError(
            f"{path.name} is missing required column(s): {', '.join(missing)}. "
            f"Available columns: {', '.join(map(str, frame.columns))}"
        )

    data = frame[required].copy()
    data["Time"] = pd.to_datetime(data["Time"], errors="coerce")
    data["Voltage (V)"] = pd.to_numeric(data["Voltage (V)"], errors="coerce")
    data["Current (A)"] = pd.to_numeric(data["Current (A)"], errors="coerce")
    data = data.dropna().sort_values("Time")
    if data.empty:
        raise ValueError("No valid time, voltage, and current rows were found.")
    return data


def trim_after_battery_death(data: pd.DataFrame) -> tuple[pd.DataFrame, int]:
    dead_sample = (
        (data["Voltage (V)"] <= DEAD_VOLTAGE_THRESHOLD)
        & (data["Current (A)"] <= DEAD_CURRENT_THRESHOLD)
    )
    sustained_death = dead_sample.rolling(
        window=DEAD_SAMPLE_COUNT, min_periods=DEAD_SAMPLE_COUNT
    ).sum().eq(DEAD_SAMPLE_COUNT)

    matches = sustained_death.to_numpy().nonzero()[0]
    if len(matches) == 0:
        return data, 0

    first_dead_index = int(matches[0]) - DEAD_SAMPLE_COUNT + 1
    while (
        first_dead_index > 0
        and data["Current (A)"].iloc[first_dead_index - 1]
        <= DEAD_CURRENT_THRESHOLD
    ):
        first_dead_index -= 1
    trimmed = data.iloc[:first_dead_index].copy()
    if trimmed.empty:
        raise ValueError("Battery shutdown was detected before any usable samples.")
    return trimmed, len(data) - len(trimmed)


def plot_recording(data: pd.DataFrame, source: Path, output: Path) -> None:
    fig, current_axis = plt.subplots(figsize=(12, 6.75), constrained_layout=True)
    current_axis.plot(
        data["Time"], data["Current (A)"], color="#d1495b", linewidth=1.2,
        label="Current",
    )

    current_axis.set_title(f"TC66 Current — {source.stem}")
    current_axis.set_xlabel("Time")
    current_axis.set_ylabel("Current (A)", color="#d1495b")
    current_axis.tick_params(axis="x", labelsize=8)
    current_axis.tick_params(axis="y", labelcolor="#d1495b")
    current_axis.grid(True, alpha=0.25)

    span = data["Time"].iloc[-1] - data["Time"].iloc[0]
    time_format = "%H:%M" if span.days < 1 else "%m-%d\n%H:%M"
    current_axis.xaxis.set_major_locator(mdates.HourLocator(interval=1))
    current_axis.xaxis.set_major_formatter(mdates.DateFormatter(time_format))
    fig.autofmt_xdate(rotation=45, ha="right")
    current_axis.legend(loc="best")

    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=160)
    plt.close(fig)


def calculate_current_statistics(data: pd.DataFrame) -> dict[str, float]:
    current = data["Current (A)"].to_numpy()
    elapsed_seconds = (
        data["Time"] - data["Time"].iloc[0]
    ).dt.total_seconds().to_numpy()

    duration_seconds = float(elapsed_seconds[-1])
    amp_seconds = 0.0
    for index in range(1, len(data)):
        interval = elapsed_seconds[index] - elapsed_seconds[index - 1]
        amp_seconds += (current[index - 1] + current[index]) / 2 * interval

    total_ah = amp_seconds / 3600
    average_current = (
        amp_seconds / duration_seconds
        if duration_seconds > 0
        else float(current.mean())
    )
    return {
        "minimum_a": float(current.min()),
        "maximum_a": float(current.max()),
        "average_a": average_current,
        "total_ah": total_ah,
        "per_24_hours_ah": average_current * 24,
        "duration_seconds": duration_seconds,
    }


def calculate_camera_state_statistics(
    data: pd.DataFrame, sleep_start: time, sleep_end: time
) -> dict[str, float]:
    """Calculate time-weighted current averages using a daily sleep schedule.

    Current is treated as changing linearly between samples. Intervals that
    cross a scheduled state transition are split at that transition.
    """
    if sleep_start == sleep_end:
        raise ValueError("Sleep start and end times must be different.")

    current = data["Current (A)"].to_numpy()
    timestamps = data["Time"].tolist()
    active_amp_seconds = 0.0
    active_seconds = 0.0
    asleep_amp_seconds = 0.0
    asleep_seconds = 0.0

    first_day = data["Time"].iloc[0].normalize() - pd.Timedelta(days=1)
    last_day = data["Time"].iloc[-1].normalize() + pd.Timedelta(days=1)
    days = pd.date_range(first_day, last_day, freq="D")
    transitions = sorted(
        [
            day + pd.Timedelta(hours=clock.hour, minutes=clock.minute)
            for day in days
            for clock in (sleep_start, sleep_end)
        ]
    )

    def is_asleep(timestamp: pd.Timestamp) -> bool:
        clock = timestamp.time()
        if sleep_start < sleep_end:
            return sleep_start <= clock < sleep_end
        return clock >= sleep_start or clock < sleep_end

    for index in range(1, len(data)):
        interval_start = timestamps[index - 1]
        interval_end = timestamps[index]
        start_current = float(current[index - 1])
        end_current = float(current[index])
        interval = float((interval_end - interval_start).total_seconds())
        if interval <= 0:
            continue

        first_transition = bisect_right(transitions, interval_start)
        split_points = [interval_start]
        transition_index = first_transition
        while (
            transition_index < len(transitions)
            and transitions[transition_index] < interval_end
        ):
            split_points.append(transitions[transition_index])
            transition_index += 1
        split_points.append(interval_end)

        for segment_start, segment_end in zip(split_points, split_points[1:]):
            start_fraction = (
                (segment_start - interval_start).total_seconds() / interval
            )
            end_fraction = (
                (segment_end - interval_start).total_seconds() / interval
            )
            segment_start_current = start_current + (
                end_current - start_current
            ) * start_fraction
            segment_end_current = start_current + (
                end_current - start_current
            ) * end_fraction
            segment_seconds = float((segment_end - segment_start).total_seconds())
            segment_amp_seconds = (
                (segment_start_current + segment_end_current) / 2 * segment_seconds
            )
            midpoint = segment_start + (segment_end - segment_start) / 2
            if is_asleep(midpoint):
                asleep_seconds += segment_seconds
                asleep_amp_seconds += segment_amp_seconds
            else:
                active_seconds += segment_seconds
                active_amp_seconds += segment_amp_seconds

    return {
        "active_average_a": (
            active_amp_seconds / active_seconds
            if active_seconds > 0
            else float("nan")
        ),
        "asleep_average_a": (
            asleep_amp_seconds / asleep_seconds
            if asleep_seconds > 0
            else float("nan")
        ),
        "active_duration_seconds": active_seconds,
        "asleep_duration_seconds": asleep_seconds,
    }


def format_duration(duration_seconds: float) -> str:
    total_seconds = int(round(duration_seconds))
    hours, remainder = divmod(total_seconds, 3600)
    minutes, seconds = divmod(remainder, 60)
    return f"{hours:02d}:{minutes:02d}:{seconds:02d}"


def format_state_average(label: str, average_a: float, duration_seconds: float) -> str:
    if average_a != average_a:  # NaN: the recording contains no time in this state.
        return f"Average current while camera is {label}: N/A (no {label} time)"
    return (
        f"Average current while camera is {label}: {average_a:.6f} A "
        f"({average_a * 1000:.3f} mA; duration {format_duration(duration_seconds)})"
    )


def write_current_summary(
    data: pd.DataFrame,
    source: Path,
    output: Path,
    sleep_start: time = DEFAULT_SLEEP_START,
    sleep_end: time = DEFAULT_SLEEP_END,
) -> None:
    stats = calculate_current_statistics(data)
    state_stats = calculate_camera_state_statistics(data, sleep_start, sleep_end)
    formatted_duration = format_duration(stats["duration_seconds"])
    lines = [
        "TC66 Current Draw Summary",
        "=========================",
        f"Source file: {source.name}",
        f"Start time: {data['Time'].iloc[0]}",
        f"End time: {data['Time'].iloc[-1]}",
        f"Duration: {formatted_duration}",
        f"Samples: {len(data)}",
        "",
        f"Minimum current draw: {stats['minimum_a']:.6f} A "
        f"({stats['minimum_a'] * 1000:.3f} mA)",
        f"Maximum current draw: {stats['maximum_a']:.6f} A "
        f"({stats['maximum_a'] * 1000:.3f} mA)",
        f"Average current draw: {stats['average_a']:.6f} A "
        f"({stats['average_a'] * 1000:.3f} mA)",
        format_state_average(
            "active",
            state_stats["active_average_a"],
            state_stats["active_duration_seconds"],
        ),
        format_state_average(
            "asleep",
            state_stats["asleep_average_a"],
            state_stats["asleep_duration_seconds"],
        ),
        f"Total current drawn: {stats['total_ah']:.9f} Ah "
        f"({stats['total_ah'] * 1000:.6f} mAh)",
        f"Current consumed per 24 hours: {stats['per_24_hours_ah']:.6f} Ah/24 h "
        f"({stats['per_24_hours_ah'] * 1000:.3f} mAh/24 h)",
        "",
        "The average and total use trapezoidal time integration between samples.",
        f"Camera schedule: asleep {sleep_start:%H:%M}-{sleep_end:%H:%M}; "
        "active outside that period.",
        "Intervals crossing a scheduled state transition are split at the transition.",
        "The 24-hour consumption is projected from the time-weighted average current.",
    ]
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_clock_time(value: str) -> time:
    try:
        return datetime.strptime(value, "%H:%M").time()
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            f"Invalid time '{value}'; expected HH:MM in 24-hour format."
        ) from exc


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Graph current over time from a TC66 CSV/Excel export."
    )
    parser.add_argument(
        "input",
        nargs="?",
        type=Path,
        help="CSV/XLSX/XLS file (default: newest supported file in 'Excel files').",
    )
    parser.add_argument(
        "--sheet",
        default="0",
        help="Excel sheet name or zero-based index (default: 0; ignored for CSV).",
    )
    parser.add_argument(
        "--output", type=Path, help="Output PNG path (default: <input>_graph.png)."
    )
    parser.add_argument(
        "--summary",
        type=Path,
        help="Output TXT path (default: <input>_summary.txt).",
    )
    parser.add_argument(
        "--sleep-start",
        type=parse_clock_time,
        default=DEFAULT_SLEEP_START,
        metavar="HH:MM",
        help="Daily camera sleep start time (default: 23:00).",
    )
    parser.add_argument(
        "--sleep-end",
        type=parse_clock_time,
        default=DEFAULT_SLEEP_END,
        metavar="HH:MM",
        help="Daily camera wake time (default: 08:00).",
    )
    args = parser.parse_args()
    if args.sleep_start == args.sleep_end:
        parser.error("--sleep-start and --sleep-end must be different")
    return args


def main() -> None:
    args = parse_args()
    if args.input is not None:
        source = args.input
    elif INPUT_FILE:
        source = Path(INPUT_FILE)
        if not source.is_absolute() and source.parent == Path("."):
            source = Path("Excel files") / source
    else:
        source = find_newest_recording(Path("Excel files"))
    source = source.expanduser().resolve()
    if not source.is_file():
        raise FileNotFoundError(f"Input file not found: {source}")

    sheet: str | int = int(args.sheet) if args.sheet.isdigit() else args.sheet
    output = args.output or source.with_name(f"{source.stem}_graph.png")
    output = output.expanduser().resolve()
    summary = args.summary or source.with_name(f"{source.stem}_summary.txt")
    summary = summary.expanduser().resolve()

    data = load_recording(source, sheet)
    data, discarded_samples = trim_after_battery_death(data)
    plot_recording(data, source, output)
    write_current_summary(
        data,
        source,
        summary,
        sleep_start=args.sleep_start,
        sleep_end=args.sleep_end,
    )
    print(f"Plotted {len(data):,} samples from {source.name}")
    if discarded_samples:
        print(f"Ignored {discarded_samples:,} samples after battery shutdown")
    print(f"Saved graph to: {output}")
    print(f"Saved current summary to: {summary}")


if __name__ == "__main__":
    main()
