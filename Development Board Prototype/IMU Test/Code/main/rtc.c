#include "rtc.h"

#include <string.h>

#include "esp_check.h"

#define RTC_ADDRESS 0x68
#define RTC_TIME_REGISTER 0x00
#define RTC_STATUS_REGISTER 0x0f
#define RTC_OSCILLATOR_STOP 0x80
#define RTC_TIMEOUT_MS 100

static uint8_t dec_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

static uint8_t bcd_to_dec(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10U) + (value & 0x0fU));
}

static esp_err_t read_register(rtc_t *rtc, uint8_t reg, uint8_t *data, size_t size)
{
    if (rtc == NULL || rtc->dev == NULL || data == NULL) return ESP_ERR_INVALID_ARG;
    return i2c_master_transmit_receive(rtc->dev, &reg, 1, data, size, RTC_TIMEOUT_MS);
}

static esp_err_t write_register(rtc_t *rtc, uint8_t reg, const uint8_t *data, size_t size)
{
    if (rtc == NULL || rtc->dev == NULL || data == NULL || size > 7) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buffer[8] = {reg};
    memcpy(buffer + 1, data, size);
    return i2c_master_transmit(rtc->dev, buffer, size + 1, RTC_TIMEOUT_MS);
}

esp_err_t rtc_init(rtc_t *rtc, i2c_port_t port, gpio_num_t sda_gpio,
                   gpio_num_t scl_gpio, uint32_t speed_hz)
{
    if (rtc == NULL) return ESP_ERR_INVALID_ARG;
    memset(rtc, 0, sizeof(*rtc));
    i2c_master_bus_config_t bus_config = {
        .i2c_port = port,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &rtc->bus), "rtc", "create bus");
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDRESS,
        .scl_speed_hz = speed_hz,
    };
    esp_err_t error = i2c_master_bus_add_device(rtc->bus, &device_config, &rtc->dev);
    if (error != ESP_OK) {
        i2c_del_master_bus(rtc->bus);
        rtc->bus = NULL;
    }
    return error;
}

esp_err_t rtc_get_time(rtc_t *rtc, struct tm *value)
{
    if (value == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t data[7] = {0};
    ESP_RETURN_ON_ERROR(read_register(rtc, RTC_TIME_REGISTER, data, sizeof(data)),
                        "rtc", "read time");
    memset(value, 0, sizeof(*value));
    value->tm_sec = bcd_to_dec(data[0] & 0x7fU);
    value->tm_min = bcd_to_dec(data[1] & 0x7fU);
    if (data[2] & 0x40U) {
        value->tm_hour = bcd_to_dec(data[2] & 0x1fU) % 12;
        if (data[2] & 0x20U) value->tm_hour += 12;
    } else {
        value->tm_hour = bcd_to_dec(data[2] & 0x3fU);
    }
    value->tm_wday = bcd_to_dec(data[3] & 0x07U) - 1;
    value->tm_mday = bcd_to_dec(data[4] & 0x3fU);
    value->tm_mon = bcd_to_dec(data[5] & 0x1fU) - 1;
    value->tm_year = bcd_to_dec(data[6]) + 100;
    value->tm_isdst = 0;
    return ESP_OK;
}

esp_err_t rtc_set_time(rtc_t *rtc, const struct tm *value)
{
    if (value == NULL || value->tm_year < 100 || value->tm_year > 199) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t data[7] = {
        dec_to_bcd((uint8_t)value->tm_sec), dec_to_bcd((uint8_t)value->tm_min),
        dec_to_bcd((uint8_t)value->tm_hour), dec_to_bcd((uint8_t)(value->tm_wday + 1)),
        dec_to_bcd((uint8_t)value->tm_mday), dec_to_bcd((uint8_t)(value->tm_mon + 1)),
        dec_to_bcd((uint8_t)(value->tm_year - 100)),
    };
    return write_register(rtc, RTC_TIME_REGISTER, data, sizeof(data));
}

esp_err_t rtc_get_oscillator_stop_flag(rtc_t *rtc, bool *stopped)
{
    if (stopped == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(read_register(rtc, RTC_STATUS_REGISTER, &status, 1),
                        "rtc", "read status");
    *stopped = (status & RTC_OSCILLATOR_STOP) != 0;
    return ESP_OK;
}

esp_err_t rtc_clear_oscillator_stop_flag(rtc_t *rtc)
{
    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(read_register(rtc, RTC_STATUS_REGISTER, &status, 1),
                        "rtc", "read status");
    status &= (uint8_t)~RTC_OSCILLATOR_STOP;
    return write_register(rtc, RTC_STATUS_REGISTER, &status, 1);
}
