/* Health status snapshot and persistent fault-recording API. */

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_system.h"

typedef struct {
    uint32_t boot_count;
    esp_reset_reason_t reset_reason;
    esp_err_t last_error;
    char last_subsystem[24];
} health_diag_status_t;

esp_err_t health_diag_init(void);
esp_err_t health_diag_record_fault(const char *subsystem, esp_err_t error);
void health_diag_get_status(health_diag_status_t *status);
