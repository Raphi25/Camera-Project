#!/usr/bin/env python3
"""Aggregate multiple BMI323 recordings into repeatable experiment statistics."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from pathlib import Path

from analyze_imu import (
    BODY_PERCENTILES,
    Turn,
    calculate_body_motion,
    calculate_timing_stats,
    detect_turns,
    load_log,
    percentile,
    recommend_motion_profiles,
)


RAW_EXCLUSIONS = ("_body_motion_summary.csv", "_detected_turns.csv")
PROFILE_PERCENTILES = (25, 50, 75, 95)


def find_raw_logs(data_directory: Path) -> list[Path]:
    """Find raw IMU CSV files while excluding generated analysis CSVs."""
    candidates = sorted(
        path
        for path in data_directory.rglob("*.csv")
        if not path.name.lower().endswith(RAW_EXCLUSIONS)
        and path.name.lower()
        not in {"imu_run_averages.csv", "imu_combined_body_motion_summary.csv"}
    )
    logs: list[Path] = []
    for path in candidates:
        try:
            with path.open("r", encoding="utf-8-sig", newline="") as stream:
                header = next(csv.reader(stream), [])
        except (OSError, UnicodeError):
            continue
        fields = set(header)
        if ({"timestamp_us", "timestamp_ms"} & fields) and {
            "accel_x_g", "accel_y_g", "accel_z_g",
            "gyro_x_dps", "gyro_y_dps", "gyro_z_dps",
        }.issubset(fields):
            logs.append(path)
    return logs


def value_percentiles(
    values: list[float], percentages: tuple[int, ...]
) -> dict[int, float]:
    if not values:
        return {percentage: math.nan for percentage in percentages}
    return {percentage: percentile(values, percentage) for percentage in percentages}


def analyze_run(
    path: Path, data_directory: Path, calibration_seconds: float
) -> tuple[dict[str, object], list[float], list[float], list[Turn]]:
    """Calculate the per-run row and return samples needed for pooled results."""
    log = load_log(path)
    motion = calculate_body_motion(log, calibration_seconds)
    turns = detect_turns(motion)
    timing = calculate_timing_stats(log)

    moving_speeds = [
        speed
        for speed in motion.absolute_rate_dps
        if speed >= motion.moving_floor_dps
    ]
    turn_angles = [abs(turn.angle_deg) for turn in turns]
    turn_peaks = [turn.peak_speed_dps for turn in turns]
    turn_durations = [turn.duration_s for turn in turns]
    all_speed = value_percentiles(motion.absolute_rate_dps, BODY_PERCENTILES)
    moving_speed = value_percentiles(moving_speeds, PROFILE_PERCENTILES)
    angle = value_percentiles(turn_angles, PROFILE_PERCENTILES)
    peak = value_percentiles(turn_peaks, PROFILE_PERCENTILES)
    duration = value_percentiles(turn_durations, PROFILE_PERCENTILES)

    row: dict[str, object] = {
        "run": path.parent.name,
        "file": str(path.relative_to(data_directory)),
        "samples": len(log.time_s),
        "duration_s": timing.duration_s,
        "effective_rate_hz": timing.effective_rate_hz,
        "median_period_ms": timing.median_period_s * 1000.0,
        "period_std_ms": timing.period_std_s * 1000.0,
        "p95_abs_jitter_ms": timing.p95_absolute_jitter_s * 1000.0,
        "timing_gaps": timing.gap_count,
        "estimated_missing_samples": timing.estimated_missing_samples,
        "rejected_rows": log.rejected_rows,
        "movement_floor_deg_s": motion.moving_floor_dps,
        "moving_fraction_pct": 100.0 * len(moving_speeds) / len(motion.absolute_rate_dps),
        "detected_turns": len(turns),
        "positive_turns": sum(turn.angle_deg >= 0 for turn in turns),
        "negative_turns": sum(turn.angle_deg < 0 for turn in turns),
        "maximum_speed_deg_s": max(motion.absolute_rate_dps),
    }
    for percentage in BODY_PERCENTILES:
        row[f"speed_p{percentage}_deg_s"] = all_speed[percentage]
    for percentage in PROFILE_PERCENTILES:
        row[f"moving_speed_p{percentage}_deg_s"] = moving_speed[percentage]
        row[f"turn_angle_p{percentage}_deg"] = angle[percentage]
        row[f"turn_peak_p{percentage}_deg_s"] = peak[percentage]
        row[f"turn_duration_p{percentage}_s"] = duration[percentage]
    return row, motion.absolute_rate_dps, moving_speeds, turns


def average_rows(rows: list[dict[str, object]]) -> dict[str, object]:
    """Build an equally weighted arithmetic mean of the per-run columns."""
    average: dict[str, object] = {"run": "AVERAGE", "file": f"{len(rows)} runs"}
    for key in rows[0]:
        if key in {"run", "file"}:
            continue
        values = [float(row[key]) for row in rows if math.isfinite(float(row[key]))]
        average[key] = statistics.fmean(values) if values else math.nan
    return average


def write_run_statistics(output: Path, rows: list[dict[str, object]]) -> None:
    """Write auditable per-run values followed by one run-average row."""
    output.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = list(rows[0])
    with output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows + [average_rows(rows)]:
            formatted = {}
            for key, value in row.items():
                if isinstance(value, float):
                    formatted[key] = "" if math.isnan(value) else f"{value:.6f}"
                else:
                    formatted[key] = value
            writer.writerow(formatted)


def write_combined_profiles(
    output: Path, moving_speeds: list[float], turns: list[Turn]
) -> None:
    """Create profiles from all moving samples and detected turns together."""
    turn_angles = [abs(turn.angle_deg) for turn in turns]
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow((
            "profile_name", "speed_percentile", "pooled_speed_deg_s",
            "turn_angle_percentile", "pooled_turn_angle_deg",
            "recommended_amplitude_deg", "recommended_speed_deg_s",
            "recommended_period_ms", "c_initializer",
        ))
        for profile in recommend_motion_profiles(
            moving_speeds, turn_angles, name_prefix="body_combined"
        ):
            writer.writerow((
                profile.name,
                profile.percentile,
                f"{profile.measured_speed_dps:.3f}",
                profile.percentile,
                f"{profile.measured_angle_deg:.3f}",
                profile.amplitude_deg,
                profile.speed_dps,
                profile.period_ms,
                profile.c_initializer,
            ))


def create_visualization(
    output: Path,
    rows: list[dict[str, object]],
    moving_speeds: list[float],
    turns: list[Turn],
) -> Path:
    """Plot run-to-run consistency and the pooled profile distributions."""
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as exc:
        raise SystemExit(
            "Plotting requires matplotlib. Install it with: "
            "python -m pip install -r requirements-gui.txt"
        ) from exc

    output.parent.mkdir(parents=True, exist_ok=True)
    figure, axes = plt.subplots(2, 2, figsize=(14, 9), constrained_layout=True)
    speed_axis, moving_axis, turn_count_axis, angle_axis = axes.flat
    run_names = [str(row["run"]) for row in rows]

    # A percentile curve makes run-to-run distribution differences visible.
    for row in rows:
        speed_axis.plot(
            PROFILE_PERCENTILES,
            [float(row[f"speed_p{p}_deg_s"]) for p in PROFILE_PERCENTILES],
            marker="o",
            linewidth=1.8,
            label=str(row["run"]),
        )
    speed_axis.set(
        title="Horizontal speed distribution by run",
        xlabel="Percentile",
        ylabel="Absolute speed (degrees/second)",
        xticks=PROFILE_PERCENTILES,
    )
    speed_axis.legend(title="Run")
    speed_axis.grid(alpha=0.25)

    profile_colors = ("tab:green", "tab:blue", "tab:orange", "tab:red")
    moving_axis.hist(moving_speeds, bins=60, color="tab:blue", alpha=0.75)
    for color, percentage in zip(profile_colors, PROFILE_PERCENTILES):
        value = percentile(moving_speeds, percentage)
        moving_axis.axvline(
            value,
            color=color,
            linestyle="--",
            linewidth=1.5,
            label=f"P{percentage} {value:.1f}",
        )
    moving_axis.set(
        title="Pooled horizontal speed while moving",
        xlabel="Absolute speed (degrees/second)",
        ylabel="Samples",
    )
    moving_axis.legend()
    moving_axis.grid(axis="y", alpha=0.2)

    positions = list(range(len(rows)))
    positive = [int(row["positive_turns"]) for row in rows]
    negative = [int(row["negative_turns"]) for row in rows]
    turn_count_axis.bar(positions, positive, label="Positive", color="tab:blue")
    turn_count_axis.bar(
        positions,
        negative,
        bottom=positive,
        label="Negative",
        color="tab:orange",
    )
    turn_count_axis.set(
        title="Detected turns by direction",
        xlabel="Run",
        ylabel="Turns",
        xticks=positions,
        xticklabels=run_names,
    )
    turn_count_axis.legend()
    turn_count_axis.grid(axis="y", alpha=0.2)

    turn_angles = [abs(turn.angle_deg) for turn in turns]
    angle_axis.hist(turn_angles, bins=35, color="tab:purple", alpha=0.75)
    for color, percentage in zip(profile_colors, PROFILE_PERCENTILES):
        value = percentile(turn_angles, percentage)
        angle_axis.axvline(
            value,
            color=color,
            linestyle="--",
            linewidth=1.5,
            label=f"P{percentage} {value:.1f}°",
        )
    angle_axis.set(
        title="Pooled absolute turn angles",
        xlabel="Turn angle (degrees)",
        ylabel="Turns",
    )
    angle_axis.legend()
    angle_axis.grid(axis="y", alpha=0.2)

    total_duration = sum(float(row["duration_s"]) for row in rows)
    figure.suptitle(
        f"Combined chest-worn IMU analysis — {len(rows)} runs, "
        f"{total_duration:.1f} s, {len(turns)} turns"
    )
    figure.savefig(output, dpi=150)
    plt.close(figure)
    return output


def print_summary(
    rows: list[dict[str, object]],
    all_speeds: list[float],
    moving_speeds: list[float],
    turns: list[Turn],
) -> None:
    print(f"Runs analyzed: {len(rows)}")
    print(f"Total samples: {sum(int(row['samples']) for row in rows)}")
    print(f"Total duration: {sum(float(row['duration_s']) for row in rows):.3f} s")
    print(f"Total detected turns: {len(turns)}")
    print("\nPer-run results")
    print("run       rate (Hz)  moving (%)  turns  P50 speed  P95 speed  maximum")
    for row in rows:
        print(
            f"{str(row['run']):<9} {float(row['effective_rate_hz']):9.3f}  "
            f"{float(row['moving_fraction_pct']):10.2f}  {int(row['detected_turns']):5d}  "
            f"{float(row['speed_p50_deg_s']):9.2f}  "
            f"{float(row['speed_p95_deg_s']):9.2f}  "
            f"{float(row['maximum_speed_deg_s']):7.2f}"
        )

    average = average_rows(rows)
    print("\nArithmetic average of the run-level results")
    print(f"Effective rate: {float(average['effective_rate_hz']):.3f} Hz")
    print(f"Moving fraction: {float(average['moving_fraction_pct']):.2f}%")
    print(f"Detected turns: {float(average['detected_turns']):.2f} per run")
    print(f"P50 horizontal speed: {float(average['speed_p50_deg_s']):.2f} deg/s")
    print(f"P95 horizontal speed: {float(average['speed_p95_deg_s']):.2f} deg/s")

    print("\nPooled measurements from all runs")
    print("percentile  all samples  moving samples  turn angle")
    turn_angles = [abs(turn.angle_deg) for turn in turns]
    for percentage in PROFILE_PERCENTILES:
        print(
            f"P{percentage:<2}       {percentile(all_speeds, percentage):11.2f}  "
            f"{percentile(moving_speeds, percentage):14.2f}  "
            f"{percentile(turn_angles, percentage):10.2f}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Average all raw BMI323 runs in a data directory"
    )
    parser.add_argument("data_directory", nargs="?", type=Path, default=Path("Data"))
    parser.add_argument("--output", type=Path, help="Per-run statistics CSV")
    parser.add_argument(
        "--profiles-output", type=Path, help="Combined Blur Test profiles CSV"
    )
    parser.add_argument("--plot-output", type=Path, help="Combined analysis PNG")
    parser.add_argument("--no-plot", action="store_true", help="Do not create a PNG")
    parser.add_argument("--calibration-seconds", type=float, default=10.0)
    arguments = parser.parse_args()

    data_directory = arguments.data_directory.resolve()
    logs = find_raw_logs(data_directory)
    if not logs:
        parser.error(f"No raw IMU CSV files found under {data_directory}")

    output = arguments.output or data_directory / "imu_run_averages.csv"
    profiles_output = (
        arguments.profiles_output
        or data_directory / "imu_combined_body_motion_summary.csv"
    )
    plot_output = arguments.plot_output or data_directory / "imu_run_averages.png"
    rows: list[dict[str, object]] = []
    all_speeds: list[float] = []
    moving_speeds: list[float] = []
    all_turns: list[Turn] = []
    for path in logs:
        row, run_speeds, run_moving_speeds, turns = analyze_run(
            path, data_directory, arguments.calibration_seconds
        )
        rows.append(row)
        all_speeds.extend(run_speeds)
        moving_speeds.extend(run_moving_speeds)
        all_turns.extend(turns)

    write_run_statistics(output, rows)
    write_combined_profiles(profiles_output, moving_speeds, all_turns)
    plot_path = None
    if not arguments.no_plot:
        plot_path = create_visualization(
            plot_output, rows, moving_speeds, all_turns
        )
    print_summary(rows, all_speeds, moving_speeds, all_turns)
    print(f"\nRun statistics CSV: {output}")
    print(f"Combined profile CSV: {profiles_output}")
    if plot_path is not None:
        print(f"Combined analysis plot: {plot_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
