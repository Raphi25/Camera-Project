/* DS3231 register access, alarms, oscillator health, and temperature readings. */

#include <string.h>

#include "esp_check.h"

#include "rtc.h"

#define RTC_STAT_OSC_STOP      0x80
#define RTC_STAT_ALARM_2       0x02
#define RTC_STAT_ALARM_1       0x01

#define RTC_CTRL_BBSQW         0x40
#define RTC_CTRL_INTCN         0x04
#define RTC_CTRL_ALARM2_INT    0x02
#define RTC_CTRL_ALARM1_INT    0x01

#define RTC_ALARM_WDAY         0x40
#define RTC_ALARM_NOTSET       0x80

#define RTC_ADDR_TIME          0x00
#define RTC_ADDR_ALARM1        0x07
#define RTC_ADDR_ALARM2        0x0b
#define RTC_ADDR_CONTROL       0x0e
#define RTC_ADDR_STATUS        0x0f
#define RTC_ADDR_TEMP          0x11

#define RTC_12HOUR_FLAG        0x40
#define RTC_12HOUR_MASK        0x1f
#define RTC_PM_FLAG            0x20
#define RTC_MONTH_MASK         0x1f

#define RTC_RW_TIMEOUT_MS      100

static inline uint8_t dec2bcd(uint8_t val)
{
    return (uint8_t)(((val / 10U) << 4) | (val % 10U));
}

static inline uint8_t bcd2dec(uint8_t val)
{
    return (uint8_t)(((val >> 4) * 10U) + (val & 0x0FU));
}

static esp_err_t rtc_write_reg(rtc_t *rtc, uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t write_buf[8] = {0};

    if (!rtc || !rtc->dev || len > 7) {
        return ESP_ERR_INVALID_ARG;
    }

    write_buf[0] = reg;
    memcpy(&write_buf[1], data, len);
    return i2c_master_transmit(rtc->dev, write_buf, len + 1, RTC_RW_TIMEOUT_MS);
}

static esp_err_t rtc_read_reg(rtc_t *rtc, uint8_t reg, uint8_t *data, size_t len)
{
    if (!rtc || !rtc->dev || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_transmit_receive(rtc->dev, &reg, 1, data, len, RTC_RW_TIMEOUT_MS);
}

esp_err_t rtc_init(rtc_t *rtc, i2c_port_t port, gpio_num_t sda_gpio, gpio_num_t scl_gpio, uint32_t scl_speed_hz)
{
    if (!rtc) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = port,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &rtc->bus), "rtc", "i2c_new_master_bus failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_I2C_ADDRESS,
        .scl_speed_hz = scl_speed_hz,
    };

    esp_err_t err = i2c_master_bus_add_device(rtc->bus, &dev_cfg, &rtc->dev);
    if (err != ESP_OK) {
        i2c_del_master_bus(rtc->bus);
        rtc->bus = NULL;
    }
    return err;
}

void rtc_deinit(rtc_t *rtc)
{
    if (!rtc) {
        return;
    }

    if (rtc->dev) {
        i2c_master_bus_rm_device(rtc->dev);
        rtc->dev = NULL;
    }
    if (rtc->bus) {
        i2c_del_master_bus(rtc->bus);
        rtc->bus = NULL;
    }
}

esp_err_t rtc_set_time(rtc_t *rtc, const struct tm *time)
{
    uint8_t data[7] = {0};

    if (!rtc || !time || time->tm_year < 100 || time->tm_year > 199) {
        return ESP_ERR_INVALID_ARG;
    }

    data[0] = dec2bcd((uint8_t)time->tm_sec);
    data[1] = dec2bcd((uint8_t)time->tm_min);
    data[2] = dec2bcd((uint8_t)time->tm_hour);
    data[3] = dec2bcd((uint8_t)(time->tm_wday + 1));
    data[4] = dec2bcd((uint8_t)time->tm_mday);
    data[5] = dec2bcd((uint8_t)(time->tm_mon + 1));
    data[6] = dec2bcd((uint8_t)(time->tm_year - 100));

    return rtc_write_reg(rtc, RTC_ADDR_TIME, data, sizeof(data));
}

