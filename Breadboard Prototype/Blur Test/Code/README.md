# Motion blur breadboard

This ESP-IDF project measures camera sharpness while a FireBeetle 2 ESP32-P4
rotates an SC2336 or OV5647 camera with an EMAX ES08MDII servo. The firmware
detects the installed sensor at boot, captures plain JPEG files to microSD, and
exports the card through TinyUSB mass storage. Image encryption is not used.

## Hardware

| Connection | Pin or interface |
| --- | --- |
| SC2336 or OV5647 | MIPI CSI connector |
| Camera SCCB | GPIO7 SDA, GPIO8 SCL |
| Servo signal | GPIO32 |
| Start button | GPIO5 to GND; internal pull-up enabled |
| microSD | CLK43, CMD44, D0-D3 on GPIO39-GPIO42, power gate GPIO45, LDO4 |

Power the servo from a suitable external supply and join its ground to the
FireBeetle ground. Do not power the servo from a GPIO or the 3.3 V rail. Power
the board off before changing camera modules.

## Build and flash

The project targets ESP-IDF 6.0.2 and an ESP32-P4 board that already has
development flash encryption enabled. From an ESP-IDF terminal:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
idf.py -B build -p COM5 encrypted-flash
esptool --chip esp32p4 -p COM5 run
idf.py -B build -p COM5 monitor
```

Replace `COM5` with the board's port. If the monitor repeatedly reports an
invalid image header, flash the complete encrypted image again; do not erase
flash or change eFuses as a routine recovery step.

At boot, both sensor drivers probe their chip IDs. A failed probe for the absent
sensor is expected before the installed sensor is found. The configured output
is 1920 x 1080 for SC2336 and 1280 x 960 for OV5647, converted to RGB565 by the
ISP and encoded to JPEG at quality 90.

## Experiment sequence

The firmware waits for a debounced change on GPIO5. While it is waiting, enter
`mode normal` or `mode burst` in the monitor. Burst is the default. Press and
hold the physical button to begin.

Each run performs three complete trials. A trial contains:

1. Stationary baseline: 10 saved images.
2. 10 degrees/s at +/-30 degrees: 10 saved images.
3. 25 degrees/s at +/-30 degrees: 10 saved images.
4. 50 degrees/s at +/-30 degrees: 10 saved images.
5. 100 degrees/s at +/-30 degrees: 10 saved images.
6. 150 degrees/s at +/-30 degrees: 10 saved images.

Captures start every three seconds. Normal mode takes one exposure. Burst mode
takes three consecutive exposures, calculates a raw-frame detail score, and
saves only the sharpest candidate. Both modes therefore produce 180 JPEG files
for a complete experiment.

Before each still, capture restarts the camera stream and discards five frames
for automatic exposure and white-balance settling. It then stops CSI before
using the JPEG engine, which avoids the early ESP32-P4 CSI/JPEG DMA conflict.
The JPEG is copied to PSRAM and queued to a dedicated SD writer task, allowing
camera control to continue while FAT writes complete.

Every JPEG has a `.jpg.csv` sidecar containing commanded amplitude and angular
velocity, assumed exposure time, focal length in pixels, expected blur in
pixels, frame timestamp, and dimensions. Expected blur uses
`angular_velocity_rad_s * exposure_s * focal_px`; it is a prediction based on
the commanded motion and assumed 1/60 s exposure, not a measured blur width.

Files are stored in a new `/blur/run_00000`, `/blur/run_00001`, and so on for
each boot. Temporary files are renamed only after a complete write.

## Console commands

Commands are newline terminated:

| Command | Action |
| --- | --- |
| `mode normal` | Save one exposure per scheduled capture |
| `mode burst` | Capture three frames and save the sharpest |
| `snap` | Save one still at the current servo state |
| `servo 1500` | Hold a pulse width from 1000 to 2000 microseconds |
| `sweep 1300 1700 2000` | Sweep between pulse widths; third value is the cycle time in ms |
| `test 5` | Save one baseline and five moving test images |
| `stop` | Stop the experiment and disable servo pulses |
| `USB_MSC_START` | Give the SD card to the OTG USB host |
| `USB_MSC_STOP` | Return the safely dismounted card to the firmware |
| `help` | Print the command list |

## Retrieve media

Connect both board USB-C ports with data cables: USB Serial/JTAG for COM control
and native OTG for the exported SD volume. Close the serial monitor, then run:

```powershell
.\Launch_GUI.cmd
```

If needed, install `gui/requirements.txt`. **Retrieve Media** asks the firmware
to export the card, finds it using `BLUR_DEVICE.TXT`, copies JPEG and CSV files,
verifies the source and destination with SHA-256, and writes a transfer
manifest under `retrieved_images`. **Delete Media** removes only recognized
experiment JPEG/CSV files after confirmation. Close Explorer windows and image
previews for the exported drive so Windows can lock and dismount it safely.

## Analyze and process images

All three scripts select the newest download when the folder argument is
omitted:

```powershell
python analyze_blur.py
python cleanup_images.py
python deblur_images.py --regularization 0.10
```

`analyze_blur.py` writes `blur_analysis.csv` and `blur_analysis.png`. It reports
Laplacian variance, Tenengrad energy, and sharpness normalized to the stationary
baseline. Use the unprocessed images for experimental results.

`cleanup_images.py` applies mild denoising and unsharp masking.
`deblur_images.py` applies a horizontal Wiener deconvolution using the expected
blur length from each sidecar. These processed copies can improve appearance,
but sharpening and deconvolution can raise a sharpness score without recovering
true scene detail, so they should be evaluated separately from raw captures.

## Verification

Run the host-side tests with:

```powershell
python -m unittest discover -s tests -v
```

The tests cover both camera IPA profile generation, GUI worker error handling,
TinyUSB response parsing, safe media inventory, and verified atomic copying.
Compilation and host tests do not replace validation on both physical cameras,
the servo, button, SD card, and both USB connections.
