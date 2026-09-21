/* Calendar-aware capture scheduling, including overnight windows. */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

typedef struct {
    bool enabled;
    uint16_t start_minute;
    uint16_t stop_minute;
    uint16_t days;
    int32_t start_day;
} capture_scheduler_t;

typedef struct {
    uint32_t sleep_ms;
    bool complete;
    bool end_of_day;
    bool day_changed;
    int32_t day_index;
} capture_schedule_result_t;

int32_t capture_scheduler_day_index(const struct tm *value);

/* Configure a daily window; start > stop denotes a window crossing midnight. */
void capture_scheduler_set(capture_scheduler_t *scheduler,
                           uint16_t start_minute,
                           uint16_t stop_minute,
                           uint16_t days);
void capture_scheduler_disable(capture_scheduler_t *scheduler);
esp_err_t capture_scheduler_mark_started(capture_scheduler_t *scheduler, const struct tm *now);
esp_err_t capture_scheduler_evaluate(capture_scheduler_t *scheduler,
                                     const struct tm *now,
                                     capture_schedule_result_t *result);

/* Correct the deep-sleep wake estimate using bounded proportional feedback. */
uint32_t capture_scheduler_adjust_wake_estimate(uint32_t current_estimate_ms,
                                                uint32_t observed_interval_ms,
                                                uint32_t target_interval_ms,
                                                uint32_t min_estimate_ms,
                                                uint32_t max_estimate_ms,
                                                uint32_t feedback_divisor,
                                                uint32_t max_adjust_ms);
