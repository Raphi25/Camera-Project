#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Camera capture owns the V4L2 stream and hardware JPEG encoder. */
esp_err_t capture_init(void);
esp_err_t capture_save(const char *kind);
esp_err_t capture_save_best_burst(const char *kind);

/* Storage copies JPEGs into an asynchronous writer queue. Motion metadata is
   snapshotted with each queued image, and USB export waits for the queue. */
esp_err_t storage_init(void);
esp_err_t storage_save(const void *data, size_t size, const char *kind,
                       uint32_t width, uint32_t height, int64_t frame_us);
esp_err_t storage_wait_idle(uint32_t timeout_ms);
void storage_set_motion_metrics(float amplitude_deg, float velocity_deg_s,
                                float exposure_s, float focal_px);
esp_err_t storage_usb_start(void);
esp_err_t storage_usb_stop(void);
bool storage_usb_active(void);
const char *storage_device_id(void);

/* Servo calls are thread-safe; the worker updates a 50 Hz PWM output. */
esp_err_t servo_init(void);
esp_err_t servo_position(unsigned pulse_us);
esp_err_t servo_sweep(unsigned low_us, unsigned high_us, unsigned period_ms);
esp_err_t servo_sweep_until(unsigned low_us, unsigned high_us, unsigned period_ms, int64_t deadline_us);
void servo_stop(void);
