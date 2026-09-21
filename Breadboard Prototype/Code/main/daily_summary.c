/* Aggregate one operating day's capture and storage metrics into an atomic report. */

#include "daily_summary.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "capture_scheduler.h"
#include "sd_storage.h"

static const char *TAG = "daily_summary";
static daily_summary_time_provider_t s_time_provider;
RTC_DATA_ATTR static int32_t s_day = -1;
RTC_DATA_ATTR static uint32_t s_captures_attempted;
RTC_DATA_ATTR static uint32_t s_captures_ok;
RTC_DATA_ATTR static uint32_t s_writes_attempted;
RTC_DATA_ATTR static uint32_t s_writes_ok;
RTC_DATA_ATTR static uint64_t s_plain_bytes;
RTC_DATA_ATTR static int32_t s_actual_start_min = -1;
RTC_DATA_ATTR static int32_t s_actual_stop_min = -1;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void summary_path_for_day(char *path, size_t path_size,
                                 const struct tm *day)
{
    snprintf(path, path_size, SD_CAPTURE_DIR "/summary_%04d%02d%02d.txt",
             day->tm_year + 1900, day->tm_mon + 1, day->tm_mday);
}

static void orientation_stage_path_for_day(char *path, size_t path_size,
                                           const struct tm *day)
{
    snprintf(path, path_size, SD_CAPTURE_DIR "/summary_%04d%02d%02d.orientation",
             day->tm_year + 1900, day->tm_mon + 1, day->tm_mday);
}

void daily_summary_init(daily_summary_time_provider_t time_provider)
{
    s_time_provider = time_provider;
}

void daily_summary_reset(int32_t day_index)
{
    portENTER_CRITICAL(&s_lock);
    s_day = day_index;
    s_captures_attempted = s_captures_ok = 0;
    s_writes_attempted = s_writes_ok = 0;
    s_plain_bytes = 0;
    s_actual_start_min = s_actual_stop_min = -1;
    portEXIT_CRITICAL(&s_lock);
}

void daily_summary_ensure_day(const struct tm *now)
{
    int32_t day = capture_scheduler_day_index(now);
    if (day >= 0 && s_day != day) {
        daily_summary_reset(day);
    }
}