esp_err_t rtc_get_time(rtc_t *rtc, struct tm *time)
{
    uint8_t data[7] = {0};
    esp_err_t err;

    if (!rtc || !time) {
        return ESP_ERR_INVALID_ARG;
    }

    err = rtc_read_reg(rtc, RTC_ADDR_TIME, data, sizeof(data));
    if (err != ESP_OK) {
        return err;
    }

    time->tm_sec = bcd2dec(data[0] & 0x7f);
    time->tm_min = bcd2dec(data[1] & 0x7f);
    if (data[2] & RTC_12HOUR_FLAG) {
        time->tm_hour = bcd2dec(data[2] & RTC_12HOUR_MASK) % 12;
        if (data[2] & RTC_PM_FLAG) {
            time->tm_hour += 12;
        }
    } else {
        time->tm_hour = bcd2dec(data[2] & 0x3f);
    }
    time->tm_wday = bcd2dec(data[3]) - 1;
    time->tm_mday = bcd2dec(data[4] & 0x3f);
    time->tm_mon = bcd2dec(data[5] & RTC_MONTH_MASK) - 1;
    time->tm_year = bcd2dec(data[6]) + 100;
    time->tm_isdst = 0;
    return ESP_OK;
}

esp_err_t rtc_set_alarm(rtc_t *rtc, rtc_alarm_t alarms,
                           const struct tm *time1, rtc_alarm1_rate_t option1,
                           const struct tm *time2, rtc_alarm2_rate_t option2)
{
    uint8_t ctrl_reg = 0;
    uint8_t status_reg = 0;
    esp_err_t err = rtc_read_reg(rtc, RTC_ADDR_CONTROL, &ctrl_reg, 1);
    if (err != ESP_OK) {
        return err;
    }

    err = rtc_read_reg(rtc, RTC_ADDR_STATUS, &status_reg, 1);
    if (err != ESP_OK) {
        return err;
    }

    ctrl_reg |= RTC_CTRL_INTCN;

    if (alarms == RTC_ALARM_1 || alarms == RTC_ALARM_BOTH) {
        uint8_t data[4] = {0};
        if (!time1) {
            return ESP_ERR_INVALID_ARG;
        }

        data[0] = dec2bcd((uint8_t)time1->tm_sec);
        data[1] = dec2bcd((uint8_t)time1->tm_min);
        data[2] = dec2bcd((uint8_t)time1->tm_hour);
        data[3] = dec2bcd((uint8_t)(time1->tm_wday + 1));

        switch (option1) {
            case RTC_ALARM1_EVERY_SECOND:
                data[0] |= RTC_ALARM_NOTSET;
                data[1] |= RTC_ALARM_NOTSET;
                data[2] |= RTC_ALARM_NOTSET;
                data[3] |= RTC_ALARM_NOTSET;
                break;
            case RTC_ALARM1_MATCH_SEC:
                data[1] |= RTC_ALARM_NOTSET;
                data[2] |= RTC_ALARM_NOTSET;
                data[3] |= RTC_ALARM_NOTSET;
                break;
            case RTC_ALARM1_MATCH_SECMIN:
                data[2] |= RTC_ALARM_NOTSET;
                data[3] |= RTC_ALARM_NOTSET;
                break;
            case RTC_ALARM1_MATCH_SECMINHOUR:
                data[3] |= RTC_ALARM_NOTSET;
                break;
            case RTC_ALARM1_MATCH_SECMINHOURDAY:
                data[3] |= RTC_ALARM_WDAY;
                break;
            case RTC_ALARM1_MATCH_SECMINHOURDATE:
                data[3] = dec2bcd((uint8_t)time1->tm_mday);
                break;
        }

        ESP_RETURN_ON_ERROR(rtc_write_reg(rtc, RTC_ADDR_ALARM1, data, sizeof(data)), "rtc", "write alarm1 failed");
        ctrl_reg |= RTC_CTRL_ALARM1_INT;
        status_reg &= (uint8_t)~RTC_STAT_ALARM_1;
    } else {
        ctrl_reg &= (uint8_t)~RTC_CTRL_ALARM1_INT;
    }

    if (alarms == RTC_ALARM_2 || alarms == RTC_ALARM_BOTH) {
        uint8_t data[3] = {0};
        if (!time2) {
            return ESP_ERR_INVALID_ARG;
        }

        data[0] = dec2bcd((uint8_t)time2->tm_min);
        data[1] = dec2bcd((uint8_t)time2->tm_hour);
        data[2] = dec2bcd((uint8_t)(time2->tm_wday + 1));

        switch (option2) {
            case RTC_ALARM2_EVERY_MIN:
                data[0] |= RTC_ALARM_NOTSET;
                data[1] |= RTC_ALARM_NOTSET;
                data[2] |= RTC_ALARM_NOTSET;
                break;
            case RTC_ALARM2_MATCH_MIN:
                data[1] |= RTC_ALARM_NOTSET;
                data[2] |= RTC_ALARM_NOTSET;
                break;
            case RTC_ALARM2_MATCH_MINHOUR:
                data[2] |= RTC_ALARM_NOTSET;
                break;
            case RTC_ALARM2_MATCH_MINHOURDAY:
                data[2] |= RTC_ALARM_WDAY;
                break;
            case RTC_ALARM2_MATCH_MINHOURDATE:
                data[2] = dec2bcd((uint8_t)time2->tm_mday);
                break;
        }

        ESP_RETURN_ON_ERROR(rtc_write_reg(rtc, RTC_ADDR_ALARM2, data, sizeof(data)), "rtc", "write alarm2 failed");
        ctrl_reg |= RTC_CTRL_ALARM2_INT;
        status_reg &= (uint8_t)~RTC_STAT_ALARM_2;
    } else {
        ctrl_reg &= (uint8_t)~RTC_CTRL_ALARM2_INT;
    }

    ESP_RETURN_ON_ERROR(rtc_write_reg(rtc, RTC_ADDR_STATUS, &status_reg, 1), "rtc", "write status failed");
    return rtc_write_reg(rtc, RTC_ADDR_CONTROL, &ctrl_reg, 1);
}

