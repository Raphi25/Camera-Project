# Project context and component interactions

**[Open the interactive architecture explorer](project-architecture.html)** for three visual views with zoom, component descriptions, source links, and SVG export. The page works offline in a web browser.

This document describes the source and configuration in this workspace. The system is an autonomous ESP32-P4 camera with a Windows Tkinter application. It can capture without the PC and reconnect for configuration, diagnostics, and media retrieval. Arrows describe commands or data exchanged, rather than every function call. Dashed arrows indicate optional paths or supporting interactions.

## System context

```mermaid
flowchart LR
    user["User"]
    pc["Windows camera application<br/>BreadboardCameraGUI / GUI.py"]
    files[("PC files<br/>Images, summaries, settings, password hash")]
    board["ESP32-P4 camera firmware<br/>ESP-IDF + FreeRTOS"]
    sensor["CSI camera sensor<br/>Detected by camera driver"]
    rtc["DS3231 real-time clock"]
    imu["BMI323 motion / orientation sensor"]
    button["Capture button"]
    led["Status LED"]
    sd[("SD card<br/>Encrypted images, summaries, logs")]
    flash[("On-board flash / NVS<br/>Program, camera mode, credentials, diagnostics")]
    radio["ESP32-C6 coprocessor<br/>Optional BLE path; disabled"]

    user -->|"Configure, capture, retrieve, inspect"| pc
    user -->|"Toggle capture / wake"| button
    pc <-->|"USB serial: commands, replies, framed media"| board
    pc <-->|"Native USB OTG: mass-storage access"| board
    pc <-->|"Read and write local files"| files
    sensor -->|"MIPI CSI image stream"| board
    board -->|"SCCB / I2C sensor configuration"| sensor
    rtc <-->|"I2C: calendar time and clock setting"| board
    imu -->|"I2C: orientation samples"| board
    button -->|"GPIO input"| board
    board -->|"GPIO output"| led
    board <-->|"SDMMC: file or block access"| sd
    board <-->|"Restore and persist state"| flash
    pc <-.->|"BLE commands and replies"| radio
    radio <-.->|"ESP-Hosted SDIO or alternate UART bridge"| board
```

The two USB connections serve different purposes. USB serial carries the command protocol and can download media. Native OTG exposes the SD card as a Windows volume after an explicit command over the control connection. The firmware and Windows take turns owning the card.

The source supports sensor detection rather than proving which camera is physically attached. `sdkconfig` enables OV5647 and SC2336 support; `camera_capture.c` also names Arducam IMX500 in its detection message. The startup camera-mode code specifically calls the SC2336 driver. Treat the camera node as the attached, detected sensor rather than assuming the older documentation's OV5647-only description.

## Firmware interactions

