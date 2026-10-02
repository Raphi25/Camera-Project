# TC66 Grapher

Graphs `Current (A)` over `Time` from TC66 CSV or Excel recordings. It also
creates a text report containing the minimum, maximum,
average, total current drawn, projected consumption per 24 hours, and separate
time-weighted average currents while the camera is active and asleep. CSV,
XLSX, XLSM, and legacy XLS files are supported.

Camera state is determined from the configured daily schedule. By default, the
camera is asleep from 23:00 until 08:00 and active from 08:00 until 23:00. To
use a different schedule:

```powershell
.\.venv\Scripts\python.exe plot_tc66.py recording.csv --sleep-start 22:30 --sleep-end 07:30
```

If a sampling interval crosses a scheduled transition, the calculation splits
that interval at the transition. The report shows the measured duration of
each state alongside its average current.

The graph labels every hour on the X-axis.

The recording is automatically cut off at battery shutdown. Shutdown is
detected when voltage is at or below 0.05 V and current is at or below 0.001 A
for 10 consecutive samples. Once confirmed, the cutoff moves back through the
immediately preceding zero-current samples to the first shutdown sample. The
shutdown samples and all later data are excluded from both the graph and
summary statistics.

## Setup

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

## Run

With no arguments, the newest recording in `Excel files` is used:

```powershell
.\.venv\Scripts\python.exe plot_tc66.py
```

Or choose a file and output path:

```powershell
.\.venv\Scripts\python.exe plot_tc66.py "Excel files\2026-08-20.csv" --output tc66_graph.png
```

The graph defaults to `<input>_graph.png` and the report defaults to
`<input>_summary.txt`. Choose a different report path with `--summary`.

For an Excel workbook, the first sheet is used by default. Select another sheet
by name or zero-based index:

```powershell
.\.venv\Scripts\python.exe plot_tc66.py recording.xlsx --sheet Measurements
```

The input must contain the columns `Time`, `Voltage (V)`, and `Current (A)`.
