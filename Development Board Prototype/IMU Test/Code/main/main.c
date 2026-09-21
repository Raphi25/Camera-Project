#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/sdmmc_host.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "rtc.h"

#define IMU_SDA_GPIO GPIO_NUM_20
#define IMU_SCL_GPIO GPIO_NUM_36
#define IMU_INT1_GPIO GPIO_NUM_22
#define IMU_I2C_SPEED_HZ 100000
#define IMU_ODR_HZ 100
#define IMU_EXPECTED_PERIOD_US (1000000 / IMU_ODR_HZ)
#define IMU_DRDY_TIMEOUT_MS 1000
#define IMU_ADDRESS_OPEN 0x69
#define IMU_ADDRESS_SHORT 0x68
#define BMI323_REG_CHIP_ID 0x00
#define BMI323_REG_ACC_DATA_X 0x03
#define BMI323_REG_INT_STATUS_INT1 0x0D
#define BMI323_REG_ACC_CONF 0x20
#define BMI323_REG_GYR_CONF 0x21
#define BMI323_REG_IO_INT_CTRL 0x38
#define BMI323_REG_INT_CONF 0x39
#define BMI323_REG_INT_MAP2 0x3B
#define BMI323_CHIP_ID 0x43
#define BMI323_DUMMY_BYTES 2
#define BMI323_INT1_ACC_DRDY (1U << 13)

#define RTC_SDA_GPIO GPIO_NUM_7
#define RTC_SCL_GPIO GPIO_NUM_8
#define RTC_I2C_SPEED_HZ 100000

#define SD_PIN_CLK GPIO_NUM_43
#define SD_PIN_CMD GPIO_NUM_44
#define SD_PIN_D0 GPIO_NUM_39
#define SD_PIN_D1 GPIO_NUM_40
#define SD_PIN_D2 GPIO_NUM_41
#define SD_PIN_D3 GPIO_NUM_42
#define SD_MOUNT_POINT "/sdcard"
#define SD_CAPTURE_DIR SD_MOUNT_POINT "/captures"
#define SD_POWER_GPIO GPIO_NUM_45
#define SD_POWER_ON_LEVEL 0
#define SD_POWER_OFF_LEVEL 1

#define LOG_BUFFER_SIZE 8192
#define LOG_FLUSH_INTERVAL_US 1000000
#define LOG_FLUSH_SAMPLE_COUNT 100
#define USB_TX_DRAIN_TIMEOUT_MS 5000
#define USB_TX_DRAIN_RETRIES 6
#define LOG_PATH_SIZE 96

typedef struct {
    volatile uint32_t interrupts;
    volatile uint32_t samples;
    volatile uint32_t i2c_errors;
    volatile uint32_t write_errors;
    volatile uint32_t invalid_samples;
    volatile uint32_t missed_samples;
    volatile uint32_t wait_timeouts;
    volatile uint32_t flushes;
} logger_stats_t;

static const char *TAG = "imu_test";
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_imu;
static SemaphoreHandle_t s_i2c_mutex;
static SemaphoreHandle_t s_rtc_mutex;
static rtc_t s_rtc;
static bool s_rtc_available;
static bool s_rtc_valid;
static volatile bool s_logging;
static volatile bool s_log_opened;
static TaskHandle_t s_logger_task;
static sdmmc_card_t *s_card;
static bool s_sd_mounted;
static uint8_t s_imu_address;
static logger_stats_t s_stats;
static char s_current_log_path[LOG_PATH_SIZE] = "none";
static char s_log_buffer[LOG_BUFFER_SIZE];

static bool calendar_valid(const struct tm *value)
{
    if (value == NULL || value->tm_year < 100 || value->tm_year > 199 ||
        value->tm_mon < 0 || value->tm_mon > 11 || value->tm_hour < 0 ||
        value->tm_hour > 23 || value->tm_min < 0 || value->tm_min > 59 ||
        value->tm_sec < 0 || value->tm_sec > 59 || value->tm_mday < 1) return false;
    static const uint8_t days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int year = value->tm_year + 1900;
    int limit = days_per_month[value->tm_mon];
    if (value->tm_mon == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        limit = 29;
    }
    return value->tm_mday <= limit;
}

