/* Deadline-based task heartbeat supervisor with diagnostic snapshots. */

#include "watchdog_supervisor.h"

#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "health_diag.h"
#include "event_breadcrumbs.h"

#define SUPERVISOR_POLL_MS 500U

typedef struct {
    bool used;
    bool overdue;
    char name[16];
    uint32_t deadline_ms;
    int64_t last_beat_us;
    uint32_t missed_count;
} heartbeat_slot_t;

static const char *TAG = "watchdog_supervisor";
static heartbeat_slot_t s_slots[WATCHDOG_SUPERVISOR_MAX_TASKS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;

static void supervisor_task(void *arg)
{
    (void)arg;
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK && wdt_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Task watchdog registration failed: %s", esp_err_to_name(wdt_err));
        (void)health_diag_record_fault("heartbeat_wdt", wdt_err);
    }

    while (true) {
        int64_t now_us = esp_timer_get_time();
        char newly_overdue[WATCHDOG_SUPERVISOR_MAX_TASKS][16] = {{0}};
        size_t newly_overdue_count = 0;

        portENTER_CRITICAL(&s_lock);
        for (size_t i = 0; i < WATCHDOG_SUPERVISOR_MAX_TASKS; ++i) {
            heartbeat_slot_t *slot = &s_slots[i];
            if (!slot->used) continue;
            bool overdue = now_us - slot->last_beat_us >
                           (int64_t)slot->deadline_ms * 1000LL;
            if (overdue && !slot->overdue) {
                slot->overdue = true;
                slot->missed_count++;
                strlcpy(newly_overdue[newly_overdue_count++],
                        slot->name, sizeof(newly_overdue[0]));
            } else if (!overdue) {
                slot->overdue = false;
            }
        }
        portEXIT_CRITICAL(&s_lock);

        for (size_t i = 0; i < newly_overdue_count; ++i) {
            ESP_LOGE(TAG, "Task heartbeat missed: %s", newly_overdue[i]);
            event_breadcrumb_record(BREADCRUMB_WATCHDOG_MISS, (int32_t)i);
            (void)health_diag_record_fault(newly_overdue[i], ESP_ERR_TIMEOUT);
        }

        (void)esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_POLL_MS));
    }
}

esp_err_t watchdog_supervisor_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    memset(s_slots, 0, sizeof(s_slots));
    if (xTaskCreate(supervisor_task, "heartbeat", 4096, NULL, 8, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    return ESP_OK;
}

watchdog_heartbeat_t watchdog_supervisor_register(const char *name, uint32_t deadline_ms)
{
    if (!s_initialized || name == NULL || *name == '\0' || deadline_ms == 0) {
        return WATCHDOG_HEARTBEAT_INVALID;
    }

    watchdog_heartbeat_t result = WATCHDOG_HEARTBEAT_INVALID;
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < WATCHDOG_SUPERVISOR_MAX_TASKS; ++i) {
        if (!s_slots[i].used) {
            s_slots[i].used = true;
            strlcpy(s_slots[i].name, name, sizeof(s_slots[i].name));
            s_slots[i].deadline_ms = deadline_ms;
            s_slots[i].last_beat_us = esp_timer_get_time();
            result = (watchdog_heartbeat_t)i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return result;
}

void watchdog_supervisor_beat(watchdog_heartbeat_t heartbeat)
{
    if (heartbeat < 0 || heartbeat >= WATCHDOG_SUPERVISOR_MAX_TASKS) return;
    portENTER_CRITICAL(&s_lock);
    heartbeat_slot_t *slot = &s_slots[(size_t)heartbeat];
    if (slot->used) {
        slot->last_beat_us = esp_timer_get_time();
        slot->overdue = false;
    }
    portEXIT_CRITICAL(&s_lock);
}

void watchdog_supervisor_get_status(watchdog_supervisor_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < WATCHDOG_SUPERVISOR_MAX_TASKS; ++i) {
        const heartbeat_slot_t *slot = &s_slots[i];
        if (!slot->used) continue;
        watchdog_heartbeat_status_t *out = &status->tasks[status->registered_count++];
        strlcpy(out->name, slot->name, sizeof(out->name));
        out->deadline_ms = slot->deadline_ms;
        int64_t age_us = now_us - slot->last_beat_us;
        out->age_ms = age_us > 0 ? (uint32_t)(age_us / 1000LL) : 0;
        out->missed_count = slot->missed_count;
        out->overdue = slot->overdue;
        if (slot->overdue) status->overdue_count++;
        status->total_misses += slot->missed_count;
    }
    portEXIT_CRITICAL(&s_lock);
}