```mermaid
flowchart TB
    host["Windows application"]
    sensors["Camera / RTC / IMU / button"]
    subgraph fw["ESP32-P4 firmware"]
        boot["main.c + startup_init<br/>Boot wiring, providers, callbacks"]
        commands["Command ingress and dispatch<br/>main.c, command_dispatcher, command_transport<br/>protocol_version, protocol_frame"]
        control["Application and power control<br/>app_controller, program_state<br/>capture_scheduler, communications_policy/control"]
        persistence["program_persistence<br/>device_credentials / device_settings"]
        capture["camera_capture<br/>esp_video + sensor / ISP drivers<br/>scene_classifier + JPEG encoder"]
        writer["Save queue and writer task<br/>Inside camera_capture.c"]
        crypto["image_crypto + crypto_kdf<br/>AES-256-GCM media encryption"]
        media["media_commands + media_transfer<br/>List, decrypt, frame, resume, delete"]
        msc["usb_msc + TinyUSB<br/>Exclusive SD ownership handoff"]
        reports["daily_summary + log_sink<br/>summary_commands + diagnostic_commands"]
        storage["sd_storage<br/>Mounting, mutex, atomic file updates"]
        health["health_diag + event_breadcrumbs<br/>watchdog_supervisor"]
    end
    sd[("SD card")]
    nvs[("NVS / retained RTC memory")]

    host <-->|"Commands, replies, protocol media"| commands
    boot -.->|"Initialize and connect"| control
    boot -.->|"Initialize drivers and inject providers"| capture
    sensors -->|"Frames, time, orientation, user input"| capture
    sensors -->|"Time and button events"| control
    commands -->|"Program, schedule, sleep / wake"| control
    commands -->|"Manual snapshot / burst"| capture
    commands <-->|"Media requests and replies"| media
    commands <-->|"Summary and diagnostic requests"| reports
    commands -->|"Start / stop mass storage"| msc
    control -->|"Schedule captures and wait for completion"| capture
    control <-->|"Save / restore program"| persistence
    persistence <-->|"Persistent configuration and credentials"| nvs
    control <-->|"Retain state across deep sleep"| nvs
    capture -->|"JPEG plus timestamp and orientation"| writer
    writer -->|"Encrypt JPEG"| crypto
    crypto -->|"Encrypted payload returned to writer for commit"| storage
    persistence -.->|"Media credentials"| crypto
    writer -->|"Capture / save metrics and orientation"| reports
    media <-->|"Pause, wait for idle, suspend / resume"| capture
    media -->|"Transfer state callbacks"| control
    media <-->|"Inventory and media files"| storage
    media -->|"Load plaintext from encrypted file"| crypto
    reports <-->|"Logs and summary files"| storage
    msc <-->|"Release / restore application filesystem"| storage
    host <-->|"USB OTG block access while host owns card"| msc
    storage <-->|"Application filesystem over SDMMC"| sd
    msc <-->|"TinyUSB SDMMC backing storage"| sd
    control -.->|"Events and task heartbeats"| health
    capture -.->|"Faults and task heartbeats"| health
    health -->|"Health, reset, task and breadcrumb data"| reports
```

`main.c` remains the composition root: it supplies concrete command handlers, starts tasks, connects callbacks, provides timestamps and orientation, and manages deep sleep. `app_controller` owns shared control state through queued events; `program_state` tracks lifecycle transitions; `capture_scheduler` evaluates capture windows. These are complementary parts, not three independent schedulers.

The capture pipeline produces JPEGs and queues save jobs. Its writer task encrypts and commits files through `sd_storage`, then records indexing and summary information. The current setting selects the direct hardware JPEG encoder (`CAMERA_JPEG_BACKEND_M2M = 0`); `jpeg_m2m_encoder` is an alternate implementation.

## Windows application interactions

```mermaid
flowchart TB
    user["User"]
    ui["GUI.py + views.py<br/>Tkinter application and background work"]
    local["auth.py + settings_service.py<br/>Local login and preferences"]
    actions["device_actions.py / device_queries.py<br/>Device operations and queries"]
    display["presentation_services.py<br/>Reply-to-view conversion"]
    commands["command_service.py<br/>Common command execution"]
    transport["device_transport.py + protocol.py<br/>Serial / BLE connections and negotiation"]
    download["media_transfer_service.py + media_protocol.py<br/>Protocol download, resume and verification"]
    otg["usb_mass_storage_service.py<br/>Volume discovery, copy / decrypt, eject"]
    catalog["image_catalog_service.py + media_utils.py<br/>Catalog, validation and media processing"]
    files[("PC filesystem")]
    device["ESP32-P4 firmware"]
    volume[("Windows-mounted camera SD volume")]

    user <-->|"Controls and results"| ui
    ui <-->|"Login and saved preferences"| local
    local <-->|"Local JSON stores"| files
    ui --> actions
    actions <-->|"Requests and parsed replies"| commands
    actions --> display
    display --> ui
    commands <-->|"Send and receive"| transport
    transport <-->|"USB serial; optional BLE"| device
    ui -->|"Protocol retrieval"| download
    download <-->|"Injected request / data operations"| transport
    download -->|"Validated downloaded files"| files
    ui -->|"OTG retrieval / deletion / eject"| otg
    otg <-->|"Export credentials and MSC start / stop commands"| commands
    otg <-->|"Read encrypted files or delete captures"| volume
    device <-->|"TinyUSB mass storage"| volume
    otg -->|"Locally decrypted images and copied summaries"| files
    files --> catalog
    catalog -->|"Image inventory and previews"| ui
```

