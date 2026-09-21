#!/usr/bin/env python3
"""Analyze timing and motion data in BMI323 CSV logs."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from dataclasses import dataclass
from pathlib import Path

AXES = ("x", "y", "z")
VALUE_KEYS = tuple(
    [f"accel_{axis}_g" for axis in AXES]
    + [f"gyro_{axis}_dps" for axis in AXES]
)


@dataclass
class ImuLog:
    time_s: list[float]
    values: dict[str, list[float]]
    rejected_rows: int
    rtc_datetime: list[str]


@dataclass
class BodyMotion:
    relative_time_s: list[float]
    horizontal_rate_dps: list[float]
    absolute_rate_dps: list[float]
    gyro_bias_dps: tuple[float, float, float]
    moving_floor_dps: float
    turn_start_dps: float


@dataclass
class Turn:
    number: int
    start_index: int
    end_index: int
    duration_s: float
    angle_deg: float
    peak_speed_dps: float
    mean_speed_dps: float


@dataclass(frozen=True)
class TimingStats:
    """Timing-quality measurements derived from consecutive sample timestamps."""

    duration_s: float
    mean_period_s: float
    median_period_s: float
    period_std_s: float
    p95_absolute_jitter_s: float
    minimum_period_s: float
    maximum_period_s: float
    gap_count: int
    estimated_missing_samples: int
    non_monotonic_samples: int

    @property
    def effective_rate_hz(self) -> float:
        return 1.0 / self.mean_period_s if self.mean_period_s > 0 else math.nan


@dataclass(frozen=True)
class MotionProfile:
    """One servo profile derived from a motion-speed and turn-angle percentile."""

    percentile: int
    measured_speed_dps: float
    measured_angle_deg: float
    speed_dps: int
    amplitude_deg: int
    period_ms: int
    name: str

    @property
    def c_initializer(self) -> str:
        return (
            f'{{"{self.name}",{self.amplitude_deg},'
            f'{self.speed_dps:.1f}f,{self.period_ms}}},'
        )


def load_log(path: Path) -> ImuLog:
    """Load either the new microsecond format or the original millisecond format."""
    time_s: list[float] = []
    values = {key: [] for key in VALUE_KEYS}
    rejected_rows = 0
    rtc_datetime: list[str] = []

    with path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        fieldnames = set(reader.fieldnames or ())
        if "timestamp_us" in fieldnames:
            timestamp_key, scale = "timestamp_us", 1_000_000.0
        elif "timestamp_ms" in fieldnames:
            timestamp_key, scale = "timestamp_ms", 1_000.0
        else:
            raise SystemExit("Log has no timestamp_us or timestamp_ms column")

        missing = set(VALUE_KEYS) - fieldnames
        if missing:
            raise SystemExit(f"Log is missing columns: {', '.join(sorted(missing))}")

        for row in reader:
            try:
                timestamp = float(row[timestamp_key]) / scale
                row_values = {key: float(row[key]) for key in VALUE_KEYS}
                if not math.isfinite(timestamp) or not all(
                    math.isfinite(value) for value in row_values.values()
                ):
                    raise ValueError
            except (KeyError, TypeError, ValueError):
                rejected_rows += 1
                continue
            time_s.append(timestamp)
            rtc_datetime.append((row.get("rtc_datetime") or "").strip())
            for key, value in row_values.items():
                values[key].append(value)

    if not time_s:
        raise SystemExit("No valid sensor rows found")
    return ImuLog(time_s, values, rejected_rows, rtc_datetime)


def percentile(values: list[float], percentage: float) -> float:
    """Return a linearly interpolated percentile using the inclusive endpoints."""
    if not values:
        raise ValueError("percentile requires at least one value")
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * percentage / 100.0
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def calculate_timing_stats(log: ImuLog) -> TimingStats:
    """Summarize timestamp rate, jitter, gaps, and ordering for one recording."""
    deltas = [later - earlier for earlier, later in zip(log.time_s, log.time_s[1:])]
    positive_deltas = [delta for delta in deltas if delta > 0]
    duration = log.time_s[-1] - log.time_s[0] if len(log.time_s) > 1 else 0.0
    if not positive_deltas:
        return TimingStats(
            duration, math.nan, math.nan, math.nan, math.nan, math.nan, math.nan,
            0, 0, len(deltas),
        )

    median_period = statistics.median(positive_deltas)
    gaps = [delta for delta in positive_deltas if delta > median_period * 1.5]
    jitter = [abs(delta - median_period) for delta in positive_deltas]
    return TimingStats(
        duration_s=duration,
        mean_period_s=statistics.fmean(positive_deltas),
        median_period_s=median_period,
        period_std_s=(
            statistics.pstdev(positive_deltas) if len(positive_deltas) > 1 else 0.0
        ),
        p95_absolute_jitter_s=percentile(jitter, 95),
        minimum_period_s=min(positive_deltas),
        maximum_period_s=max(positive_deltas),
        gap_count=len(gaps),
        estimated_missing_samples=sum(
            max(0, round(delta / median_period) - 1) for delta in gaps
        ),
        non_monotonic_samples=len(deltas) - len(positive_deltas),
    )


def vector_magnitudes(log: ImuLog, prefix: str, suffix: str) -> list[float]:
    x = log.values[f"{prefix}_x_{suffix}"]
    y = log.values[f"{prefix}_y_{suffix}"]
    z = log.values[f"{prefix}_z_{suffix}"]
    return [math.sqrt(a * a + b * b + c * c) for a, b, c in zip(x, y, z)]


def print_summary(path: Path, log: ImuLog) -> None:
    count = len(log.time_s)
    timing = calculate_timing_stats(log)

    print(f"file: {path}")
    print(f"samples: {count}")
    print(f"rejected rows: {log.rejected_rows}")
    print(f"duration: {timing.duration_s:.6f} s")

    if math.isfinite(timing.mean_period_s):
        print("\nsample timing")
        print(f"effective rate: {timing.effective_rate_hz:.3f} Hz")
        print(
            f"period mean/median: {timing.mean_period_s * 1000:.4f} / "
            f"{timing.median_period_s * 1000:.4f} ms"
        )
        print(
            f"period min/max: {timing.minimum_period_s * 1000:.4f} / "
            f"{timing.maximum_period_s * 1000:.4f} ms"
        )
        print(f"period standard deviation: {timing.period_std_s * 1000:.4f} ms")
        print(
            "95th percentile absolute jitter: "
            f"{timing.p95_absolute_jitter_s * 1000:.4f} ms"
        )
        print(f"gaps > 1.5 periods: {timing.gap_count}")
        print(f"estimated missing samples: {timing.estimated_missing_samples}")
        print(
            "non-monotonic/duplicate timestamps: "
            f"{timing.non_monotonic_samples}"
        )

    for title, prefix, suffix, unit in (
        ("accelerometer", "accel", "g", "g"),
        ("gyroscope", "gyro", "dps", "degrees/second"),
    ):
        print(f"\n{title} ({unit})")
        print("axis   average       minimum       maximum       rms")
        for axis in AXES:
            series = log.values[f"{prefix}_{axis}_{suffix}"]
            rms = math.sqrt(statistics.fmean(value * value for value in series))
            print(
                f"{axis}   {statistics.fmean(series): .6f}   {min(series): .6f}   "
                f"{max(series): .6f}   {rms: .6f}"
            )
        magnitude = vector_magnitudes(log, prefix, suffix)
        print(
            f"|v| {statistics.fmean(magnitude): .6f}   {min(magnitude): .6f}   "
            f"{max(magnitude): .6f}"
        )


def create_plot(path: Path, log: ImuLog, output: Path | None = None) -> Path:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as exc:
        raise SystemExit(
            "Plotting requires matplotlib. Install it with: "
            "python -m pip install -r requirements-gui.txt"
        ) from exc

    output = output or path.with_name(f"{path.stem}_plot.png")
    relative_time = [value - log.time_s[0] for value in log.time_s]
    deltas_ms = [
        (later - earlier) * 1000.0
        for earlier, later in zip(log.time_s, log.time_s[1:])
    ]

    figure, axes = plt.subplots(3, 1, figsize=(12, 9), constrained_layout=True)
    for axis in AXES:
        axes[0].plot(
            relative_time, log.values[f"accel_{axis}_g"], label=axis.upper(), linewidth=0.8
        )
    axes[0].set(title="Acceleration", ylabel="g")
    axes[0].legend(loc="upper right", ncol=3)
    axes[0].grid(alpha=0.25)

    for axis in AXES:
        axes[1].plot(
            relative_time, log.values[f"gyro_{axis}_dps"], label=axis.upper(), linewidth=0.8
        )
    axes[1].set(title="Angular rate", ylabel="degrees/second")
    axes[1].legend(loc="upper right", ncol=3)
    axes[1].grid(alpha=0.25)

    axes[2].plot(relative_time[1:], deltas_ms, linewidth=0.7)
    if deltas_ms:
        axes[2].axhline(
            statistics.median(deltas_ms),
            color="tab:red",
            linestyle="--",
            label="median period",
        )
        axes[2].legend(loc="upper right")
    axes[2].set(title="Sample timing", xlabel="Time since start (s)", ylabel="Period (ms)")
    axes[2].grid(alpha=0.25)

    figure.suptitle(path.name)
    figure.savefig(output, dpi=150)
    plt.close(figure)
    return output


def _median_vector(values: dict[str, list[float]], prefix: str, suffix: str,
                   indices: list[int]) -> tuple[float, float, float]:
    return tuple(
        statistics.median(values[f"{prefix}_{axis}_{suffix}"][index] for index in indices)
        for axis in AXES
    )


def calculate_body_motion(log: ImuLog, calibration_seconds: float = 10.0) -> BodyMotion:
    """Project chest-mounted gyro motion onto the accelerometer-derived vertical axis."""
    relative = [value - log.time_s[0] for value in log.time_s]
    calibration = [index for index, value in enumerate(relative) if value <= calibration_seconds]
    if len(calibration) < 10:
        calibration = list(range(min(len(relative), 100)))

    bias = _median_vector(log.values, "gyro", "dps", calibration)
    gravity = list(_median_vector(log.values, "accel", "g", calibration))
    gravity_norm = math.sqrt(sum(value * value for value in gravity))
    if gravity_norm < 0.25:
        raise SystemExit("Cannot estimate vertical axis: accelerometer magnitude is too small")
    gravity = [value / gravity_norm for value in gravity]

    horizontal: list[float] = []
    previous_time = relative[0]
    for index, current_time in enumerate(relative):
        dt = max(0.0, current_time - previous_time)
        previous_time = current_time
        acceleration = [log.values[f"accel_{axis}_g"][index] for axis in AXES]
        magnitude = math.sqrt(sum(value * value for value in acceleration))
        if 0.75 <= magnitude <= 1.25:
            alpha = 1.0 - math.exp(-dt / 1.5) if dt > 0 else 0.0
            gravity = [
                (1.0 - alpha) * old + alpha * new
                for old, new in zip(gravity, acceleration)
            ]
            norm = math.sqrt(sum(value * value for value in gravity))
            if norm > 0.25:
                gravity = [value / norm for value in gravity]
        corrected_gyro = [
            log.values[f"gyro_{axis}_dps"][index] - bias[axis_index]
            for axis_index, axis in enumerate(AXES)
        ]
        horizontal.append(sum(rate * vertical for rate, vertical in zip(corrected_gyro, gravity)))

    calibration_speed = [abs(horizontal[index]) for index in calibration]
    noise_99 = percentile(calibration_speed, 99) if calibration_speed else 0.0
    moving_floor = max(5.0, noise_99 * 3.0)
    turn_start = max(12.0, noise_99 * 4.0)
    return BodyMotion(relative, horizontal, [abs(value) for value in horizontal],
                      bias, moving_floor, turn_start)


def detect_turns(motion: BodyMotion) -> list[Turn]:
    """Detect sustained horizontal turns with hysteresis and short-gap merging."""
    if len(motion.relative_time_s) < 2:
        return []
    sample_period = statistics.median(
        later - earlier
        for earlier, later in zip(motion.relative_time_s, motion.relative_time_s[1:])
        if later > earlier
    )
    window = max(1, round(0.05 / sample_period))
    smoothed: list[float] = []
    running = 0.0
    for index, value in enumerate(motion.horizontal_rate_dps):
        running += value
        if index >= window:
            running -= motion.horizontal_rate_dps[index - window]
        smoothed.append(running / min(index + 1, window))

    exit_threshold = motion.turn_start_dps * 0.5
    segments: list[tuple[int, int]] = []
    start: int | None = None
    for index, value in enumerate(smoothed):
        if start is None and abs(value) >= motion.turn_start_dps:
            start = max(0, index - 1)
        elif start is not None and abs(value) <= exit_threshold:
            segments.append((start, index))
            start = None
    if start is not None:
        segments.append((start, len(smoothed) - 1))

    merged: list[tuple[int, int]] = []
    for segment in segments:
        segment_sign = 1 if sum(smoothed[segment[0]:segment[1] + 1]) >= 0 else -1
        previous_sign = (
            1 if merged and sum(smoothed[merged[-1][0]:merged[-1][1] + 1]) >= 0 else -1
        )
        if (merged and segment_sign == previous_sign and
                motion.relative_time_s[segment[0]] -
                motion.relative_time_s[merged[-1][1]] <= 0.20):
            merged[-1] = (merged[-1][0], segment[1])
        else:
            merged.append(segment)

    turns: list[Turn] = []
    for start_index, end_index in merged:
        duration = motion.relative_time_s[end_index] - motion.relative_time_s[start_index]
        angle = 0.0
        for index in range(start_index + 1, end_index + 1):
            dt = motion.relative_time_s[index] - motion.relative_time_s[index - 1]
            angle += 0.5 * (smoothed[index - 1] + smoothed[index]) * dt
        speeds = [abs(value) for value in smoothed[start_index:end_index + 1]]
        if duration < 0.25 or abs(angle) < 8.0 or not speeds:
            continue
        turns.append(Turn(len(turns) + 1, start_index, end_index, duration, angle,
                          max(speeds), statistics.fmean(speeds)))
    return turns


BODY_PERCENTILES = (25, 50, 75, 90, 95, 99)


def rounded_profile_value(value: float, minimum: int, maximum: int) -> int:
    return max(minimum, min(maximum, int(round(value / 5.0) * 5)))


def recommend_motion_profiles(
    moving_speeds: list[float],
    turn_angles: list[float],
    name_prefix: str = "body",
) -> list[MotionProfile]:
    """Convert pooled motion percentiles into servo amplitudes and periods."""
    if not moving_speeds:
        raise ValueError("profile recommendations require motion-speed samples")

    profiles: list[MotionProfile] = []
    for percentage in (25, 50, 75, 95):
        measured_speed = percentile(moving_speeds, percentage)
        measured_angle = percentile(turn_angles, percentage) if turn_angles else 60.0
        speed = rounded_profile_value(measured_speed, 5, 500)
        amplitude = rounded_profile_value(measured_angle / 2.0, 5, 60)
        period_ms = max(
            400,
            min(60000, round(2.0 * math.pi * amplitude / speed * 1000.0)),
        )
        profiles.append(
            MotionProfile(
                percentage,
                measured_speed,
                measured_angle,
                speed,
                amplitude,
                period_ms,
                f"{name_prefix}_p{percentage:02d}_a{amplitude:02d}",
            )
        )
    return profiles


def write_turns_csv(path: Path, log: ImuLog, motion: BodyMotion,
                    turns: list[Turn]) -> Path:
    output = path.with_name(f"{path.stem}_detected_turns.csv")
    with output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("turn", "start_s", "end_s", "start_rtc", "end_rtc",
                         "duration_s", "direction", "angle_deg",
                         "peak_speed_deg_s", "mean_speed_deg_s"))
        for turn in turns:
            writer.writerow((
                turn.number,
                f"{motion.relative_time_s[turn.start_index]:.6f}",
                f"{motion.relative_time_s[turn.end_index]:.6f}",
                log.rtc_datetime[turn.start_index],
                log.rtc_datetime[turn.end_index],
                f"{turn.duration_s:.6f}",
                "positive" if turn.angle_deg >= 0 else "negative",
                f"{turn.angle_deg:.3f}",
                f"{turn.peak_speed_dps:.3f}",
                f"{turn.mean_speed_dps:.3f}",
            ))
    return output


def write_blur_profiles(path: Path, motion: BodyMotion, turns: list[Turn]) -> Path:
    """Write servo-ready profiles derived from the recording's moving samples."""
    output = path.with_name(f"{path.stem}_body_motion_summary.csv")
    moving_speeds = [
        value for value in motion.absolute_rate_dps if value >= motion.moving_floor_dps
    ]
    if len(moving_speeds) < 20:
        moving_speeds = motion.absolute_rate_dps
    turn_angles = [abs(turn.angle_deg) for turn in turns]
    with output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow((
            "profile_name", "speed_percentile", "measured_speed_deg_s",
            "turn_angle_percentile", "measured_turn_angle_deg",
            "recommended_amplitude_deg", "recommended_period_ms", "c_initializer",
        ))
        for profile in recommend_motion_profiles(moving_speeds, turn_angles):
            writer.writerow((
                profile.name,
                profile.percentile,
                f"{profile.measured_speed_dps:.3f}",
                profile.percentile,
                f"{profile.measured_angle_deg:.3f}",
                profile.amplitude_deg,
                profile.period_ms,
                profile.c_initializer,
            ))
    return output