esp_err_t daily_summary_note_start(void)
{
    if (s_actual_start_min >= 0) return ESP_OK;
    if (s_time_provider == NULL) return ESP_ERR_INVALID_STATE;
    struct tm now = {0};
    esp_err_t err = s_time_provider(&now);
    if (err != ESP_OK) return err;
    portENTER_CRITICAL(&s_lock);
    if (s_actual_start_min < 0) s_actual_start_min = now.tm_hour * 60 + now.tm_min;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void daily_summary_add_capture(uint32_t capture_ms, esp_err_t result)
{
    (void)capture_ms;
    portENTER_CRITICAL(&s_lock);
    s_captures_attempted++;
    if (result == ESP_OK) s_captures_ok++;
    portEXIT_CRITICAL(&s_lock);
}

void daily_summary_add_save(const camera_save_metrics_t *metrics)
{
    if (metrics == NULL) return;
    portENTER_CRITICAL(&s_lock);
    s_writes_attempted++;
    if (metrics->ok) s_writes_ok++;
    s_plain_bytes += metrics->plain_bytes;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t daily_summary_append_image_orientation(
    const struct tm *captured_at,
    const char *image_name,
    const imu_orientation_sample_t *orientation)
{
    if (image_name == NULL || orientation == NULL) return ESP_ERR_INVALID_ARG;

    struct tm day = {0};
    if (captured_at != NULL) {
        day = *captured_at;
    } else {
        if (s_time_provider == NULL) return ESP_ERR_INVALID_STATE;
        esp_err_t err = s_time_provider(&day);
        if (err != ESP_OK) return err;
    }

    char stage_path[112];
    orientation_stage_path_for_day(stage_path, sizeof(stage_path), &day);
    sd_storage_lock();
    FILE *file = fopen(stage_path, "a");
    if (file == NULL) {
        sd_storage_unlock();
        return ESP_FAIL;
    }

    int written;
    if (orientation->valid) {
        written = fprintf(file,
                          "Device was %s for image %s "
                          "(accel_x=%dmg, accel_y=%dmg, accel_z=%dmg)\n",
                          orientation->status, image_name,
                          (int)orientation->x_mg, (int)orientation->y_mg,
                          (int)orientation->z_mg);
    } else {
        written = fprintf(file,
                          "Device orientation was unavailable for image %s\n",
                          image_name);
    }
    bool ok = written > 0 && fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    sd_storage_unlock();
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t daily_summary_write_file(const char *reason, uint16_t configured_days)
{
    esp_err_t err = sd_storage_require_mounted();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Summary skipped because SD is unavailable: %s", esp_err_to_name(err));
        return err;
    }
    if (s_time_provider == NULL) return ESP_ERR_INVALID_STATE;
    struct tm now = {0};
    if ((err = s_time_provider(&now)) != ESP_OK) return err;
    daily_summary_ensure_day(&now);

    uint32_t captures_attempted, captures_ok, writes_attempted, writes_ok;
    uint64_t plain_bytes;
    int32_t start_min, stop_min = now.tm_hour * 60 + now.tm_min;
    portENTER_CRITICAL(&s_lock);
    if (s_actual_stop_min < 0) s_actual_stop_min = stop_min;
    captures_attempted = s_captures_attempted;
    captures_ok = s_captures_ok;
    writes_attempted = s_writes_attempted;
    writes_ok = s_writes_ok;
    plain_bytes = s_plain_bytes;
    start_min = s_actual_start_min;
    stop_min = s_actual_stop_min;
    portEXIT_CRITICAL(&s_lock);

    char path[96];
    char orientation_stage_path[112];
    summary_path_for_day(path, sizeof(path), &now);
    orientation_stage_path_for_day(orientation_stage_path,
                                   sizeof(orientation_stage_path), &now);
    char start[12] = "unknown", stop[12] = "unknown";
    if (start_min >= 0) snprintf(start, sizeof(start), "%02ld:%02ld", (long)start_min / 60, (long)start_min % 60);
    if (stop_min >= 0) snprintf(stop, sizeof(stop), "%02ld:%02ld", (long)stop_min / 60, (long)stop_min % 60);

    sd_storage_atomic_file_t atomic_file;
    esp_err_t begin_err = sd_storage_atomic_begin(&atomic_file, path, "w");
    if (begin_err != ESP_OK) return begin_err;
    FILE *file = atomic_file.file;
    fprintf(file, "Daily device summary\n");
    fprintf(file, "Date: %04d-%02d-%02d\n", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
    fprintf(file, "Reason: %s\n", reason != NULL ? reason : "scheduled summary");
    fprintf(file, "Actual run time: %s to %s\n", start, stop);
    fprintf(file, "Configured days: %u\n\n", (unsigned)configured_days);
    fprintf(file, "Capture cycles attempted: %u\n", (unsigned)captures_attempted);
    fprintf(file, "Capture cycles successful: %u\n", (unsigned)captures_ok);
    fprintf(file, "Media writes attempted: %u\n", (unsigned)writes_attempted);
    fprintf(file, "Media writes successful: %u\n", (unsigned)writes_ok);
    fprintf(file, "Plain media storage: %.3f MB (%.6f GB)\n",
            (double)plain_bytes / 1000000.0, (double)plain_bytes / 1000000000.0);

    FILE *orientations = fopen(orientation_stage_path, "r");
    if (orientations != NULL) {
        fprintf(file, "\nImage orientations\n");
        char line[256];
        while (fgets(line, sizeof(line), orientations) != NULL) {
            if (fputs(line, file) == EOF) break;
        }
        bool orientation_read_ok = !ferror(orientations);
        fclose(orientations);
        if (!orientation_read_ok) {
            sd_storage_atomic_abort(&atomic_file);
            return ESP_FAIL;
        }
    }
    if (ferror(file)) {
        sd_storage_atomic_abort(&atomic_file);
        return ESP_FAIL;
    }
    err = sd_storage_atomic_commit(&atomic_file);
    if (err != ESP_OK) return err;
    sd_storage_lock();
    (void)unlink(orientation_stage_path);
    sd_storage_unlock();
    ESP_LOGI(TAG, "Wrote daily summary: %s", path);
    return ESP_OK;
}