static int64_t calendar_epoch_seconds(const struct tm *value)
{
    /* Convert the RTC's timezone-free calendar value without depending on the
     * process TZ setting used by mktime(). */
    int year = value->tm_year + 1900;
    unsigned month = (unsigned)value->tm_mon + 1U;
    unsigned day = (unsigned)value->tm_mday;
    year -= month <= 2U;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned year_of_era = (unsigned)(year - era * 400);
    int adjusted_month = (int)month + (month > 2U ? -3 : 9);
    unsigned day_of_year = (153U * (unsigned)adjusted_month + 2U) / 5U + day - 1U;
    unsigned day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    int64_t days = (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
    return days * 86400 + value->tm_hour * 3600 + value->tm_min * 60 + value->tm_sec;
}

static bool rtc_read_valid(struct tm *value)
{
    if (!s_rtc_available || !s_rtc_valid || value == NULL) return false;
    xSemaphoreTake(s_rtc_mutex, portMAX_DELAY);
    esp_err_t error = rtc_get_time(&s_rtc, value);
    xSemaphoreGive(s_rtc_mutex);
    if (error != ESP_OK || !calendar_valid(value)) {
        s_rtc_valid = false;
        return false;
    }
    return true;
}

static void rtc_initialize(void)
{
    esp_err_t error = rtc_init(&s_rtc, I2C_NUM_0, RTC_SDA_GPIO, RTC_SCL_GPIO,
                               RTC_I2C_SPEED_HZ);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "DS3231 unavailable on SDA=%d SCL=%d: %s",
                 RTC_SDA_GPIO, RTC_SCL_GPIO, esp_err_to_name(error));
        return;
    }
    s_rtc_available = true;
    bool stopped = true;
    struct tm now = {0};
    if (rtc_get_oscillator_stop_flag(&s_rtc, &stopped) == ESP_OK && !stopped &&
        rtc_get_time(&s_rtc, &now) == ESP_OK && calendar_valid(&now)) {
        s_rtc_valid = true;
        ESP_LOGI(TAG, "DS3231 ready: %04d-%02d-%02d %02d:%02d:%02d",
                 now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                 now.tm_hour, now.tm_min, now.tm_sec);
    } else {
        ESP_LOGW(TAG, "DS3231 time is invalid; sync it from the GUI");
    }
}

static int16_t le_i16(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint16_t le_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static esp_err_t imu_read(uint8_t reg, uint8_t *data, size_t length)
{
    uint8_t response[26] = {0};
    if (length > sizeof(response) - BMI323_DUMMY_BYTES) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = i2c_master_transmit_receive(s_imu, &reg, 1, response,
                                                 length + BMI323_DUMMY_BYTES, 100);
    if (err == ESP_OK) memcpy(data, &response[BMI323_DUMMY_BYTES], length);
    return err;
}

static esp_err_t imu_write(uint8_t reg, const uint8_t *data, size_t length)
{
    uint8_t buffer[3] = {reg, 0, 0};
    if (length > 2) return ESP_ERR_INVALID_SIZE;
    memcpy(&buffer[1], data, length);
    return i2c_master_transmit(s_imu, buffer, length + 1, 100);
}

static esp_err_t imu_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = IMU_SDA_GPIO,
        .scl_io_num = IMU_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_i2c_bus), TAG, "I2C bus");

    const uint8_t addresses[] = {IMU_ADDRESS_OPEN, IMU_ADDRESS_SHORT};
    for (size_t address_index = 0; address_index < 2; address_index++) {
        uint8_t address = addresses[address_index];
        if (i2c_master_probe(s_i2c_bus, address, 100) != ESP_OK) continue;
        i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = IMU_I2C_SPEED_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &device_config, &s_imu), TAG,
                            "BMI323 device");
        uint8_t chip_id[2] = {0};
        ESP_RETURN_ON_ERROR(imu_read(BMI323_REG_CHIP_ID, chip_id, sizeof(chip_id)), TAG,
                            "BMI323 chip ID");
        if (chip_id[0] != BMI323_CHIP_ID) {
            i2c_master_bus_rm_device(s_imu);
            s_imu = NULL;
            continue;
        }
        s_imu_address = address;
        const uint8_t accel_config[2] = {0xA8, 0x70}; /* 100 Hz, +/-8 g, high performance */
        const uint8_t gyro_config[2] = {0xC8, 0x70};  /* 100 Hz, +/-2000 dps, high performance */
        ESP_RETURN_ON_ERROR(imu_write(BMI323_REG_ACC_CONF, accel_config, 2), TAG, "accel config");
        ESP_RETURN_ON_ERROR(imu_write(BMI323_REG_GYR_CONF, gyro_config, 2), TAG, "gyro config");
        ESP_LOGI(TAG, "BMI323 ready at 0x%02x, SDA=%d SCL=%d INT1=%d",
                 address, IMU_SDA_GPIO, IMU_SCL_GPIO, IMU_INT1_GPIO);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static void IRAM_ATTR imu_data_ready_isr(void *arg)
{
    (void)arg;
    TaskHandle_t task = s_logger_task;
    if (task == NULL) return;
    s_stats.interrupts++;
    BaseType_t higher_priority_woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &higher_priority_woken);
    if (higher_priority_woken) portYIELD_FROM_ISR();
}

