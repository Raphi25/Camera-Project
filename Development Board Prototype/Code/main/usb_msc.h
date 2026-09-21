/* Exclusive SD-card handoff to the FireBeetle 2 ESP32-P4 OTG USB port. */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"
esp_err_t usb_msc_start(sdmmc_card_t *card);
esp_err_t usb_msc_stop(void);
esp_err_t usb_msc_init_detection(void);
bool usb_msc_is_active(void);
bool usb_msc_is_connected(void);
