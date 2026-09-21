/* Central storage for runtime-adjustable device settings. */

#include "device_settings.h"

/* Runtime-readable snapshot of the most important compile-time settings. */
const device_settings_t g_device_settings = {
    .capture_interval_ms = CAMERA_AUTO_SNAP_INTERVAL_MS,
    .burst_window_ms = CAMERA_BURST_WINDOW_MS,
    .initial_wake_estimate_ms = INITIAL_WAKE_ESTIMATE_MS,
    .jpeg_quality = CAMERA_JPEG_QUALITY,
    .isp_brightness_percent = CAMERA_ISP_BRIGHTNESS_PERCENT,
    .deep_sleep_enabled = CAMERA_AUTO_SNAP_DEEP_SLEEP != 0,
    .sd_capture_path = DEVICE_SD_CAPTURE_PATH,
};