static esp_err_t imu_data_ready_init(void)
{
    gpio_config_t interrupt_gpio_config = {
        .pin_bit_mask = 1ULL << IMU_INT1_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&interrupt_gpio_config), TAG, "INT1 GPIO");
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "GPIO ISR service");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(IMU_INT1_GPIO, imu_data_ready_isr, NULL), TAG,
                        "INT1 handler");

    const uint8_t io_int_ctrl[2] = {0x05, 0x00}; /* INT1 enabled, push-pull, active high */
    const uint8_t int_conf[2] = {0x01, 0x00};    /* latched until status is read */
    const uint8_t int_map2[2] = {0x00, 0x04};    /* accelerometer DRDY -> INT1 */
    ESP_RETURN_ON_ERROR(imu_write(BMI323_REG_IO_INT_CTRL, io_int_ctrl, 2), TAG, "INT1 config");
    ESP_RETURN_ON_ERROR(imu_write(BMI323_REG_INT_CONF, int_conf, 2), TAG, "interrupt latch");
    ESP_RETURN_ON_ERROR(imu_write(BMI323_REG_INT_MAP2, int_map2, 2), TAG, "data-ready map");
    ESP_LOGI(TAG, "BMI323 data-ready interrupt enabled on GPIO%d at %d Hz",
             IMU_INT1_GPIO, IMU_ODR_HZ);
    return ESP_OK;
}

static esp_err_t sd_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    gpio_config_t power_config = {
        .pin_bit_mask = 1ULL << SD_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&power_config), TAG, "SD power GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(SD_POWER_GPIO, SD_POWER_ON_LEVEL), TAG, "SD power on");
    vTaskDelay(pdMS_TO_TICKS(20));

    sd_pwr_ctrl_ldo_config_t ldo_config = {.ldo_chan_id = 4};
    sd_pwr_ctrl_handle_t ldo = NULL;
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &ldo), TAG, "SD LDO");

    /* Some cards and breadboard layouts are unreliable at high speed. Try the
     * fastest configuration first, then progressively safer fallbacks. */
    const int widths[] = {4, 1, 1, 1};
    const int frequencies[] = {
        SDMMC_FREQ_HIGHSPEED, SDMMC_FREQ_HIGHSPEED,
        SDMMC_FREQ_DEFAULT, SDMMC_FREQ_PROBING,
    };
    esp_err_t mount_error = ESP_FAIL;
    for (size_t attempt = 0; attempt < 4; attempt++) {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        host.max_freq_khz = frequencies[attempt];
        host.pwr_ctrl_handle = ldo;
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = widths[attempt];
        slot.clk = SD_PIN_CLK;
        slot.cmd = SD_PIN_CMD;
        slot.d0 = SD_PIN_D0;
        slot.d1 = SD_PIN_D1;
        slot.d2 = SD_PIN_D2;
        slot.d3 = SD_PIN_D3;
        slot.cd = SDMMC_SLOT_NO_CD;
        slot.wp = SDMMC_SLOT_NO_WP;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        ESP_LOGI(TAG, "Trying SD mount: %d-bit, %d kHz", widths[attempt], frequencies[attempt]);
        mount_error = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                               &mount_config, &s_card);
        if (mount_error == ESP_OK) break;
        ESP_LOGW(TAG, "SD mount attempt %u failed: %s", (unsigned)(attempt + 1),
                 esp_err_to_name(mount_error));
    }
    if (mount_error != ESP_OK) {
        (void)sd_pwr_ctrl_del_on_chip_ldo(ldo);
        (void)gpio_set_level(SD_POWER_GPIO, SD_POWER_OFF_LEVEL);
        return mount_error;
    }
    if (mkdir(SD_CAPTURE_DIR, 0775) != 0 && errno != EEXIST) return ESP_FAIL;
    s_sd_mounted = true;
    ESP_LOGI(TAG, "SD ready; run files are stored in %s", SD_CAPTURE_DIR);
    return ESP_OK;
}

