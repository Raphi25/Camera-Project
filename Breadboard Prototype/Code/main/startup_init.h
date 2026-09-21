/* Startup initialization and controlled-restart API. */

#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t startup_init_camera_mode(void);
uint8_t startup_camera_mode_fps(void);
const char *startup_camera_mode_name(uint8_t fps);
esp_err_t startup_select_camera_mode(uint8_t fps);
