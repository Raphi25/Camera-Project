Shared Camera App
=================

This folder contains the single desktop application used by both sibling
firmware projects: Development Board Prototype and Breadboard Prototype.
Select the connected board's COM port or BLE connection inside the app.
The app negotiates firmware capabilities; no project selection is required.

RUN
Double-click Launch_GUI.cmd or BreadboardCameraGUI.exe. Python and ESP-IDF
are not required for the packaged executable. Both firmware projects also
contain Launch_GUI.cmd shortcuts to this same application.

EDIT AND BUILD
The Python source and Camera.png/Camera.ico assets are in this folder.
Run Setup_App.cmd once to create this app's own Python environment and
install dependencies (requires Python and an internet connection).
Run Launch_Source.cmd to use edited source, or Build_GUI_App.cmd to rebuild
BreadboardCameraGUI.exe here. Launch_GUI.cmd prefers the packaged executable,
so rebuild it after editing source. The App shows the last captured scene, its brightness, and the percentage of dark pixels.

SETTINGS AND PASSWORD
Source and packaged launches use gui_settings_store.json and
gui_password_store.json in this folder. The migration keeps the Development
Board Prototype 1 source GUI's saved settings and password. Earlier source
and packaged settings/passwords from both projects are preserved separately
under .migration-backup. Use the existing source GUI password; on a fresh
installation without a password store, the initial password is ChangeMe123!.
RTC_GUI_PASSWORD can override the local password, as before.

MEDIA
Downloaded media is saved under Pictures\SD_Card_Media in your user folder.
For fast retrieval, connect the camera's regular serial cable and a second
cable to its native OTG USB-C port. Get SD Media copies/decrypts files and
returns the SD card to the device. Serial transfer is used without OTG.
After an interrupted OTG copy, retry Get SD Media or use Eject OTG.
FFmpeg is optional for MJPEG conversion; make ffmpeg.exe available on PATH.

MIGRATION BACKUP
.migration-backup contains the previous gui, App, App_pending, and GUI dist
folders plus root GUI executables and App.zip archives from both projects.
The firmware projects contain only shortcuts to this shared app; their local GUI folders have been removed.
Keep the backup until you have verified the app with your hardware.
