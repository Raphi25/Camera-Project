# Camera stack provenance

Copied from the local `Breadboard Prototype - SC2336` on 2026-09-06.
Directory names omit the `espressif__` registry prefix so ESP-IDF can resolve
the component dependencies with its component manager disabled.

- esp_cam_sensor 2.3.0: original project's local component, including its patches.
- esp_video 2.3.0, esp_ipa 2.2.0~1, esp_sccb_intf 0.0.8 and cmake_utilities:
  original project's managed components; version metadata is retained.

The OV5647 driver file was SHA-256 identical in both source projects.
`main/ov5647_custom.json` was copied from the OV5647 prototype.
Third-party licenses and component manifests are retained in each directory.

Local change: `esp_ipa/CMakeLists.txt` and `tools/config/esp_ipa_config.py` pass
and accept multiple separately quoted JSON paths. This fixes building both
sensor profiles together in a Windows workspace whose path contains spaces.
Legacy semicolon-separated input remains accepted.

TinyUSB additions copied from the same local prototype on 2026-09-08:
- esp_tinyusb 2.2.1 (commit 8e779566ef71d43928cbf7e125e8eb54bab3f542).
- tinyusb 0.21.0~1 (commit 7049c58a0e895acc92c6407574b05b5536eddfc8).
The existing esp_tinyusb CMake dependency declaration for disabled component
manager is retained. License files are included in both components.
