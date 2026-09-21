/* RTC-retained bounded event history used for post-reset diagnosis. */

#include "event_breadcrumbs.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define BREADCRUMB_RTC_MAGIC 0x42524332U

RTC_DATA_ATTR static uint32_t s_magic;
RTC_DATA_ATTR static uint32_t s_next_sequence;
RTC_DATA_ATTR static uint32_t s_write_index;
RTC_DATA_ATTR static uint32_t s_count;
RTC_DATA_ATTR static event_breadcrumb_t s_entries[EVENT_BREADCRUMB_CAPACITY];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void event_breadcrumbs_init(int32_t reset_reason)
{
    portENTER_CRITICAL(&s_lock);
    if (s_magic != BREADCRUMB_RTC_MAGIC ||
        s_write_index >= EVENT_BREADCRUMB_CAPACITY ||
        s_count > EVENT_BREADCRUMB_CAPACITY) {
        memset(s_entries, 0, sizeof(s_entries));
        s_next_sequence = 0;
        s_write_index = 0;
        s_count = 0;
        s_magic = BREADCRUMB_RTC_MAGIC;
    }
    portEXIT_CRITICAL(&s_lock);
    event_breadcrumb_record(BREADCRUMB_BOOT, reset_reason);
}

void event_breadcrumb_record(event_breadcrumb_code_t code, int32_t value)
{
    event_breadcrumb_t entry = {
        .uptime_ms = (uint32_t)(esp_timer_get_time() / 1000LL),
        .code = code,
        .value = value,
    };
    portENTER_CRITICAL(&s_lock);
    entry.sequence = ++s_next_sequence;
    s_entries[s_write_index] = entry;
    s_write_index = (s_write_index + 1U) % EVENT_BREADCRUMB_CAPACITY;
    if (s_count < EVENT_BREADCRUMB_CAPACITY) s_count++;
    portEXIT_CRITICAL(&s_lock);
}

size_t event_breadcrumbs_snapshot(event_breadcrumb_t *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return 0;
    portENTER_CRITICAL(&s_lock);
    size_t count = s_count < capacity ? s_count : capacity;
    size_t oldest = (s_write_index + EVENT_BREADCRUMB_CAPACITY - s_count) %
                    EVENT_BREADCRUMB_CAPACITY;
    size_t skip = s_count - count;
    for (size_t i = 0; i < count; ++i) {
        output[i] = s_entries[(oldest + skip + i) % EVENT_BREADCRUMB_CAPACITY];
    }
    portEXIT_CRITICAL(&s_lock);
    return count;
}

const char *event_breadcrumb_name(event_breadcrumb_code_t code)
{
    static const char *const names[] = {
        "UNKNOWN", "BOOT", "PROGRAM", "APP", "CAPTURE", "SD_MOUNT",
        "SD_COMMIT", "TRANSFER", "SLEEP", "WATCHDOG", "FAULT",
    };
    return code >= BREADCRUMB_BOOT && code <= BREADCRUMB_FAULT
        ? names[code]
        : names[0];
}
