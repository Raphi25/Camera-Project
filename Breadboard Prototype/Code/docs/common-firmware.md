# Common prototype firmware

Development Board Prototype 1 and Breadboard Prototype use the same firmware
sources, sensor drivers, dependency versions and build configuration. Each
project keeps its existing GPIO and bus assignments. The desktop application
is shared in the sibling `App` folder.

| Setting | Both projects |
| --- | --- |
| Flash encryption | Disabled in `sdkconfig` and `sdkconfig.defaults` |
| Partition table | `0xc000` |
| NVS / PHY | `0xd000` / `0x13000` |
| Application / core dump | `0x20000` / `0x197000` |
| Camera output before JPEG | RGB888 |
| Camera warm-up | 10 frames |
| Normal / low-light mode | 30 / 25 FPS |
| JPEG quality | 90 |
| Capture completion | Wait for capture/encoding and all pending save jobs |
| Orientation journals | Retained across repeated daily-summary writes |
| Periodic ISP exposure/gain/white-balance diagnostics | Removed |
| Scene classification | RGB888, RGB565 and UYVY; last result exposed through STATUS and the shared App |

Scene analysis samples every eighth RGB pixel and uses integer BT.601 luma.
Two consecutive dark or bright captures are needed to change the classification;
the hysteresis counters saturate to avoid wraparound during long runs. Both
single and burst captures publish a synchronized snapshot. STATUS reports
`scene=light|dark|unknown`, `scene_avg_luma` (0-255) and `scene_dark_pct` (0-100).
The shared App displays the last captured scene, brightness and dark-pixel
percentage. Before a capture, scene is unknown. This is diagnostic information;
it does not automatically change camera mode or frame rate.

The `0xc000` layout reserves extra bootloader room without changing the
application or core-dump addresses. RGB888 preserves eight bits per color
channel before JPEG encoding; it uses three bytes per pixel versus RGB565's
two bytes. This favors image color detail over reduced frame-buffer traffic.

SC2336 uses its existing 1920x1080 30/25 FPS modes. OV5647 uses 1280x960
binning with its existing PLL and line timing, changing vertical frame length
to 1640 lines for approximately 30 FPS and 1967 lines for approximately 25 FPS.
The ISP receives matching frame-length metadata. These new OV5647 timings
need verification on physical cameras.

The Arducam IMX500 module exposes frame rate through its control protocol.
The driver checks the module's range and step, applies 30 or 25 FPS and verifies
readback. Unsupported or clamped rates produce an error, rather than reporting
a rate the hardware did not apply. Actual module support needs hardware testing.

Saved camera values from the former 45/15 FPS configuration map to 30/25 FPS.
New selections store 30/25 FPS. A saved 25 FPS value remains valid after reboot.

An atomic pending-save count includes queued jobs and jobs currently being
written, through completion of orientation logging and metrics callbacks.
Stop/export/sleep also wait for acquisition and encoding under the capture
mutex. A burst already holding that mutex only drains its pending save jobs.
This avoids treating the SD card as idle between dequeue and writer completion.

Daily orientation journals remain after summary generation so subsequent reports
include earlier images. Summary listings expose report files only;
`SUMMARY_DELETE_ALL` deletes reports and orientation journals together.

## Building and flashing

Use ESP-IDF 6.0.2. Both projects' VS Code tasks use regular `flash`, with no
`encrypted-flash` task. No flashing or eFuse programming is part of source
alignment.

Disabling the build setting does not disable encryption already enabled in a
physical board's eFuses. Verify the board's state before installing plaintext
firmware. A previously encrypted board needs a separate hardware transition;
the project does not perform that transition automatically.

The development-board NVS address changed from `0xa000` to `0xd000`. Flashing
the new layout does not migrate existing device settings or credentials at the
old address. The breadboard NVS address is unchanged.
Before flashing the development board, export/decrypt existing SD media and
back up its device credentials. If the old credentials are absent at the new
NVS location, the firmware generates new media/log passwords; those new
passwords cannot decrypt files saved using the old ones.

Host checks use production C functions with filesystem/RTOS/control stubs:

```
python tests/run_summary_regressions.py
python tests/run_camera_mode_regressions.py
```

They require Visual Studio Build Tools on Windows. They do not verify camera
image quality, physical FPS, SD-card behavior or hardware eFuse state.
