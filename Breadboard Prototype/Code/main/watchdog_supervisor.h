/* Register, beat, and inspect supervised task heartbeats. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define WATCHDOG_SUPERVISOR_MAX_TASKS 8
#define WATCHDOG_HEARTBEAT_INVALID (-1)

typedef int8_t watchdog_heartbeat_t;

typedef struct {
    char name[16];
    uint32_t deadline_ms;
    uint32_t age_ms;
    uint32_t missed_count;
    bool overdue;
} watchdog_heartbeat_status_t;

typedef struct {
    uint32_t registered_count;
    uint32_t overdue_count;
    uint32_t total_misses;
    watchdog_heartbeat_status_t tasks[WATCHDOG_SUPERVISOR_MAX_TASKS];
} watchdog_supervisor_status_t;

esp_err_t watchdog_supervisor_init(void);
watchdog_heartbeat_t watchdog_supervisor_register(const char *name, uint32_t deadline_ms);
void watchdog_supervisor_beat(watchdog_heartbeat_t heartbeat);
void watchdog_supervisor_get_status(watchdog_supervisor_status_t *status);
