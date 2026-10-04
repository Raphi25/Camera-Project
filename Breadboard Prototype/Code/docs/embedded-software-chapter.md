# Embedded Software of the Autonomous Camera

## Overview

This project is an automatic camera built around an ESP32-P4 processor, an OV5647 camera, an SD card, and a DS3231 real-time clock (RTC). It can take pictures on a schedule without a computer and can be controlled from a Windows application through USB or Bluetooth.

The software has five main jobs:

1. capture pictures at the correct time;
2. convert them into JPEG files;
3. encrypt and save them to the SD card;
4. communicate with the Windows application; and
5. recover after sleep, a reset, or a power failure.

## How a Picture Is Saved

The camera sends an image to the ESP32-P4, which processes it and converts it into a JPEG. The JPEG is then encrypted and written to the SD card.

```text
Camera -> image processing -> JPEG -> encryption -> SD card
```

SD-card writes can be slow, so a separate task handles encryption and saving. This allows the camera to prepare for the next picture instead of waiting for the card. This follows the **Delegation** and **Pipeline** patterns described in *Making Embedded Systems* (White 2024, 22, 233-235).

## Main Design Patterns

The project uses several patterns from Elecia White's *Making Embedded Systems: Design Patterns for Great Software*.

| Pattern | Meaning in this project |
|---|---|
| Layering | Hardware, storage, scheduling, commands, and the GUI are kept in separate levels. |
| Encapsulation | Each module has one main responsibility, such as time, encryption, or storage. |
| Delegation | Slow saving work is given to a separate task. |
| Adapter | USB and Bluetooth are made to work with the same command system. |
| Facade | Complicated camera and storage operations are provided through simple functions. |
| Command | Each user command is connected to a specific handler. |
| State Machine | The device can only move between valid operating states. |
| Active Object | One task owns shared application information and receives events from other tasks. |
| Watchdog | The device detects when software has stopped making progress. |

These patterns make the program easier to understand because every important responsibility has a clear owner.

## State and Task Coordination

The camera can be idle, waiting, capturing, saving, transferring files, sleeping, or handling an error. A table-driven state machine defines which changes are allowed. Invalid changes are rejected instead of being silently accepted.

White advises, “When you consider your implementation, be lazy.”[^white-state] In this case, that means using a simple transition table instead of a more complicated system.

Several FreeRTOS tasks need to know whether capture is enabled, paused, or busy. One controller task owns this shared information. Other tasks send events to it, and it processes them one at a time. This reduces conflicts between tasks.

## Time, Sleep, and Power-Loss Recovery

The DS3231 RTC keeps calendar time using its backup battery. It supports normal daytime schedules and schedules that cross midnight. When the camera has nothing to do, the ESP32-P4 enters deep sleep to save energy.

The RTC remembers the time but does not remember the capture program. The firmware therefore stores the current schedule and capture settings in non-volatile storage called NVS.

After power returns, the processor starts from the beginning. It loads the saved program from NVS, reads the RTC, and decides whether to capture immediately or wait for the next scheduled period.

- The RTC remembers **what time it is**.
- NVS remembers **what the camera should do**.

## Protecting Images and Settings

Power can fail while a file is being written. To reduce the chance of corruption, the software first writes new content to a temporary file. It replaces the final file only after the new data is complete. Backup and temporary files can be recovered during the next boot.

Pictures are encrypted with AES-256-GCM before being saved. This hides the image and detects whether the encrypted file has been modified. The project also uses CRC-16 to check transfer frames and SHA-256 to verify complete downloaded files.

The active program and encrypted image format contain version information. This helps future software recognize old or incompatible data.

## Commands and the Windows Application

The device accepts commands for reading status, setting the clock, taking pictures, changing schedules, and downloading files. USB and Bluetooth use the same command handlers through an adapter layer, so the main behavior does not have to be implemented twice.

The packaged application is `BreadboardCameraGUI.exe`. It includes the required Python libraries and provides controls for the camera, schedule, clock, diagnostics, and media transfer. The GUI and firmware exchange version and capability information so they can avoid using unsupported features.

## Fault Detection

The firmware checks normal errors and records useful diagnostic information. It keeps activity logs, daily summaries, reset information, recent event breadcrumbs, task information, and core dumps.

A watchdog is used if a task stops making progress. The higher-level supervisor records which task failed before the processor watchdog resets the system. White warns that “Using a watchdog does not free you from handling normal errors”[^white-watchdog]. The watchdog is therefore a final recovery method, not a replacement for normal error handling.

## Verification and Conclusion

The firmware builds successfully with ESP-IDF 6.0.2 for the ESP32-P4. The Python source is checked for syntax errors, and the packaged Windows application has passed a startup test. Hardware testing is still needed for power failures, SD-card removal, long schedules, deep sleep, RTC faults, and interrupted downloads.

The main strength of the software is clear ownership. The camera module captures images, the writer saves them, the storage module protects file updates, the RTC provides time, NVS stores the active program, and the controller owns shared state. The design patterns from *Making Embedded Systems* help these parts work together without turning the project into one large and difficult program.

## Appendix: Windows GUI Python Files

- `auth.py` verifies the GUI password and safely stores its salted hash in a local credential file.
- `command_service.py` sends ordinary commands over serial or BLE and converts their replies into one common result format.
- `device_actions.py` implements operations that change the device, such as capture, clock synchronization, and media deletion.
- `device_queries.py` performs read-only requests for device status and summary information and returns results ready for display.
- `device_transport.py` owns reusable USB-serial and BLE connections, protocol negotiation, and incoming data collection.
- `GUI.py` is the main packaged Tkinter application for camera control, scheduling, diagnostics, authentication, and media retrieval.
- `image_catalog_service.py` finds downloaded images and creates stable catalog entries for the GUI's image list and preview.
- `media_protocol.py` parses text and binary media frames and checks sequence numbers, lengths, CRC values, and encoded data.
- `media_transfer_service.py` lists device media and safely downloads, resumes, validates, and commits files on the PC.
- `media_utils.py` validates local media, repairs MJPEG frame streams, and locates FFmpeg for optional video conversion.
- `presentation_services.py` converts raw protocol replies into clear text and view models that the GUI can display.
- `protocol.py` parses protocol-negotiation replies and checks whether the connected firmware supports specific capabilities.
- `settings_service.py` validates capture schedules and atomically saves the GUI's local settings.
- `views.py` constructs the Tkinter widgets and connects them to behavior supplied by the main application.

## References

[^white-state]: Elecia White, *Making Embedded Systems: Design Patterns for Great Software*, 2nd ed. (O'Reilly Media, 2024), 164.

[^white-watchdog]: White, *Making Embedded Systems*, 165.

White, Elecia. *Making Embedded Systems: Design Patterns for Great Software*. 2nd ed. Sebastopol, CA: O'Reilly Media, 2024. ISBN 978-1-098-15154-6.
