/* Persistent fault counters and last-failure diagnostics. */

#include "health_diag.h"

#include <string.h>

#include "nvs.h"
#include "event_breadcrumbs.h"

#define HEALTH_NAMESPACE "health"

static health_diag_status_t s_status;

esp_err_t health_diag_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(HEALTH_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    (void)nvs_get_u32(handle, "boots", &s_status.boot_count);
    s_status.boot_count++;
    s_status.reset_reason = esp_reset_reason();

    int32_t saved_error = ESP_OK;
    (void)nvs_get_i32(handle, "last_err", &saved_error);
    s_status.last_error = (esp_err_t)saved_error;
    size_t subsystem_size = sizeof(s_status.last_subsystem);
    if (nvs_get_str(handle, "last_sub", s_status.last_subsystem, &subsystem_size) != ESP_OK) {
        strcpy(s_status.last_subsystem, "none");
    }
    if (s_status.reset_reason == ESP_RST_TASK_WDT ||
        s_status.reset_reason == ESP_RST_INT_WDT ||
        s_status.reset_reason == ESP_RST_WDT) {
        strcpy(s_status.last_subsystem, "watchdog_reset");
        s_status.last_error = ESP_ERR_TIMEOUT;
        (void)nvs_set_i32(handle, "last_err", (int32_t)s_status.last_error);
        (void)nvs_set_str(handle, "last_sub", s_status.last_subsystem);
    }

    err = nvs_set_u32(handle, "boots", s_status.boot_count);
    if (err == ESP_OK) err = nvs_set_u8(handle, "reset", (uint8_t)s_status.reset_reason);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t health_diag_record_fault(const char *subsystem, esp_err_t error)
{
    if (subsystem == NULL || *subsystem == '\0') return ESP_ERR_INVALID_ARG;
    event_breadcrumb_record(BREADCRUMB_FAULT, (int32_t)error);
    strlcpy(s_status.last_subsystem, subsystem, sizeof(s_status.last_subsystem));
    s_status.last_error = error;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(HEALTH_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(handle, "last_err", (int32_t)error);
    if (err == ESP_OK) err = nvs_set_str(handle, "last_sub", s_status.last_subsystem);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

void health_diag_get_status(health_diag_status_t *status)
{
    if (status != NULL) *status = s_status;
}
