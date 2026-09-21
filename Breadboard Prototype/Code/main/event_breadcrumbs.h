/* Compact breadcrumb codes and snapshot API safe across warm resets. */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define EVENT_BREADCRUMB_CAPACITY 16

typedef enum {
    BREADCRUMB_BOOT = 1,
    BREADCRUMB_PROGRAM_EVENT,
    BREADCRUMB_APP_EVENT,
    BREADCRUMB_CAPTURE_RESULT,
    BREADCRUMB_SD_MOUNT,
    BREADCRUMB_SD_COMMIT,
    BREADCRUMB_TRANSFER,
    BREADCRUMB_SLEEP,
    BREADCRUMB_WATCHDOG_MISS,
    BREADCRUMB_FAULT,
} event_breadcrumb_code_t;

typedef struct {
    uint32_t sequence;
    uint32_t uptime_ms;
    event_breadcrumb_code_t code;
    int32_t value;
} event_breadcrumb_t;

void event_breadcrumbs_init(int32_t reset_reason);
void event_breadcrumb_record(event_breadcrumb_code_t code, int32_t value);
size_t event_breadcrumbs_snapshot(event_breadcrumb_t *output, size_t capacity);
const char *event_breadcrumb_name(event_breadcrumb_code_t code);
