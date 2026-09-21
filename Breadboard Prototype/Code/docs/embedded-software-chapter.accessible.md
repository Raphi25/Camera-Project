# Embedded Software of the Autonomous Camera

## 1. Overview

This project is a camera that can take pictures automatically without being connected to a computer. It uses an ESP32-P4 processor, an OV5647 camera, an SD card, and a DS3231 real-time clock (RTC). A Windows application allows the user to control the camera through USB or Bluetooth.

The software performs five main jobs:

1. take pictures at the requested time;
2. convert the pictures into JPEG files;
3. encrypt and save them to the SD card;
4. communicate with the Windows application; and
5. recover safely after sleep, a reset, or a loss of power.

The system is designed to continue working even when one part has a problem. For example, if the camera or SD card fails, the communication and diagnostic features can remain available so the user can find out what happened.

## 2. How a Picture Is Created

When it is time to take a picture, the camera sends raw image data to the ESP32-P4. The processor improves the image and converts it into a JPEG. The JPEG is then encrypted and saved to the SD card.

```text
Camera -> image processing -> JPEG -> encryption -> SD card
```

Saving to an SD card can be much slower than taking a picture. To avoid making the camera wait, the software gives the completed JPEG to a separate saving task. The camera can then prepare for the next picture while the saving task handles encryption and storage.

This is an example of **delegation** and a **pipeline**, two ideas discussed in *Making Embedded Systems*. Delegation means giving slow work to another part of the program. A pipeline means dividing a large job into smaller stages, with each stage having one clear purpose (White 2024, 22, 233-235).

## 3. How the Software Is Organized

The software is divided into small modules instead of placing everything in one large file. Each module has a clear responsibility. For example:

- the RTC module reads and sets time;
- the camera module captures images;
- the encryption module protects image data;
- the storage module manages the SD card;
- the scheduler decides when pictures should be taken;
- the command modules process requests from the user; and
- the diagnostic modules record faults and system health.

This follows the book's ideas of **layering** and **encapsulation** (White 2024, 20-27). In plain language, one part of the program should not need to know the private details of another part. Code that needs the time simply asks the RTC module. It does not need to understand I2C messages or RTC registers.

The main benefit is that one part can change without forcing the entire program to change. For example, the project was updated to ESP-IDF 6.0.2 and the newer PSA Crypto system. Most of this work remained inside the encryption modules, while the camera and scheduling logic stayed the same.

## 4. The Main Design Patterns

The project uses several design patterns from Elecia White's *Making Embedded Systems: Design Patterns for Great Software*.

| Pattern | Simple meaning | How it helps this project |
|---|---|---|
| Layering | Arrange software in levels | Keeps hardware details away from the user interface and scheduling rules |
| Encapsulation | Give each module one clear job | Makes the software easier to understand and change |
| Delegation | Give slow work to another task | Prevents SD-card saving from blocking the camera |
| Adapter | Make different connections look similar | Lets USB and Bluetooth use the same commands |
| Facade | Hide a complicated process behind a simple operation | Lets the program request a picture without managing every camera step |
| Dependency Injection | Provide a module with the operations it needs | Allows media transfer to pause the camera without owning the camera code |
| Command | Connect each user command to one handler | Keeps the control system organized |
| State Machine | Define allowed operating states and changes | Prevents impossible or unsafe sequences |
| Active Object | Let one task own shared information | Reduces conflicts between tasks |
| Watchdog | Detect software that has stopped progressing | Allows the device to recover from a serious lockup |

These patterns are useful because they answer practical questions: Who owns this information? Which part controls this hardware? What should happen when work is slow? How should the device recover from a fault?

## 5. States and Safe Coordination

The device can be booting, idle, waiting, capturing, saving, transferring files, preparing to sleep, or handling an error. These states cannot occur in any order. For example, the device should not enter the saving state when no image has been captured.

The firmware uses a **table-driven state machine** to list the allowed changes. If an invalid change is requested, the software reports an error instead of guessing what to do. White advises, “When you consider your implementation, be lazy.”[^white-state] Here, that means using the simplest state-machine design that clearly protects the system.

Several tasks also need to know whether capturing is enabled, paused, or busy. Instead of allowing every task to change these shared values, one controller task owns them. Other tasks send events to the controller. The controller processes one event at a time and publishes an updated view of the system.

This is the **Active Object** pattern from the book (White 2024, 174-176). It reduces the chance that two tasks will change the same information at the same time.

## 6. Scheduling, Sleep, and Power Loss

The DS3231 RTC keeps calendar time using its backup battery. The scheduler uses this time to decide when the camera is allowed to operate. It supports normal daytime schedules and schedules that cross midnight, such as 22:00 to 06:00.

When the device has nothing to do, the ESP32-P4 can enter deep sleep. Before sleeping, the software saves important information and selects the next wake-up source. During an automatic timer wake, it normally avoids starting Bluetooth to save energy. If USB is connected, the device stays awake for interactive use.

