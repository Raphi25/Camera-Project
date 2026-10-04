/* Minimal BMI323 driver for capture-time gravity-vector logging. */

#include "imu_orientation.h"

#include <stdlib.h>
#include <string.h>

#include "device_settings.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu_orientation";

#define BMI323_ADDRESS_OPEN_PAD  0x69
#define BMI323_ADDRESS_SHORT_PAD 0x68
#define BMI323_REG_CHIP_ID        0x00
#define BMI323_REG_ACC_DATA_X     0x03
#define BMI323_REG_ACC_CONF       0x20
#define BMI323_CHIP_ID            0x43
#define BMI323_I2C_DUMMY_BYTES    2
#define BMI323_RW_TIMEOUT_MS      100

/* ACC_CONF: 100 Hz, +/-2 g, ODR/4 bandwidth, 8-sample averaging, normal mode.
 * The resulting gravity vector is stable while still following posture changes
 * quickly enough for capture-time metadata. */
static const uint8_t BMI323_ACCEL_CONFIG[2] = {0x88, 0x43};

static esp_err_t read_registers(imu_orientation_t *imu, uint8_t reg,
                                uint8_t *data, size_t size)
{
    if (imu == NULL || imu->dev == NULL || data == NULL || size == 0 || size > 8) {
        return ESP_ERR_INVALID_ARG;
    }

    /* BMI323's I2C protocol returns two dummy bytes before register data.
     * Request and discard them, matching Bosch's BMI3 SensorAPI framing. */
    uint8_t response[8 + BMI323_I2C_DUMMY_BYTES] = {0};
    esp_err_t err = i2c_master_transmit_receive(
        imu->dev, &reg, 1, response, size + BMI323_I2C_DUMMY_BYTES,
        BMI323_RW_TIMEOUT_MS);
    if (err == ESP_OK) {
        memcpy(data, &response[BMI323_I2C_DUMMY_BYTES], size);
    }
    return err;
}

static esp_err_t write_registers(imu_orientation_t *imu, uint8_t reg,
                                 const uint8_t *data, size_t size)
{
    if (imu == NULL || imu->dev == NULL || data == NULL || size == 0 || size > 8) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buffer[9] = {reg};
    memcpy(&buffer[1], data, size);
    return i2c_master_transmit(imu->dev, buffer, size + 1, BMI323_RW_TIMEOUT_MS);
}

static int16_t little_endian_i16(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static int16_t raw_to_mg(int16_t raw)
{
    return (int16_t)(((int32_t)raw * 2000) / 32768);
}

static int16_t mapped_axis(const int16_t sensor_mg[3], int axis, int sign)
{
    if (axis < 0 || axis > 2) return 0;
    return (int16_t)(sensor_mg[axis] * (sign < 0 ? -1 : 1));
}

static const char *classify_orientation(int16_t x_mg, int16_t y_mg, int16_t z_mg)
{
    const int32_t ax = abs((int)x_mg);
    const int32_t ay = abs((int)y_mg);
    const int32_t az = abs((int)z_mg);
    const int32_t magnitude_squared = ax * ax + ay * ay + az * az;
    int32_t dominant = ax;

    /* A stationary gravity vector should be close to 1 g. Avoid assigning a
     * confident posture during strong movement or an obviously bad sample. */
    if (magnitude_squared < (650 * 650) || magnitude_squared > (1350 * 1350)) {
        return "moving or indeterminate";
    }
    if (ay > dominant) dominant = ay;
    if (az > dominant) dominant = az;
    if (dominant < IMU_ORIENTATION_CARDINAL_THRESHOLD_MG) {
        return "tilted or moving";
    }

    if (dominant == ay) {
        return y_mg >= 0 ? "upright facing forwards"
                         : "upside down facing forwards";
    }
    if (dominant == az) {
        return z_mg >= 0 ? "laying down facing upwards"
                         : "laying down facing downwards";
    }
    return x_mg >= 0 ? "laying on its left side"
                     : "laying on its right side";
}

static esp_err_t try_address(imu_orientation_t *imu, uint8_t address,
                             uint32_t i2c_speed_hz)
{
    esp_err_t err = i2c_master_probe(imu->bus, address, BMI323_RW_TIMEOUT_MS);
    if (err != ESP_OK) return err;

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = i2c_speed_hz,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(imu->bus, &device_config, &imu->dev),
                        TAG, "add BMI323 I2C device");

    uint8_t chip_id[2] = {0};
    err = read_registers(imu, BMI323_REG_CHIP_ID, chip_id, sizeof(chip_id));
    if (err != ESP_OK || chip_id[0] != BMI323_CHIP_ID) {
        ESP_LOGW(TAG, "I2C device 0x%02x is not BMI323 (chip ID 0x%02x)",
                 address, chip_id[0]);
        i2c_master_bus_rm_device(imu->dev);
        imu->dev = NULL;
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }

    imu->address = address;
    return ESP_OK;
}