`GUI.pyw` launches `GUI.py`, and `Build_GUI_App.cmd` packages that same application as `../../App/BreadboardCameraGUI.exe`. Executables in other folders are build/distribution artifacts; this diagram does not assert that every existing binary matches today's source.

## Main end-to-end flows

1. **Autonomous capture:** boot restores the program from retained memory or NVS; the scheduler checks time; capture reads a frame and orientation; JPEG encoding feeds the save queue; the writer encrypts and saves to SD. Firmware waits for idle before scheduled deep sleep. Timer and button wake paths restart the application.
2. **Control:** a GUI action passes through the command service and transport to firmware dispatch. Handlers update the controller, persistent configuration, clock, or camera and return replies over the initiating transport.
3. **Serial media retrieval:** the GUI requests inventory and files; firmware coordinates capture/transfer state, loads and decrypts media, and sends framed payloads. The GUI checks frame integrity and file metadata/hash information before committing downloads.
4. **OTG media retrieval:** the GUI requests media export credentials and `USB_MSC_START`; firmware pauses capture, waits for idle, suspends the stream, closes the log, and lends the SD card to Windows. The GUI decrypts image files locally. Eject/dismount and `USB_MSC_STOP` return filesystem ownership and restore capture state.
5. **Diagnostics:** tasks report faults, breadcrumbs, heartbeats, and capture/save metrics. Diagnostic and summary commands expose the resulting information to the GUI; the watchdog supervisor detects stalled registered tasks.

## Current configuration and boundaries

- Both `CAMERA_ESP_HOSTED_BLE_ENABLE` and `CAMERA_C6_BLE_BRIDGE_ENABLE` are `0`. BLE support exists in source but is not an active control path under these settings.
- The wired command task starts on every wake. Radio startup is separately subject to the communications power policy.
- The camera control bus shares I2C with the DS3231; the BMI323 uses a separate I2C port. DS3231 calendar time and the ESP32's retained RTC memory are distinct facilities.
- `CAMERA_USB_DETECT_GPIO` is unconfigured. A dedicated USB-detect GPIO wake is therefore conditional; native OTG connection detection and USB-serial connection checks are separate mechanisms.
- `components/` contains local Espressif camera/video components. `managed_components/` supplies dependencies such as TinyUSB and ESP-Hosted. Dependency presence alone does not make Wi-Fi or a cloud service part of the application; no such application path is shown here.
- GUI password authentication is local to the desktop application. It is separate from firmware media credentials and image encryption.

## Source map

| Relationship | Main source evidence |
|---|---|
| Boot, tasks, command registration, sleep and callbacks | [main.c](../main/main.c), [startup_init.c](../main/startup_init.c) |
| Application state and schedule decisions | [app_controller.c](../main/app_controller.c), [program_state.c](../main/program_state.c), [capture_scheduler.c](../main/capture_scheduler.c) |
| Camera, JPEG, save queue, encryption and metadata | [camera_capture.c](../main/camera_capture.c), [image_crypto.c](../main/image_crypto.c), [daily_summary.c](../main/daily_summary.c) |
| Command framing and protocol downloads | [command_transport.c](../main/command_transport.c), [media_transfer.c](../main/media_transfer.c), [protocol_frame.c](../main/protocol_frame.c) |
| Exclusive SD ownership and OTG transfer | [sd_storage.h](../main/sd_storage.h), [usb_msc.c](../main/usb_msc.c), [usb_mass_storage_service.py](../../../App/usb_mass_storage_service.py) |
| Hardware configuration and optional interfaces | [device_settings.h](../main/device_settings.h), [sdkconfig](../sdkconfig), [communications_policy.c](../main/communications_policy.c) |
| Desktop entry point, services and transports | [GUI.py](../../../App/GUI.py), [command_service.py](../../../App/command_service.py), [device_transport.py](../../../App/device_transport.py), [media_transfer_service.py](../../../App/media_transfer_service.py) |
| Build composition and dependencies | [main/CMakeLists.txt](../main/CMakeLists.txt), [idf_component.yml](../main/idf_component.yml), [Build_GUI_App.cmd](../../../App/Build_GUI_App.cmd) |

This is a source-based architecture review, not a hardware connectivity test. Open this Markdown file in a Mermaid-capable preview to render the diagrams.
