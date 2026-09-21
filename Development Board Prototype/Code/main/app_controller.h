/* Thread-safe application-state events and immutable snapshot API. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    APP_EVENT_SET_ENABLED,
    APP_EVENT_SET_PAUSED,
    APP_EVENT_SET_CAMERA_READY,
    APP_EVENT_SET_FORCED_AWAKE,
    APP_EVENT_SET_TRANSFER_ACTIVE,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    bool value;
} app_event_t;

typedef struct {
    bool enabled;
    bool paused;
    bool camera_ready;
    bool forced_awake;
    bool transfer_active;
    uint32_t events_processed;
    uint32_t events_dropped;
} app_state_snapshot_t;

typedef void (*app_controller_enabled_cb_t)(bool enabled);

typedef struct {
    bool enabled;
    bool paused;
    bool camera_ready;
    bool forced_awake;
    app_controller_enabled_cb_t enabled_changed;
} app_controller_config_t;

esp_err_t app_controller_init(const app_controller_config_t *config);
esp_err_t app_controller_post(app_event_type_t type, bool value);
void app_controller_get_snapshot(app_state_snapshot_t *snapshot);
bool app_controller_is_enabled(void);
bool app_controller_is_paused(void);
bool app_controller_is_camera_ready(void);
bool app_controller_is_forced_awake(void);
