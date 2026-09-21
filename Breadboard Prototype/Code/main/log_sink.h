/* Encrypted SD log lifecycle; append calls are serialized internally. */

#pragma once

#include "esp_err.h"

esp_err_t log_sink_init(const char *path, const char *password);
esp_err_t log_sink_append(const char *line);
void log_sink_deinit(void);
