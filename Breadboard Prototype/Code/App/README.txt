Breadboard Camera GUI
=====================

1. Unzip the complete App folder.
2. Double-click BreadboardCameraGUI.exe.
3. The initial password is: ChangeMe123!
4. Connect the camera's regular USB/serial cable and select its COM port in the
   application.
5. For fast retrieval, also connect a second cable to the native OTG USB-C port
   before clicking Get SD Media. The GUI detects the temporary SD-card drive,
   copies/decrypts all files, safely releases the drive, and returns the SD card
   to the ESP32-P4. Without the OTG connection, it falls back to serial transfer.
6. If an OTG copy is interrupted, the SD card remains exposed so Get SD Media
   can retry. Click Eject OTG to safely return the SD card to the ESP32-P4.

No Python or ESP-IDF installation is required to run the GUI.

The application creates its settings and password files in this folder.
Keep the folder in a writable location, such as Documents or Desktop.

Downloaded media is stored under:
  Pictures\SD_Card_Media

FFmpeg is optional. Install FFmpeg or make ffmpeg.exe available on PATH if
MJPEG conversion features require it.