def create_body_motion_plot(path: Path, motion: BodyMotion, turns: list[Turn],
                            output: Path | None = None) -> Path:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as exc:
        raise SystemExit("Body-motion plotting requires matplotlib") from exc

    output = output or path.with_name(f"{path.stem}_body_motion.png")
    figure, axes = plt.subplots(2, 2, figsize=(14, 9), constrained_layout=True)
    signed_axis, absolute_axis, histogram_axis, table_axis = axes.flat
    signed_axis.plot(motion.relative_time_s, motion.horizontal_rate_dps, linewidth=0.8)
    signed_axis.axhline(0, color="black", linewidth=0.6)
    for turn in turns:
        signed_axis.axvspan(motion.relative_time_s[turn.start_index],
                            motion.relative_time_s[turn.end_index], alpha=0.12,
                            color="tab:orange")
    signed_axis.set(title="Signed horizontal angular velocity (detected turns shaded)",
                    xlabel="Time (s)", ylabel="degrees/second")
    signed_axis.grid(alpha=0.25)

    absolute_axis.plot(motion.relative_time_s, motion.absolute_rate_dps,
                       linewidth=0.8, color="tab:purple")
    absolute_axis.axhline(motion.moving_floor_dps, color="tab:red", linestyle="--",
                          label=f"movement floor {motion.moving_floor_dps:.1f} deg/s")
    absolute_axis.set(title="Absolute horizontal speed", xlabel="Time (s)",
                      ylabel="degrees/second")
    absolute_axis.legend()
    absolute_axis.grid(alpha=0.25)

    histogram_axis.hist(motion.absolute_rate_dps, bins=50, color="tab:blue", alpha=0.8)
    for percentage in (50, 75, 95):
        value = percentile(motion.absolute_rate_dps, percentage)
        histogram_axis.axvline(value, linestyle="--", label=f"P{percentage} {value:.1f}")
    histogram_axis.set(title="Horizontal speed histogram", xlabel="degrees/second",
                       ylabel="Samples")
    histogram_axis.legend()
    histogram_axis.grid(alpha=0.2)

    percentile_rows = [
        (f"P{percentage}", f"{percentile(motion.absolute_rate_dps, percentage):.2f}")
        for percentage in BODY_PERCENTILES
    ]
    percentile_rows.append(("Maximum", f"{max(motion.absolute_rate_dps):.2f}"))
    table_axis.axis("off")
    table_axis.set_title(f"Horizontal speed percentiles (deg/s) — {len(turns)} turns")
    table_axis.table(cellText=percentile_rows, colLabels=("Statistic", "deg/s"),
                     loc="center", cellLoc="center")
    figure.suptitle(f"Chest-worn body motion: {path.name}")
    figure.savefig(output, dpi=150)
    plt.close(figure)
    return output


