/* Daily metrics lifecycle and report-writing API. */

#pragma once

#include <stdint.h>
#include <time.h>

#include "camera.h"
#include "esp_err.h"

typedef esp_err_t (*daily_summary_time_provider_t)(struct tm *out_time);

void daily_summary_init(daily_summary_time_provider_t time_provider);
void daily_summary_reset(int32_t day_index);
void daily_summary_ensure_day(const struct tm *now);
esp_err_t daily_summary_note_start(void);
void daily_summary_add_capture(uint32_t capture_ms, esp_err_t result);
void daily_summary_add_save(const camera_save_metrics_t *metrics);
esp_err_t daily_summary_append_image_orientation(
    const struct tm *captured_at,
    const char *image_name,
    const imu_orientation_sample_t *orientation);
esp_err_t daily_summary_write_file(const char *reason, uint16_t configured_days);