esp_err_t rtc_clear_alarm_flags(rtc_t *rtc, rtc_alarm_t alarms)
{
    uint8_t status_reg = 0;
    ESP_RETURN_ON_ERROR(rtc_read_reg(rtc, RTC_ADDR_STATUS, &status_reg, 1), "rtc", "read status failed");

    if (alarms == RTC_ALARM_1 || alarms == RTC_ALARM_BOTH) {
        status_reg &= (uint8_t)~RTC_STAT_ALARM_1;
    }
    if (alarms == RTC_ALARM_2 || alarms == RTC_ALARM_BOTH) {
        status_reg &= (uint8_t)~RTC_STAT_ALARM_2;
    }

    return rtc_write_reg(rtc, RTC_ADDR_STATUS, &status_reg, 1);
}

esp_err_t rtc_check_alarm_flags(rtc_t *rtc, rtc_alarm_t *alarms)
{
    uint8_t status_reg = 0;

    if (!rtc || !alarms) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(rtc_read_reg(rtc, RTC_ADDR_STATUS, &status_reg, 1), "rtc", "read status failed");

    *alarms = RTC_ALARM_NONE;
    if (status_reg & RTC_STAT_ALARM_1) {
        *alarms = RTC_ALARM_1;
    }
    if (status_reg & RTC_STAT_ALARM_2) {
        *alarms = (*alarms == RTC_ALARM_1) ? RTC_ALARM_BOTH : RTC_ALARM_2;
    }
    return ESP_OK;
}

esp_err_t rtc_get_oscillator_stop_flag(rtc_t *rtc, bool *stopped)
{
    uint8_t status_reg = 0;

    if (!rtc || !stopped) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(rtc_read_reg(rtc, RTC_ADDR_STATUS, &status_reg, 1), "rtc", "read status failed");
    *stopped = (status_reg & RTC_STAT_OSC_STOP) != 0;
    return ESP_OK;
}

esp_err_t rtc_clear_oscillator_stop_flag(rtc_t *rtc)
{
    uint8_t status_reg = 0;

    ESP_RETURN_ON_ERROR(rtc_read_reg(rtc, RTC_ADDR_STATUS, &status_reg, 1), "rtc", "read status failed");
    status_reg &= (uint8_t)~RTC_STAT_OSC_STOP;
    return rtc_write_reg(rtc, RTC_ADDR_STATUS, &status_reg, 1);
}

esp_err_t rtc_set_squarewave(rtc_t *rtc, bool enable, rtc_sqwave_freq_t freq)
{
    uint8_t ctrl_reg = 0;
    ESP_RETURN_ON_ERROR(rtc_read_reg(rtc, RTC_ADDR_CONTROL, &ctrl_reg, 1), "rtc", "read control failed");

    ctrl_reg &= (uint8_t)~(RTC_CTRL_INTCN | RTC_CTRL_ALARM1_INT | RTC_CTRL_ALARM2_INT | 0x18);
    if (enable) {
        ctrl_reg |= (uint8_t)freq;
        ctrl_reg |= RTC_CTRL_BBSQW;
    }

    return rtc_write_reg(rtc, RTC_ADDR_CONTROL, &ctrl_reg, 1);
}

esp_err_t rtc_get_temp_float(rtc_t *rtc, float *temp)
{
    uint8_t data[2] = {0};
    int16_t raw = 0;

    if (!rtc || !temp) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(rtc_read_reg(rtc, RTC_ADDR_TEMP, data, sizeof(data)), "rtc", "read temp failed");
    raw = (int16_t)((int16_t)(int8_t)data[0] << 2) | (data[1] >> 6);
    *temp = raw * 0.25f;
    return ESP_OK;
}
