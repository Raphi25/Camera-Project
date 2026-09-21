/* Versioned NVS record used to resume a program after complete power loss. */

#include "program_persistence.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define PROGRAM_NAMESPACE "program"
#define PROGRAM_STATE_KEY "state"
#define PROGRAM_STATE_VERSION 1U
#define PROGRAM_STATE_MAGIC 0x50524731U

typedef struct {
    /* Header fields reject incompatible or partially initialized NVS blobs. */
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint8_t enabled;
    uint8_t schedule_enabled;
    uint8_t burst_enabled;
    uint8_t reserved;
    uint16_t schedule_start_minute;
    uint16_t schedule_stop_minute;
    uint16_t schedule_days;
    uint16_t reserved2;
    int32_t schedule_start_day;
    uint32_t interval_ms;
} program_nvs_record_t;

static SemaphoreHandle_t s_mutex;

esp_err_t program_persistence_init(void)
{
    if (s_mutex != NULL) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t program_persistence_load(program_persistence_state_t *state, bool *found)
{
    if (state == NULL || found == NULL || s_mutex == NULL) return ESP_ERR_INVALID_ARG;
    *found = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(PROGRAM_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }

    program_nvs_record_t record = {0};
    size_t size = sizeof(record);
    err = nvs_get_blob(handle, PROGRAM_STATE_KEY, &record, &size);
    nvs_close(handle);
    xSemaphoreGive(s_mutex);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (size != sizeof(record) || record.magic != PROGRAM_STATE_MAGIC ||
        record.version != PROGRAM_STATE_VERSION || record.size != sizeof(record)) {
        return ESP_ERR_INVALID_VERSION;
    }

    *state = (program_persistence_state_t) {
        .enabled = record.enabled != 0,
        .schedule_enabled = record.schedule_enabled != 0,
        .schedule_start_minute = record.schedule_start_minute,
        .schedule_stop_minute = record.schedule_stop_minute,
        .schedule_days = record.schedule_days,
        .schedule_start_day = record.schedule_start_day,
        .interval_ms = record.interval_ms,
        .burst_enabled = record.burst_enabled != 0,
    };
    *found = true;
    return ESP_OK;
}

esp_err_t program_persistence_save(const program_persistence_state_t *state)
{
    if (state == NULL || s_mutex == NULL) return ESP_ERR_INVALID_ARG;
    program_nvs_record_t record = {
        .magic = PROGRAM_STATE_MAGIC,
        .version = PROGRAM_STATE_VERSION,
        .size = sizeof(program_nvs_record_t),
        .enabled = state->enabled ? 1U : 0U,
        .schedule_enabled = state->schedule_enabled ? 1U : 0U,
        .burst_enabled = state->burst_enabled ? 1U : 0U,
        .schedule_start_minute = state->schedule_start_minute,
        .schedule_stop_minute = state->schedule_stop_minute,
        .schedule_days = state->schedule_days,
        .schedule_start_day = state->schedule_start_day,
        .interval_ms = state->interval_ms,
    };

    /* NVS commit is the durability boundary used by power-loss recovery. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(PROGRAM_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_blob(handle, PROGRAM_STATE_KEY, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    xSemaphoreGive(s_mutex);
    return err;
}