The RTC alone cannot make the program continue after complete power loss. The RTC remembers the time, but not the active camera settings. The software therefore stores the current program in non-volatile storage called NVS. This includes whether automatic capture is enabled, the schedule, capture interval, burst setting, and starting day.

After power returns, the processor starts from the beginning. It loads the saved program from NVS, reads the current time from the RTC, and decides whether to resume immediately or wait for the next scheduled period.

In short:

- the RTC remembers **what time it is**; and
- NVS remembers **what the camera was supposed to do**.

Both are needed for automatic recovery.

## 7. Protecting Files and Settings

Power can fail while a file is being written, so the software does not immediately replace the final file. It first writes to a temporary file. When the new file is complete, it safely replaces the old one. A backup is kept during the replacement and removed only after success.

If power is lost during this process, the storage module checks for temporary and backup files during the next boot. This does not make the SD card completely immune to corruption, but it greatly reduces the chance of losing a previously valid file.

The active program is stored as a versioned NVS record. Version and size fields help the firmware reject information that is incomplete or belongs to an incompatible future format.

The Windows GUI uses a similar safe replacement method when its password is changed. It stores only a salted password hash, not the readable password itself.

## 8. Encryption and Data Integrity

New pictures are encrypted with AES-256-GCM before they are saved to the removable SD card. This protects the contents of the image and detects whether the encrypted file has been changed.

The system also uses:

- CRC-16 to detect damaged transfer frames;
- SHA-256 to verify complete downloaded files; and
- version numbers so the firmware and GUI can understand which features they share.

These methods have different purposes. Encryption hides the image. The AES-GCM authentication tag detects modification. CRC checks short transfer frames, while SHA-256 provides a stronger check of the complete file.

Encryption does not solve every security problem. Production use would still require careful password provisioning, secure boot, flash encryption, BLE security, and a method for changing device credentials.

## 9. Commands and the Windows Application

The device accepts text commands for operations such as reading status, setting the clock, taking a picture, changing the schedule, and downloading media. Each command is connected to a specific handler. This is the **Command Pattern** described by White (White 2024, 79-81).

USB and BLE carry data differently, but an **Adapter Pattern** gives both transports a common command interface. This avoids writing separate camera-control logic for each connection.

The packaged Windows program is `BreadboardCameraGUI.exe`. It is built from `gui/GUI.py` and includes the required Python libraries. The GUI is also divided into clear parts: transports handle connections, services handle commands and downloads, protocol code checks incoming data, and the views display information to the user.

The GUI and firmware exchange version and capability information. This allows a newer GUI to avoid requesting a feature that an older firmware version does not support.

## 10. Detecting and Explaining Failures

The device uses two watchdog levels. ESP-IDF provides the processor-level watchdog, while `watchdog_supervisor.c` checks whether important tasks are still reporting regular heartbeats. If a task stops responding, the supervisor records its name before the final watchdog reset occurs.

White warns that “Using a watchdog does not free you from handling normal errors”[^white-watchdog]. The software therefore still checks normal errors and reports them. The watchdog is used only when the system can no longer recover normally.

The firmware keeps several forms of diagnostic information:

- normal activity logs;
- daily capture and storage summaries;
- persistent boot and error information;
- a short history of important events kept in RTC memory;
- FreeRTOS task information; and
- core dumps for serious processor faults.

These records help answer not only *that* the system failed, but also *where* and *when* it failed.

## 11. Verification and Remaining Work

The firmware currently builds successfully with ESP-IDF 6.0.2 for the ESP32-P4. The Python source is checked for syntax errors, and the packaged Windows application has been tested to confirm that it starts correctly.

Real hardware testing is still essential. A computer-only test cannot reproduce camera timing, SD-card removal, brownouts, deep-sleep wiring, RTC battery problems, or radio interference. Important future tests include repeatedly cutting power during saves, running schedules across midnight, interrupting downloads, removing the SD card during writes, and operating the system continuously for long periods.

The main remaining software improvement is automated testing for schedule calculations, state changes, communication frames, and interrupted file operations. Production deployment would also require a complete security and device-provisioning plan.

## 12. Conclusion

The camera software is designed around reliability rather than only taking pictures. It separates hardware control from user commands, gives slow storage work to a separate task, controls shared information through one owner, saves the active program before power is lost, protects files during updates, and records useful evidence when something fails.

The most useful design patterns from *Making Embedded Systems* are layering, encapsulation, delegation, adapters, facades, dependency injection, commands, state machines, active objects, pipelines, and watchdogs. They make the project easier to understand because every important responsibility has a clear owner and every complicated operation is divided into manageable steps.

## References

[^white-state]: Elecia White, *Making Embedded Systems: Design Patterns for Great Software*, 2nd ed. (O'Reilly Media, 2024), 164.

[^white-watchdog]: White, *Making Embedded Systems*, 165.

White, Elecia. *Making Embedded Systems: Design Patterns for Great Software*. 2nd ed. Sebastopol, CA: O'Reilly Media, 2024. ISBN 978-1-098-15154-6.
