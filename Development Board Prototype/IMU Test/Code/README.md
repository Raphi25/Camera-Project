# BMI323 IMU Test

ESP-IDF project for the FireBeetle 2 ESP32-P4 and DFRobot Fermion BMI323.

## Wiring

- BMI323 SDA -> GPIO20
- BMI323 SCL -> GPIO36
- BMI323 INT1 -> GPIO22
- BMI323 INT2 -> GPIO21 (reserved; the current logger does not use INT2)
- BMI323 3V3 -> 3.3 V
- BMI323 GND -> GND
- DS3231 SDA -> GPIO7
- DS3231 SCL -> GPIO8
- DS3231 VCC -> 3.3 V
- DS3231 GND -> GND

The firmware probes I2C addresses 0x69 and 0x68, configures accelerometer and
gyroscope data at 100 Hz, and records each accelerometer data-ready event from
BMI323 INT1.

The SDMMC pin map follows the prototype board: CLK GPIO43, CMD GPIO44, D0 GPIO39, D1 GPIO40, D2 GPIO41, D3 GPIO42.

## Firmware use

Build and flash with the ESP-IDF terminal:

```text
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

Logging starts automatically after BMI323, RTC, and SD initialization. Every
start creates a new RTC-named file:

```text
/sdcard/captures/imu_20260918_143205_00.csv
/sdcard/captures/imu_20260918_151022_00.csv
```

If power is interrupted, the battery-backed DS3231 retains the clock. On the
next boot the logger automatically starts a fresh timestamped file, preserving
the previous run instead of appending to a potentially interrupted file. If the
RTC has lost its time, logging continues with a legacy numbered filename until
the GUI synchronizes the clock.

Rows are buffered in RAM and flushed to the SD card every second (or 100
samples). Stop logging before removing the card so the final buffer is written.

The USB serial monitor accepts:

```text
IMU_LOG_START
IMU_LOG_STOP
LOG_LIST
LOG_GET imu_run_000001.csv
LOG_DELETE_ALL
RTC_GET
RTC_SET 2026-09-18 14:32:05
STATUS
```

`LOG_LIST` lists completed CSV recordings and their sizes. `LOG_GET` transfers
one CSV over the same USB serial connection using framed lines, with byte-count
and CRC-32 verification on the PC.
`LOG_DELETE_ALL` removes only recognized IMU CSV files in the capture directory
and reports how many deletions succeeded or failed.

STATUS reports the current path and counters for samples, interrupts, estimated
missed samples, invalid sensor data, I2C errors, SD write errors, interrupt
timeouts, and buffer flushes.

Each CSV row contains its sample index, RTC-derived wall-clock timestamp in
microseconds, readable RTC date/time, acceleration in g, and gyroscope rate in
degrees per second. Monotonic timing supplies sub-second precision between RTC
readings.

## Analysis

Copy an imu_run_*.csv file from the SD card and run:

```text
python analyze_imu.py path\to\imu_run_000001.csv
```

The script reports sample rate, period jitter, timing gaps, estimated missing
samples, and per-axis average/minimum/maximum/RMS values. It also saves a
three-panel acceleration, gyroscope, and sample-period plot next to the CSV.
Use --no-plot for text-only analysis.

For the chest-worn blur experiment, remain still for the first 10 seconds and
run:

```text
python analyze_imu.py path\to\imu_20260918_143205_00.csv --body-motion
```

Body-motion mode estimates the chest's vertical axis from gravity, removes the
initial stationary gyro bias, and projects angular velocity onto that vertical
axis. It produces:

- `*_body_motion.png`: signed/absolute horizontal speed, detected turns,
  histogram, and percentile table.
- `*_detected_turns.csv`: timing, direction, angle, peak speed, and mean speed
  for every detected turn.
- `*_body_motion_summary.csv`: slow, typical, fast, and very-fast servo profile
  recommendations. Its `c_initializer` column can be pasted into the Blur Test
  Breadboard `motion_profile_t` array.

Recommended amplitudes are half of the measured turn angle and are limited to
the Blur Test servo's current +/-60 degree range. Recommended cycle periods use
the same `T = 2*pi*A / peak_velocity` relationship as the Blur Test firmware.

To combine every raw recording under `Data` (including `Test1`, `Test2`, and
other subfolders), run:

```text
python average_imu_runs.py Data
```

This writes `Data/imu_run_averages.csv`, containing one row per run plus an
arithmetic-average row, and `Data/imu_combined_body_motion_summary.csv`, which
contains pooled Blur Test profile recommendations. It also creates
`Data/imu_run_averages.png` with run comparisons, pooled motion distributions,
and turn counts. Generated analysis CSVs are excluded automatically, so the
command can be rerun when more tests are added. Use `--no-plot` for CSV-only
analysis or `--plot-output path.png` to choose another image location.

## Quick PC GUI

Install the serial and plotting dependencies once:

```text
python -m pip install -r requirements-gui.txt
```

Start the GUI:

```text
python imu_gui.py
```

Click **Start Logging** to close any active run, synchronize the DS3231 to the
PC's local clock, and start a fresh recording. **Stop Logging** flushes and
closes the active recording.

Only the serial/programming USB connection is required; USB OTG Mass Storage is
not used. Click **Download All**. The GUI stops logging so the active CSV is
closed, downloads every IMU CSV on the SD card, verifies each file's size and
CRC-32, and saves them in the project's `Data` folder. The newest downloaded
file is selected for analysis. Click **Analyze Body Motion** to generate the
timing report, chest-motion plots, detected turns, and Blur Test profile CSV.

The GUI's **Delete All Logs** button asks for confirmation, stops logging, and
then removes all IMU run CSV files from the SD card. Downloaded PC files are not
affected.
