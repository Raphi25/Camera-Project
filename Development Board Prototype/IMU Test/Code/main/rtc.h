#pragma once

#include <stdbool.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
} rtc_t;

/* The RTC owns a dedicated I2C bus so its slow transactions cannot delay the
 * BMI323 data-ready reads on the acquisition bus. */
esp_err_t rtc_init(rtc_t *rtc, i2c_port_t port, gpio_num_t sda_gpio,
                   gpio_num_t scl_gpio, uint32_t speed_hz);
esp_err_t rtc_get_time(rtc_t *rtc, struct tm *value);
esp_err_t rtc_set_time(rtc_t *rtc, const struct tm *value);
esp_err_t rtc_get_oscillator_stop_flag(rtc_t *rtc, bool *stopped);
esp_err_t rtc_clear_oscillator_stop_flag(rtc_t *rtc);