static bool next_log_path(char *path, size_t path_size, struct tm *run_time,
                          bool *rtc_timestamped)
{
    /* Prefer human-readable RTC names. The numbered fallback keeps logging
     * usable when the RTC battery is absent or the clock has not been synced. */
    *rtc_timestamped = rtc_read_valid(run_time);
    if (*rtc_timestamped) {
        for (unsigned suffix = 0; suffix < 100; suffix++) {
            int written = snprintf(
                path, path_size,
                SD_CAPTURE_DIR "/imu_%04d%02d%02d_%02d%02d%02d_%02u.csv",
                run_time->tm_year + 1900, run_time->tm_mon + 1, run_time->tm_mday,
                run_time->tm_hour, run_time->tm_min, run_time->tm_sec, suffix);
            if (written <= 0 || (size_t)written >= path_size) return false;
            struct stat info;
            if (stat(path, &info) != 0 && errno == ENOENT) return true;
        }
        return false;
    }

    uint32_t highest_run = 0;
    DIR *directory = opendir(SD_CAPTURE_DIR);
    if (directory == NULL) return false;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        unsigned int run_number = 0;
        char extra = '\0';
        if (sscanf(entry->d_name, "imu_run_%6u.csv%c", &run_number, &extra) == 1 &&
            run_number > highest_run) {
            highest_run = run_number;
        }
    }
    closedir(directory);
    int written = snprintf(path, path_size, SD_CAPTURE_DIR "/imu_run_%06" PRIu32 ".csv",
                           highest_run + 1);
    return written > 0 && (size_t)written < path_size;
}

static FILE *open_log(int64_t *wall_epoch_us, int64_t *anchor_monotonic_us,
                      bool *rtc_timestamped)
{
    /* Anchor wall time once per run. Per-sample timestamps then use the
     * monotonic timer, avoiding slow RTC reads in the 100 Hz acquisition path. */
    struct tm run_time = {0};
    if (!next_log_path(s_current_log_path, sizeof(s_current_log_path), &run_time,
                       rtc_timestamped)) return NULL;
    FILE *file = fopen(s_current_log_path, "w");
    if (file == NULL) return NULL;
    if (setvbuf(file, s_log_buffer, _IOFBF, sizeof(s_log_buffer)) != 0) {
        fclose(file);
        return NULL;
    }
    if (fprintf(file,
                "sample_index,timestamp_us,rtc_datetime,accel_x_g,accel_y_g,accel_z_g,"
                "gyro_x_dps,gyro_y_dps,gyro_z_dps\n") < 0 || fflush(file) != 0) {
        fclose(file);
        return NULL;
    }
    *anchor_monotonic_us = esp_timer_get_time();
    *wall_epoch_us = *rtc_timestamped ? calendar_epoch_seconds(&run_time) * 1000000LL : 0;
    return file;
}