def analyze_body_motion(path: Path, log: ImuLog, calibration_seconds: float = 10.0) -> tuple[Path, Path, Path]:
    motion = calculate_body_motion(log, calibration_seconds)
    turns = detect_turns(motion)
    print("\nchest-worn horizontal motion")
    print("gyro bias (x/y/z): " + ", ".join(f"{value:.4f}" for value in motion.gyro_bias_dps) + " deg/s")
    print(f"movement floor: {motion.moving_floor_dps:.3f} deg/s")
    print("percentile     horizontal speed (deg/s)")
    for percentage in BODY_PERCENTILES:
        print(f"P{percentage:<2}            {percentile(motion.absolute_rate_dps, percentage):9.3f}")
    print(f"maximum        {max(motion.absolute_rate_dps):9.3f}")
    print(f"detected turns: {len(turns)}")
    for turn in turns:
        print(f"  turn {turn.number:02d}: {turn.duration_s:.2f} s, "
              f"angle {turn.angle_deg:+.1f} deg, peak {turn.peak_speed_dps:.1f} deg/s")
    plot = create_body_motion_plot(path, motion, turns)
    turns_csv = write_turns_csv(path, log, motion, turns)
    summary_csv = write_blur_profiles(path, motion, turns)
    print(f"body-motion plot: {plot}")
    print(f"detected turns CSV: {turns_csv}")
    print(f"Blur Test profile CSV: {summary_csv}")
    return plot, turns_csv, summary_csv


def analyze(path: Path, make_plot: bool = True, plot_path: Path | None = None,
            body_motion: bool = False, calibration_seconds: float = 10.0) -> Path | None:
    log = load_log(path)
    print_summary(path, log)
    if body_motion:
        analyze_body_motion(path, log, calibration_seconds)
    if not make_plot:
        return None
    output = create_plot(path, log, plot_path)
    print(f"\nplot: {output}")
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Analyze BMI323 CSV logs")
    parser.add_argument("log_file", type=Path, help="Path to an IMU run CSV")
    parser.add_argument("--no-plot", action="store_true", help="Only print statistics")
    parser.add_argument("--plot", type=Path, help="Output path for the PNG plot")
    parser.add_argument("--body-motion", action="store_true",
                        help="Analyze chest-worn horizontal rotation and recommend Blur Test profiles")
    parser.add_argument("--calibration-seconds", type=float, default=10.0,
                        help="Initial stationary interval used for gyro bias (default: 10)")
    arguments = parser.parse_args()
    analyze(arguments.log_file, not arguments.no_plot, arguments.plot,
            arguments.body_motion, arguments.calibration_seconds)
