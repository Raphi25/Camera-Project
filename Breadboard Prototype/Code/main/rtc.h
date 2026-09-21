/* Shared-I2C DS3231 lifecycle and calendar/alarm API. */

#ifndef RTC_H_
#define RTC_H_

#include <stdbool.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#define RTC_I2C_ADDRESS 0x68

typedef enum {
    RTC_ALARM_NONE = 0,
    RTC_ALARM_1,
    RTC_ALARM_2,
    RTC_ALARM_BOTH,
} rtc_alarm_t;

typedef enum {
    RTC_ALARM1_EVERY_SECOND = 0,
    RTC_ALARM1_MATCH_SEC,
    RTC_ALARM1_MATCH_SECMIN,
    RTC_ALARM1_MATCH_SECMINHOUR,
    RTC_ALARM1_MATCH_SECMINHOURDAY,
    RTC_ALARM1_MATCH_SECMINHOURDATE,
} rtc_alarm1_rate_t;

typedef enum {
    RTC_ALARM2_EVERY_MIN = 0,
    RTC_ALARM2_MATCH_MIN,
    RTC_ALARM2_MATCH_MINHOUR,
    RTC_ALARM2_MATCH_MINHOURDAY,
    RTC_ALARM2_MATCH_MINHOURDATE,
} rtc_alarm2_rate_t;

typedef enum {
    RTC_SQWAVE_1HZ = 0x00,
    RTC_SQWAVE_1024HZ = 0x08,
    RTC_SQWAVE_4096HZ = 0x10,
    RTC_SQWAVE_8192HZ = 0x18,
} rtc_sqwave_freq_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
} rtc_t;

esp_err_t rtc_init(rtc_t *rtc, i2c_port_t port, gpio_num_t sda_gpio, gpio_num_t scl_gpio, uint32_t scl_speed_hz);
void rtc_deinit(rtc_t *rtc);

esp_err_t rtc_set_time(rtc_t *rtc, const struct tm *time);
esp_err_t rtc_get_time(rtc_t *rtc, struct tm *time);

/* Program either alarm and route enabled alarm matches to the DS3231 INT pin. */
esp_err_t rtc_set_alarm(rtc_t *rtc, rtc_alarm_t alarms,
                           const struct tm *time1, rtc_alarm1_rate_t option1,
                           const struct tm *time2, rtc_alarm2_rate_t option2);
esp_err_t rtc_clear_alarm_flags(rtc_t *rtc, rtc_alarm_t alarms);
esp_err_t rtc_check_alarm_flags(rtc_t *rtc, rtc_alarm_t *alarms);

/* OSF indicates that battery-backed time may be invalid after oscillator loss. */
esp_err_t rtc_get_oscillator_stop_flag(rtc_t *rtc, bool *stopped);
esp_err_t rtc_clear_oscillator_stop_flag(rtc_t *rtc);

esp_err_t rtc_set_squarewave(rtc_t *rtc, bool enable, rtc_sqwave_freq_t freq);
esp_err_t rtc_get_temp_float(rtc_t *rtc, float *temp);

#endif