static void logger_task(void *arg)
{
    /* One data-ready notification corresponds to one sensor sample. SD writes
     * remain buffered and are flushed periodically to limit latency and loss. */
    (void)arg;
    int64_t wall_epoch_us = 0;
    int64_t anchor_monotonic_us = 0;
    bool rtc_timestamped = false;
    FILE *file = open_log(&wall_epoch_us, &anchor_monotonic_us, &rtc_timestamped);
    if (file == NULL) {
        ESP_LOGE(TAG, "Cannot create a unique log in %s", SD_CAPTURE_DIR);
        s_logging = false;
        s_logger_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_log_opened = true;

    uint8_t status_data[2];
    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    (void)imu_read(BMI323_REG_INT_STATUS_INT1, status_data, sizeof(status_data));
    xSemaphoreGive(s_i2c_mutex);
    (void)ulTaskNotifyTake(pdTRUE, 0);

    int64_t last_sample_us = 0;
    int64_t last_flush_us = esp_timer_get_time();
    uint32_t samples_since_flush = 0;
    ESP_LOGI(TAG, "IMU logging started: %s", s_current_log_path);

    while (s_logging) {
        uint32_t notifications = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IMU_DRDY_TIMEOUT_MS));
        if (!s_logging) break;
        if (notifications == 0) {
            s_stats.wait_timeouts++;
            ESP_LOGW(TAG, "Timed out waiting for BMI323 data-ready interrupt");
            continue;
        }
        if (notifications > 1) s_stats.missed_samples += notifications - 1;

        uint8_t data[12] = {0};
        esp_err_t status_error;
        esp_err_t data_error = ESP_FAIL;
        xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
        status_error = imu_read(BMI323_REG_INT_STATUS_INT1, status_data, sizeof(status_data));
        if (status_error == ESP_OK && (le_u16(status_data) & BMI323_INT1_ACC_DRDY)) {
            data_error = imu_read(BMI323_REG_ACC_DATA_X, data, sizeof(data));
        }
        xSemaphoreGive(s_i2c_mutex);
        if (status_error != ESP_OK || data_error != ESP_OK) {
            s_stats.i2c_errors++;
            ESP_LOGW(TAG, "BMI323 interrupt/data read failed");
            continue;
        }

        int16_t acc_x = le_i16(&data[0]);
        int16_t acc_y = le_i16(&data[2]);
        int16_t acc_z = le_i16(&data[4]);
        int16_t gyr_x = le_i16(&data[6]);
        int16_t gyr_y = le_i16(&data[8]);
        int16_t gyr_z = le_i16(&data[10]);
        if (acc_x == INT16_MIN || acc_y == INT16_MIN || acc_z == INT16_MIN ||
            gyr_x == INT16_MIN || gyr_y == INT16_MIN || gyr_z == INT16_MIN) {
            s_stats.invalid_samples++;
            continue;
        }

        int64_t monotonic_us = esp_timer_get_time();
        if (last_sample_us != 0) {
            int64_t elapsed_us = monotonic_us - last_sample_us;
            if (elapsed_us > IMU_EXPECTED_PERIOD_US * 3 / 2) {
                uint32_t periods = (uint32_t)((elapsed_us + IMU_EXPECTED_PERIOD_US / 2) /
                                               IMU_EXPECTED_PERIOD_US);
                if (periods > 1) s_stats.missed_samples += periods - 1;
            }
        }
        last_sample_us = monotonic_us;

        int64_t timestamp_us = rtc_timestamped
                                   ? wall_epoch_us + (monotonic_us - anchor_monotonic_us)
                                   : monotonic_us;
        char rtc_datetime[32] = "";
        if (rtc_timestamped) {
            time_t seconds = (time_t)(timestamp_us / 1000000LL);
            unsigned microseconds = (unsigned)(timestamp_us % 1000000LL);
            struct tm sample_time = {0};
            gmtime_r(&seconds, &sample_time);
            char calendar_text[24] = "";
            strftime(calendar_text, sizeof(calendar_text), "%Y-%m-%dT%H:%M:%S",
                     &sample_time);
            snprintf(rtc_datetime, sizeof(rtc_datetime), "%s.%06u",
                     calendar_text, microseconds);
        }

        int result = fprintf(file, "%" PRIu32 ",%" PRId64 ",%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                             s_stats.samples, timestamp_us, rtc_datetime,
                             (double)acc_x * 8.0 / 32768.0,
                             (double)acc_y * 8.0 / 32768.0,
                             (double)acc_z * 8.0 / 32768.0,
                             (double)gyr_x * 2000.0 / 32768.0,
                             (double)gyr_y * 2000.0 / 32768.0,
                             (double)gyr_z * 2000.0 / 32768.0);
        if (result < 0) {
            s_stats.write_errors++;
            ESP_LOGE(TAG, "SD write failed; stopping log");
            s_logging = false;
            break;
        }
        s_stats.samples++;
        samples_since_flush++;

        if (samples_since_flush >= LOG_FLUSH_SAMPLE_COUNT ||
            monotonic_us - last_flush_us >= LOG_FLUSH_INTERVAL_US) {
            if (fflush(file) != 0) {
                s_stats.write_errors++;
                ESP_LOGE(TAG, "SD flush failed; stopping log");
                s_logging = false;
                break;
            }
            s_stats.flushes++;
            samples_since_flush = 0;
            last_flush_us = monotonic_us;
        }
    }

    if (fflush(file) != 0) s_stats.write_errors++;
    if (fclose(file) != 0) s_stats.write_errors++;
    s_logging = false;
    s_log_opened = false;
    s_logger_task = NULL;
    ESP_LOGI(TAG, "IMU logging stopped: samples=%" PRIu32 " missed=%" PRIu32
             " i2c_errors=%" PRIu32 " write_errors=%" PRIu32,
             s_stats.samples, s_stats.missed_samples, s_stats.i2c_errors, s_stats.write_errors);
    vTaskDelete(NULL);
}