esp_err_t imu_orientation_init(imu_orientation_t *imu,
                               i2c_port_t i2c_port,
                               gpio_num_t sda_gpio,
                               gpio_num_t scl_gpio,
                               uint32_t i2c_speed_hz)
{
    if (imu == NULL || sda_gpio == GPIO_NUM_NC || scl_gpio == GPIO_NUM_NC ||
        i2c_speed_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(imu, 0, sizeof(*imu));

    i2c_master_bus_config_t bus_config = {
        .i2c_port = i2c_port,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &imu->bus),
                        TAG, "create BMI323 I2C bus");

    esp_err_t err = try_address(imu, BMI323_ADDRESS_OPEN_PAD, i2c_speed_hz);
    if (err != ESP_OK) {
        err = try_address(imu, BMI323_ADDRESS_SHORT_PAD, i2c_speed_hz);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BMI323 not found at 0x69 or 0x68");
        i2c_del_master_bus(imu->bus);
        imu->bus = NULL;
        return err;
    }

    err = write_registers(imu, BMI323_REG_ACC_CONF, BMI323_ACCEL_CONFIG,
                          sizeof(BMI323_ACCEL_CONFIG));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Configure BMI323 accelerometer failed: %s",
                 esp_err_to_name(err));
        imu_orientation_deinit(imu);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    imu->ready = true;
    ESP_LOGI(TAG, "BMI323 ready at I2C address 0x%02x", imu->address);
    return ESP_OK;
}

void imu_orientation_deinit(imu_orientation_t *imu)
{
    if (imu == NULL) return;
    if (imu->dev != NULL) {
        i2c_master_bus_rm_device(imu->dev);
    }
    if (imu->bus != NULL) {
        i2c_del_master_bus(imu->bus);
    }
    memset(imu, 0, sizeof(*imu));
}

esp_err_t imu_orientation_read(imu_orientation_t *imu,
                               imu_orientation_sample_t *sample)
{
    if (sample == NULL) return ESP_ERR_INVALID_ARG;
    memset(sample, 0, sizeof(*sample));
    strlcpy(sample->status, "IMU status unavailable", sizeof(sample->status));
    if (imu == NULL || !imu->ready) return ESP_ERR_INVALID_STATE;

    uint8_t data[6] = {0};
    ESP_RETURN_ON_ERROR(read_registers(imu, BMI323_REG_ACC_DATA_X,
                                       data, sizeof(data)),
                        TAG, "read BMI323 acceleration");
    int16_t raw[3] = {
        little_endian_i16(&data[0]),
        little_endian_i16(&data[2]),
        little_endian_i16(&data[4]),
    };
    if (raw[0] == INT16_MIN || raw[1] == INT16_MIN || raw[2] == INT16_MIN) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    int16_t sensor_mg[3] = {
        raw_to_mg(raw[0]), raw_to_mg(raw[1]), raw_to_mg(raw[2]),
    };
    sample->x_mg = mapped_axis(sensor_mg, IMU_DEVICE_X_SENSOR_AXIS,
                               IMU_DEVICE_X_SENSOR_SIGN);
    sample->y_mg = mapped_axis(sensor_mg, IMU_DEVICE_Y_SENSOR_AXIS,
                               IMU_DEVICE_Y_SENSOR_SIGN);
    sample->z_mg = mapped_axis(sensor_mg, IMU_DEVICE_Z_SENSOR_AXIS,
                               IMU_DEVICE_Z_SENSOR_SIGN);
    strlcpy(sample->status,
            classify_orientation(sample->x_mg, sample->y_mg, sample->z_mg),
            sizeof(sample->status));
    sample->valid = true;
    return ESP_OK;
}
