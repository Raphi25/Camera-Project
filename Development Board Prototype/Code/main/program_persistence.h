/* Power-loss-safe persistence for the autonomous capture configuration. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool enabled;
    bool schedule_enabled;
    uint16_t schedule_start_minute;
    uint16_t schedule_stop_minute;
    uint16_t schedule_days;
    int32_t schedule_start_day;
    uint32_t interval_ms;
    bool burst_enabled;
} program_persistence_state_t;

esp_err_t program_persistence_init(void);

/* Load the last committed record. A missing record is success with found=false. */
esp_err_t program_persistence_load(program_persistence_state_t *state, bool *found);

/* Commit a versioned record to NVS before acknowledging a program change. */
esp_err_t program_persistence_save(const program_persistence_state_t *state);
