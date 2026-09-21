/* Pure schedule-window calculations plus persisted run-start bookkeeping. */

#include "capture_scheduler.h"

#include <string.h>

int32_t capture_scheduler_day_index(const struct tm *value)
{
    if (value == NULL) {
        return -1;
    }
    struct tm copy = *value;
    time_t epoch = mktime(&copy);
    return epoch == (time_t)-1 ? -1 : (int32_t)(epoch / 86400);
}

void capture_scheduler_set(capture_scheduler_t *scheduler,
                           uint16_t start_minute,
                           uint16_t stop_minute,
                           uint16_t days)
{
    if (scheduler == NULL) {
        return;
    }
    scheduler->enabled = true;
    scheduler->start_minute = start_minute;
    scheduler->stop_minute = stop_minute;
    scheduler->days = days;
    scheduler->start_day = -1;
}

void capture_scheduler_disable(capture_scheduler_t *scheduler)
{
    if (scheduler != NULL) {
        scheduler->enabled = false;
        scheduler->start_day = -1;
    }
}

esp_err_t capture_scheduler_mark_started(capture_scheduler_t *scheduler, const struct tm *now)
{
    if (scheduler == NULL || now == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!scheduler->enabled || scheduler->start_day >= 0) {
        return ESP_OK;
    }
    scheduler->start_day = capture_scheduler_day_index(now);
    return scheduler->start_day < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t capture_scheduler_evaluate(capture_scheduler_t *scheduler,
                                     const struct tm *now,
                                     capture_schedule_result_t *result)
{
    if (scheduler == NULL || now == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    if (!scheduler->enabled || scheduler->start_minute == scheduler->stop_minute) {
        return ESP_OK;
    }

    int32_t today = capture_scheduler_day_index(now);
    if (today < 0) {
        return ESP_FAIL;
    }
    result->day_index = today;
    if (scheduler->start_day < 0) {
        scheduler->start_day = today;
        result->day_changed = true;
    }

    int32_t elapsed_days = today - scheduler->start_day + 1;
    if (scheduler->days > 0 && elapsed_days > (int32_t)scheduler->days) {
        result->complete = true;
        return ESP_OK;
    }

    uint16_t now_minute = (uint16_t)(now->tm_hour * 60 + now->tm_min);
    bool in_window;
    uint32_t sleep_minutes = 0;
    bool wakes_tomorrow = false;
    if (scheduler->start_minute < scheduler->stop_minute) {
        in_window = now_minute >= scheduler->start_minute && now_minute < scheduler->stop_minute;
        if (!in_window && now_minute < scheduler->start_minute) {
            sleep_minutes = scheduler->start_minute - now_minute;
        } else if (!in_window) {
            sleep_minutes = 1440U - now_minute + scheduler->start_minute;
            wakes_tomorrow = true;
            result->end_of_day = true;
        }
    } else {
        /* An overnight window spans midnight, e.g. 22:00 through 06:00. */
        in_window = now_minute >= scheduler->start_minute || now_minute < scheduler->stop_minute;
        if (!in_window) {
            sleep_minutes = scheduler->start_minute - now_minute;
            result->end_of_day = true;
        }
    }
    if (in_window) {
        return ESP_OK;
    }

    if (scheduler->days > 0) {
        int32_t wake_day = today + (wakes_tomorrow ? 1 : 0);
        if (wake_day - scheduler->start_day + 1 > (int32_t)scheduler->days) {
            result->complete = true;
            return ESP_OK;
        }
    }
    result->sleep_ms = sleep_minutes * 60U * 1000U;
    if (result->sleep_ms == 0) {
        result->sleep_ms = 1000U;
    }
    return ESP_OK;
}

uint32_t capture_scheduler_adjust_wake_estimate(uint32_t current_estimate_ms,
                                                uint32_t observed_interval_ms,
                                                uint32_t target_interval_ms,
                                                uint32_t min_estimate_ms,
                                                uint32_t max_estimate_ms,
                                                uint32_t feedback_divisor,
                                                uint32_t max_adjust_ms)
{
    /* Apply bounded proportional feedback so one delayed wake cannot overcorrect. */
    int64_t error_ms = (int64_t)observed_interval_ms - (int64_t)target_interval_ms;
    if (feedback_divisor == 0) feedback_divisor = 1;
    int64_t adjustment_ms = error_ms / (int64_t)feedback_divisor;
    if (adjustment_ms > (int64_t)max_adjust_ms) adjustment_ms = max_adjust_ms;
    if (adjustment_ms < -(int64_t)max_adjust_ms) adjustment_ms = -(int64_t)max_adjust_ms;

    int64_t updated = (int64_t)current_estimate_ms + adjustment_ms;
    if (updated < (int64_t)min_estimate_ms) updated = min_estimate_ms;
    if (updated > (int64_t)max_estimate_ms) updated = max_estimate_ms;
    return (uint32_t)updated;
}
