/* Camera lifecycle, capture scheduling boundary, and persistence metrics API. */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "device_settings.h"
#include "esp_err.h"
#include "imu_orientation.h"
#include "scene_classifier.h"

/*
 * Optional ESP32-C6 BLE control bridge.
 *
 * Option A architecture:
 *   PC GUI <--BLE--> ESP32-C6 <--UART text commands--> ESP32-P4
 *
 * Keep this disabled until the real P4<->C6 UART TX/RX pins are confirmed on
 * the FireBeetle board. C6_WAKEUP is only an attention/wake line; it is not the
 * command data path.
 */

typedef esp_err_t (*camera_timestamp_provider_t)(struct tm *out_time);
typedef esp_err_t (*camera_orientation_provider_t)(imu_orientation_sample_t *out_sample);

typedef struct {
    uint32_t capture_ms;
    uint32_t encrypt_ms;
    uint32_t write_ms;
    uint32_t total_ms;
    size_t plain_bytes;
    size_t stored_bytes;
    bool ok;
} camera_save_metrics_t;

typedef void (*camera_capture_metrics_cb_t)(uint32_t capture_ms, esp_err_t result);
typedef void (*camera_save_metrics_cb_t)(const camera_save_metrics_t *metrics);

/* Initialize esp_video/ISP/JPEG resources using the already-created shared I2C
 * bus. This module does not own the RTC/I2C bus lifetime. */
esp_err_t camera_init(i2c_master_bus_handle_t shared_i2c_bus);

/* Release camera resources. Safe to call before deep sleep after writes drain. */
void camera_deinit(void);

/* Capture one JPEG and queue it for encrypted SD-card write. The write may
 * finish after this function returns; use camera_wait_for_idle() before power
 * state transitions or serial image transfer. */
esp_err_t camera_capture_once(void);

/* Save independent encrypted JPEG stills for the requested time window. */
esp_err_t camera_capture_burst(uint32_t duration_ms, uint32_t *captured_count);

/* Wait until queued still-image encryption/write work is complete. */
esp_err_t camera_wait_for_idle(uint32_t timeout_ms);

/* Pause automatic capture from the command path so serial DATA frames cannot be
 * interleaved with camera logs or SD writes. */
void camera_set_capture_paused(bool paused);

/* Explicit stream controls used around media transfer and deep sleep. */
esp_err_t camera_suspend_stream(void);
esp_err_t camera_resume_stream(void);

/* Print current and supported V4L2 capture formats to stdout for serial debug. */
esp_err_t camera_print_v4l2_formats(void);

/* Timestamp callback supplied by main.c so camera filenames can use RTC time
 * without coupling this module to the RTC driver. */
void camera_set_timestamp_provider(camera_timestamp_provider_t provider);

/* Capture-time IMU callback. The resulting posture is appended to the shared
 * orientation text log only after the matching image has committed to SD. */
void camera_set_orientation_provider(camera_orientation_provider_t provider);

/* Optional callbacks for power/battery accounting. They run from camera tasks,
 * so implementations should only update counters and avoid blocking I/O. */
void camera_set_metrics_callbacks(camera_capture_metrics_cb_t capture_cb,
                                  camera_save_metrics_cb_t save_cb);

/* Snapshot the last captured frame's scene analysis. False before the first
 * analysed frame or after camera shutdown; does not wait for capture to finish. */
bool camera_get_scene_stats(scene_stats_t *out_stats);
