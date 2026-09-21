/* Read-only access to credentials initialized from encrypted NVS. */

#pragma once

#include "esp_err.h"

esp_err_t device_credentials_init(void);
const char *device_credentials_media_password(void);
const char *device_credentials_log_password(void);