static void start_logging(void)
{
    if (s_logging) return;
    if (!s_sd_mounted) {
        ESP_LOGE(TAG, "Cannot start logging without an SD card");
        return;
    }
    memset(&s_stats, 0, sizeof(s_stats));
    s_log_opened = false;
    s_logging = true;
    BaseType_t created = xTaskCreate(logger_task, "imu_logger", 5120, NULL, 5, &s_logger_task);
    if (created != pdPASS) {
        s_logging = false;
        s_logger_task = NULL;
        ESP_LOGE(TAG, "Failed to create IMU logger task");
        return;
    }
    for (unsigned attempt = 0; attempt < 100 && s_logging && !s_log_opened; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void stop_logging(void)
{
    /* Wake the task if it is blocked on data-ready, then wait until its final
     * fflush/fclose has made the file safe for transfer or power removal. */
    s_logging = false;
    TaskHandle_t task = s_logger_task;
    if (task != NULL) xTaskNotifyGive(task);
    while (s_logger_task != NULL) vTaskDelay(pdMS_TO_TICKS(10));
}

static void print_status(void)
{
    struct tm now = {0};
    bool time_valid = rtc_read_valid(&now);
    char rtc_text[24] = "invalid";
    if (time_valid) {
        strftime(rtc_text, sizeof(rtc_text), "%Y-%m-%dT%H:%M:%S", &now);
    }
    printf("STATUS logging=%d sd_mounted=%d path=%s samples=%" PRIu32
           " interrupts=%" PRIu32
           " missed=%" PRIu32 " invalid=%" PRIu32 " i2c_errors=%" PRIu32
           " write_errors=%" PRIu32 " timeouts=%" PRIu32 " flushes=%" PRIu32
           " rtc_available=%d rtc=%s\n",
           s_logging ? 1 : 0, s_sd_mounted ? 1 : 0,
           s_current_log_path, s_stats.samples, s_stats.interrupts,
           s_stats.missed_samples, s_stats.invalid_samples, s_stats.i2c_errors,
           s_stats.write_errors, s_stats.wait_timeouts, s_stats.flushes,
           s_rtc_available ? 1 : 0, rtc_text);
}

static bool valid_log_name(const char *name)
{
    if (name == NULL) return false;
    size_t length = strlen(name);
    if (length == 18 && strncmp(name, "imu_run_", 8) == 0 &&
        strcmp(name + 14, ".csv") == 0) {
        for (size_t index = 8; index < 14; index++) {
            if (name[index] < '0' || name[index] > '9') return false;
        }
        return true;
    }
    if (length != 26 || strncmp(name, "imu_", 4) != 0 || name[12] != '_' ||
        name[19] != '_' || strcmp(name + 22, ".csv") != 0) return false;
    for (size_t index = 4; index < 22; index++) {
        if (index == 12 || index == 19) continue;
        if (name[index] < '0' || name[index] > '9') return false;
    }
    return true;
}

static bool parse_rtc_time(const char *text, struct tm *value)
{
    int year, month, day, hour, minute, second;
    if (text == NULL || value == NULL ||
        sscanf(text, "%d-%d-%d %d:%d:%d", &year, &month, &day,
               &hour, &minute, &second) != 6) return false;
    memset(value, 0, sizeof(*value));
    value->tm_year = year - 1900;
    value->tm_mon = month - 1;
    value->tm_mday = day;
    value->tm_hour = hour;
    value->tm_min = minute;
    value->tm_sec = second;
    if (!calendar_valid(value)) return false;
    int64_t days = calendar_epoch_seconds(value) / 86400;
    value->tm_wday = (int)((days + 4) % 7);
    if (value->tm_wday < 0) value->tm_wday += 7;
    return true;
}

static void print_rtc_time(void)
{
    struct tm now = {0};
    if (!rtc_read_valid(&now)) {
        printf("ERR RTC_GET unavailable_or_invalid\n");
        return;
    }
    printf("RTC %04d-%02d-%02d %02d:%02d:%02d\n",
           now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
           now.tm_hour, now.tm_min, now.tm_sec);
}

static void set_rtc_time(const char *text)
{
    struct tm target = {0};
    if (!s_rtc_available) {
        printf("ERR RTC_SET unavailable\n");
        return;
    }
    if (!parse_rtc_time(text, &target)) {
        printf("ERR RTC_SET format=YYYY-MM-DD_HH:MM:SS\n");
        return;
    }
    xSemaphoreTake(s_rtc_mutex, portMAX_DELAY);
    esp_err_t error = rtc_set_time(&s_rtc, &target);
    if (error == ESP_OK) error = rtc_clear_oscillator_stop_flag(&s_rtc);
    xSemaphoreGive(s_rtc_mutex);
    if (error != ESP_OK) {
        printf("ERR RTC_SET %s\n", esp_err_to_name(error));
        return;
    }
    s_rtc_valid = true;
    printf("OK RTC_SET %04d-%02d-%02d %02d:%02d:%02d\n",
           target.tm_year + 1900, target.tm_mon + 1, target.tm_mday,
           target.tm_hour, target.tm_min, target.tm_sec);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    crc = ~crc;
    for (size_t index = 0; index < length; index++) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static bool wait_for_usb_tx(void)
{
    for (unsigned attempt = 0; attempt < USB_TX_DRAIN_RETRIES; attempt++) {
        if (usb_serial_jtag_wait_tx_done(
                pdMS_TO_TICKS(USB_TX_DRAIN_TIMEOUT_MS)) == ESP_OK) {
            return true;
        }
        /* A busy Windows host can pause reads briefly. Keep the transfer alive
         * instead of treating one back-pressure timeout as a fatal error. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static void list_logs(void)
{
    if (!s_sd_mounted || s_logging) {
        printf("ERR LOG_LIST %s\n", s_logging ? "logging_active" : "sd_unavailable");
        return;
    }
    DIR *directory = opendir(SD_CAPTURE_DIR);
    if (directory == NULL) {
        printf("ERR LOG_LIST open_dir_failed\n");
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!valid_log_name(entry->d_name)) continue;
        char path[LOG_PATH_SIZE];
        int written = snprintf(path, sizeof(path), SD_CAPTURE_DIR "/%s", entry->d_name);
        struct stat info;
        if (written > 0 && (size_t)written < sizeof(path) && stat(path, &info) == 0) {
            printf("LOG %s %u\n", entry->d_name, (unsigned)info.st_size);
        }
    }
    closedir(directory);
    printf("OK LOG_LIST\n");
}

static void delete_all_logs(void)
{
    if (!s_sd_mounted || s_logging) {
        printf("ERR LOG_DELETE_ALL %s\n",
               s_logging ? "logging_active" : "sd_unavailable");
        return;
    }
    DIR *directory = opendir(SD_CAPTURE_DIR);
    if (directory == NULL) {
        printf("ERR LOG_DELETE_ALL open_dir_failed\n");
        return;
    }

    uint32_t deleted = 0;
    uint32_t failed = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!valid_log_name(entry->d_name)) continue;
        char path[LOG_PATH_SIZE];
        int written = snprintf(path, sizeof(path), SD_CAPTURE_DIR "/%s", entry->d_name);
        if (written <= 0 || (size_t)written >= sizeof(path) || remove(path) != 0) {
            failed++;
        } else {
            deleted++;
        }
    }
    closedir(directory);
    printf("OK LOG_DELETE_ALL deleted=%" PRIu32 " failed=%" PRIu32 "\n",
           deleted, failed);
}

static void send_log(const char *name)
{
    /* The line-framed protocol lets the PC distinguish control messages from
     * CSV payload while byte count and CRC detect truncated USB transfers. */
    if (!s_sd_mounted || s_logging) {
        printf("ERR LOG_GET %s\n", s_logging ? "logging_active" : "sd_unavailable");
        return;
    }
    if (!valid_log_name(name)) {
        printf("ERR LOG_GET invalid_name\n");
        return;
    }
    char path[LOG_PATH_SIZE];
    int written = snprintf(path, sizeof(path), SD_CAPTURE_DIR "/%s", name);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        printf("ERR LOG_GET path_too_long\n");
        return;
    }
    struct stat info;
    FILE *file = fopen(path, "rb");
    if (file == NULL || stat(path, &info) != 0) {
        if (file != NULL) fclose(file);
        printf("ERR LOG_GET open_failed\n");
        return;
    }

    printf("BEGIN_LOG %s %u\n", name, (unsigned)info.st_size);
    fflush(stdout);
    if (!wait_for_usb_tx()) {
        fclose(file);
        printf("ERR LOG_GET serial_timeout\n");
        return;
    }
    char csv_line[256];
    uint32_t bytes_sent = 0;
    uint32_t crc = 0;
    uint32_t lines_sent = 0;
    while (fgets(csv_line, sizeof(csv_line), file) != NULL) {
        size_t length = strlen(csv_line);
        crc = crc32_update(crc, (const uint8_t *)csv_line, length);
        bytes_sent += (uint32_t)length;
        while (length > 0 && (csv_line[length - 1] == '\n' || csv_line[length - 1] == '\r')) {
            csv_line[--length] = '\0';
        }
        printf("CSV %s\n", csv_line);
        lines_sent++;
        if ((lines_sent % 32U) == 0U) {
            fflush(stdout);
            if (!wait_for_usb_tx()) {
                fclose(file);
                printf("ERR LOG_GET serial_timeout\n");
                return;
            }
        }
    }
    bool read_error = ferror(file);
    fclose(file);
    if (read_error) {
        printf("ERR LOG_GET read_failed\n");
        return;
    }
    printf("END_LOG %s %" PRIu32 " %08" PRIx32 "\n", name, bytes_sent, crc);
    fflush(stdout);
    (void)wait_for_usb_tx();
}

static void command_task(void *arg)
{
    /* All state-changing serial commands execute in this task. The logger task
     * owns the open FILE, so commands stop it before listing or reading logs. */
    (void)arg;
    char line[64];
    printf("READY BMI323 address=0x%02x runs=%s\n", s_imu_address, SD_CAPTURE_DIR);
    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (strncasecmp(line, "IMU_LOG_START", 13) == 0) {
            if (!s_sd_mounted) {
                printf("ERR IMU_LOG_START SD_NOT_AVAILABLE\n");
            } else if (s_logging) {
                printf("OK IMU_LOG_START already_logging=1 path=%s\n", s_current_log_path);
            } else {
                start_logging();
                if (s_logging) {
                    printf("OK IMU_LOG_START path=%s\n", s_current_log_path);
                } else {
                    printf("ERR IMU_LOG_START task_failed\n");
                }
            }
        } else if (strncasecmp(line, "IMU_LOG_STOP", 12) == 0) {
            stop_logging();
            printf("OK IMU_LOG_STOP path=%s samples=%" PRIu32 "\n",
                   s_current_log_path, s_stats.samples);
        } else if (strncasecmp(line, "LOG_LIST", 8) == 0) {
            list_logs();
        } else if (strncasecmp(line, "LOG_DELETE_ALL", 14) == 0) {
            delete_all_logs();
        } else if (strncasecmp(line, "LOG_GET ", 8) == 0) {
            char *name = line + 8;
            name[strcspn(name, "\r\n ")] = '\0';
            send_log(name);
        } else if (strncasecmp(line, "STATUS", 6) == 0) {
            print_status();
        } else if (strncasecmp(line, "RTC_GET", 7) == 0) {
            print_rtc_time();
        } else if (strncasecmp(line, "RTC_SET ", 8) == 0) {
            char *value = line + 8;
            value[strcspn(value, "\r\n")] = '\0';
            set_rtc_time(value);
        } else {
            printf("ERR unknown command; use IMU_LOG_START, IMU_LOG_STOP, "
                   "LOG_LIST, LOG_GET <name>, LOG_DELETE_ALL, RTC_GET, "
                   "RTC_SET <YYYY-MM-DD HH:MM:SS>, STATUS\n");
        }
    }
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb_serial_config = {
        .tx_buffer_size = 8192,
        .rx_buffer_size = 1024,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_serial_config));
    usb_serial_jtag_vfs_use_driver();

    s_i2c_mutex = xSemaphoreCreateMutex();
    s_rtc_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_i2c_mutex != NULL && s_rtc_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(imu_init());
    ESP_ERROR_CHECK(imu_data_ready_init());
    rtc_initialize();
    esp_err_t sd_error = sd_init();
    if (sd_error == ESP_OK) {
        start_logging();
    } else {
        ESP_LOGE(TAG, "SD unavailable: %s; IMU remains available but logging is disabled",
                 esp_err_to_name(sd_error));
    }
    xTaskCreate(command_task, "commands", 3072, NULL, 3, NULL);
}
