/* Queue-owned application state controller; callers communicate only by events. */

#include "app_controller.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "watchdog_supervisor.h"

#define APP_EVENT_QUEUE_LENGTH 16

static QueueHandle_t s_event_queue;
static app_state_snapshot_t s_state;
static app_controller_enabled_cb_t s_enabled_changed;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static void controller_task(void *arg)
{
    (void)arg;
    app_event_t event;
    watchdog_heartbeat_t heartbeat =
        watchdog_supervisor_register("app_controller", 2000);
    while (true) {
        watchdog_supervisor_beat(heartbeat);
        if (xQueueReceive(s_event_queue, &event, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        bool notify_enabled = false;
        portENTER_CRITICAL(&s_state_lock);
        switch (event.type) {
        case APP_EVENT_SET_ENABLED:
            notify_enabled = s_state.enabled != event.value;
            s_state.enabled = event.value;
            if (!event.value) s_state.forced_awake = false;
            break;
        case APP_EVENT_SET_PAUSED:
            s_state.paused = event.value;
            break;
        case APP_EVENT_SET_CAMERA_READY:
            s_state.camera_ready = event.value;
            break;
        case APP_EVENT_SET_FORCED_AWAKE:
            s_state.forced_awake = event.value;
            break;
        case APP_EVENT_SET_TRANSFER_ACTIVE:
            s_state.transfer_active = event.value;
            break;
        default:
            break;
        }
        s_state.events_processed++;
        portEXIT_CRITICAL(&s_state_lock);
        if (notify_enabled && s_enabled_changed != NULL) {
            s_enabled_changed(event.value);
        }
    }
}

esp_err_t app_controller_init(const app_controller_config_t *config)
{
    if (config == NULL || s_event_queue != NULL) return ESP_ERR_INVALID_ARG;
    memset(&s_state, 0, sizeof(s_state));
    s_state.enabled = config->enabled;
    s_state.paused = config->paused;
    s_state.camera_ready = config->camera_ready;
    s_state.forced_awake = config->forced_awake;
    s_enabled_changed = config->enabled_changed;
    s_event_queue = xQueueCreate(APP_EVENT_QUEUE_LENGTH, sizeof(app_event_t));
    if (s_event_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(controller_task, "app_controller", 4096, NULL, 7, NULL) != pdPASS) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t app_controller_post(app_event_type_t type, bool value)
{
    if (s_event_queue == NULL) return ESP_ERR_INVALID_STATE;
    app_event_t event = {.type = type, .value = value};
    if (xQueueSend(s_event_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) return ESP_OK;
    portENTER_CRITICAL(&s_state_lock);
    s_state.events_dropped++;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_ERR_TIMEOUT;
}

void app_controller_get_snapshot(app_state_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    portENTER_CRITICAL(&s_state_lock);
    *snapshot = s_state;
    portEXIT_CRITICAL(&s_state_lock);
}

bool app_controller_is_enabled(void) { app_state_snapshot_t s; app_controller_get_snapshot(&s); return s.enabled; }
bool app_controller_is_paused(void) { app_state_snapshot_t s; app_controller_get_snapshot(&s); return s.paused; }
bool app_controller_is_camera_ready(void) { app_state_snapshot_t s; app_controller_get_snapshot(&s); return s.camera_ready; }
bool app_controller_is_forced_awake(void) { app_state_snapshot_t s; app_controller_get_snapshot(&s); return s.forced_awake; }
